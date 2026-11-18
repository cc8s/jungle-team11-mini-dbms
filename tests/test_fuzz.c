#include "test.h"
#include "test_util.h"

#include "json.h"
#include "tokenizer.h"

/* 무작위 입력 퍼징 (시드 고정). 크래시·메모리 오류가 없는지가 목적이라 make test-asan 에서 의미가 크다.
 * 입력은 정확한 길이로 malloc 하고 끝에 \0 을 두지 않는다: 길이 밖을 읽으면 ASan 이 잡는다.
 * 절반은 순수 무작위 바이트 (첫 토큰에서 대부분 끝난다), 절반은 올바른 입력을 조금씩 망가뜨린 것
 * (파서·검증·실행 깊은 곳까지 들어간다). */

#define FUZZ_SEED 0x5eed2026ULL
#define FUZZ_CASES 10000
#define FUZZ_MAX_RANDOM_LEN 256
#define FUZZ_MAX_MUTATIONS 8
#define FUZZ_MUTATION_HEADROOM (FUZZ_MAX_MUTATIONS * 16) /* 삽입으로 늘어날 수 있는 바이트 */
#define BYTE_VALUES 256
#define MUTATION_KINDS 4
#define USERS_SQL "CREATE TABLE users (id INT, name VARCHAR(8), age INT);"

static const char *const SQL_SEEDS[] = {
    "INSERT INTO users VALUES ('kim', 25);",
    "SELECT id, name FROM users WHERE id >= 3 LIMIT 2;",
    "SELECT * FROM users WHERE name = 'it''s' -- comment\n;",
    "SELECT * FROM users WHERE age != -2147483648;",
    "INSERT INTO users VALUES (?, ?); SELECT * FROM users;",
    "CREATE TABLE t (id INT, n VARCHAR(4));",
};

static const char *const JSON_SEEDS[] = {
    "{\"sql\":\"SELECT * FROM users\",\"params\":[1,\"a\"]}",
    "{\"params\":[-2147483648,\"\\u00e9\\ud83d\\ude00\"],"
    "\"sql\":\"INSERT INTO users VALUES (?, ?)\"}",
    "{\"sql\":\"x\",\"extra\":{\"a\":[1,[2,[3]]],\"b\":null,\"c\":true}}",
    "{\"sql\":\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"}",
    "  {\"sql\" : \"SELECT id FROM users WHERE id = ?\" , \"params\" : [ 7 ] }  ",
};

/* 문법을 깨거나 경계를 건드리는 조각 */
static const char *const FRAGMENTS[] = {
    "'", "''", "--", "?", ";", "(", ")", "\"", "\\", "\\u", "{", "[", ",", ":",
    "99999999999", "-", "*", "VARCHAR(", "WHERE", "\n", "\xff", "\xc3",
};

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

typedef struct {
    char *bytes;
    size_t len;
} FuzzInput;

static FuzzInput random_input(TestRng *rng)
{
    FuzzInput input = { .len = test_rng_below(rng, FUZZ_MAX_RANDOM_LEN + 1) };
    input.bytes = malloc(input.len > 0 ? input.len : 1);
    CHECK(input.bytes != NULL);
    for (size_t i = 0; i < input.len; i++)
        input.bytes[i] = (char)test_rng_below(rng, BYTE_VALUES);
    return input;
}

/* at 자리에 text 를 끼워 넣는다 (buf 에는 len + strlen(text) 이상의 공간이 있다) */
static void insert_text(char *buf, size_t *len, size_t at, const char *text)
{
    size_t text_len = strlen(text);
    memmove(buf + at + text_len, buf + at, *len - at);
    memcpy(buf + at, text, text_len);
    *len += text_len;
}

static void mutate_once(TestRng *rng, char *buf, size_t *len)
{
    size_t at = test_rng_below(rng, *len + 1);
    switch (test_rng_below(rng, MUTATION_KINDS)) {
    case 0: /* 바이트 바꾸기 */
        if (at < *len)
            buf[at] = (char)test_rng_below(rng, BYTE_VALUES);
        break;
    case 1: /* 바이트 지우기 */
        if (at < *len) {
            memmove(buf + at, buf + at + 1, *len - at - 1);
            (*len)--;
        }
        break;
    case 2: /* 조각 끼워 넣기 */
        insert_text(buf, len, at, FRAGMENTS[test_rng_below(rng, COUNT_OF(FRAGMENTS))]);
        break;
    default: /* 뒷부분 자르기 */
        *len = at;
        break;
    }
}

static FuzzInput mutated_input(TestRng *rng, const char *const *seeds, size_t seed_count)
{
    const char *seed = seeds[test_rng_below(rng, seed_count)];
    char work[FUZZ_MAX_RANDOM_LEN + FUZZ_MUTATION_HEADROOM];
    size_t len = strlen(seed);
    CHECK(len + FUZZ_MUTATION_HEADROOM <= sizeof(work));
    memcpy(work, seed, len);
    size_t mutations = 1 + test_rng_below(rng, FUZZ_MAX_MUTATIONS);
    for (size_t i = 0; i < mutations; i++)
        mutate_once(rng, work, &len);
    FuzzInput input = { .bytes = malloc(len > 0 ? len : 1), .len = len };
    CHECK(input.bytes != NULL);
    memcpy(input.bytes, work, len);
    return input;
}

static FuzzInput next_input(TestRng *rng, int case_index, const char *const *seeds,
                            size_t seed_count)
{
    return case_index % 2 == 0 ? random_input(rng) : mutated_input(rng, seeds, seed_count);
}

/* 엔진 API 는 \0 으로 끝나는 문자열을 받으므로 따로 복사한다 */
static char *to_c_string(const FuzzInput *input)
{
    char *text = malloc(input->len + 1);
    CHECK(text != NULL);
    memcpy(text, input->bytes, input->len);
    text[input->len] = '\0';
    return text;
}

static bool is_valid_error(ErrorCode error)
{
    return error >= ERR_NONE && error <= ERR_INTERNAL;
}

/* ---- SQL ---- */

/* 토큰은 매번 한 글자 이상 먹거나 EOF/에러로 끝나야 한다 (무한 루프 없음) */
static void fuzz_tokenizer(const FuzzInput *input)
{
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, input->bytes, input->len);
    Token token;
    for (size_t steps = 0; steps <= input->len + 1; steps++) {
        ErrorCode error = tokenizer_next(&tokenizer, &token);
        CHECK(is_valid_error(error));
        if (error != ERR_NONE || token.type == TOK_EOF)
            return;
    }
    CHECK(false); /* 위에서 끝났어야 한다 */
}

static void fuzz_parser(const FuzzInput *input)
{
    Statement stmt;
    ParseError parse_error;
    CHECK(is_valid_error(parser_parse_single(input->bytes, input->len, &stmt, &parse_error)));
    statement_free(&stmt);

    Parser *parser = parser_create(input->bytes, input->len);
    CHECK(parser != NULL);
    ErrorCode error;
    size_t statements = 0;
    while (parser_next_statement(parser, &stmt, &error)) {
        CHECK(is_valid_error(error));
        statement_free(&stmt);
        CHECK(++statements <= input->len + 1); /* 문장마다 한 글자 이상 먹는다 */
    }
    parser_destroy(parser);
}

/* 파서를 통과한 입력은 검증·실행까지 간다 (정상 문장이면 실제로 INSERT 된다). 성공하면 true */
static bool fuzz_engine(Engine *engine, const FuzzInput *input)
{
    char *sql = to_c_string(input);
    Result result = engine_execute(engine, sql, NULL, 0);
    CHECK(is_valid_error(result.error));
    bool is_ok = result.error == ERR_NONE;
    result_free(&result);
    free(sql);
    return is_ok;
}

/* E43-03 */
static void test_fuzz_sql(void)
{
    TestDb db = test_db_create(USERS_SQL);
    CHECK(db.engine != NULL);
    TestRng rng = { .state = FUZZ_SEED };
    int executed = 0;
    for (int i = 0; i < FUZZ_CASES; i++) {
        FuzzInput input = next_input(&rng, i, SQL_SEEDS, COUNT_OF(SQL_SEEDS));
        fuzz_tokenizer(&input);
        fuzz_parser(&input);
        executed += fuzz_engine(db.engine, &input);
        free(input.bytes);
    }
    printf("    executed %d of %d inputs\n", executed, FUZZ_CASES);
    CHECK(executed > 0); /* 망가뜨린 입력 일부는 실행 단계까지 들어갔다 (퍼징이 겉만 돌지 않음) */
    test_db_destroy(&db);
}

/* ---- JSON ---- */

static bool is_valid_json_status(JsonStatus status)
{
    return status == JSON_OK || status == JSON_BAD_REQUEST || status == JSON_BAD_PARAM ||
           status == JSON_NO_MEMORY;
}

/* 성공한 요청은 서버처럼 엔진까지 보낸다 (params 바인딩 경로). JSON 이 올바르면 true */
static bool fuzz_json(Engine *engine, const FuzzInput *input)
{
    QueryRequest request;
    char message[MAX_MESSAGE];
    JsonStatus status = json_parse_query(input->bytes, input->len, &request, message);
    CHECK(is_valid_json_status(status));
    if (status != JSON_OK)
        return false;
    Result result = engine_execute(engine, request.sql, request.params, request.param_count);
    CHECK(is_valid_error(result.error));
    result_free(&result);
    query_request_free(&request);
    return true;
}

/* E43-04 */
static void test_fuzz_json(void)
{
    TestDb db = test_db_create(USERS_SQL);
    CHECK(db.engine != NULL);
    TestRng rng = { .state = FUZZ_SEED + 1 };
    int parsed = 0;
    for (int i = 0; i < FUZZ_CASES; i++) {
        FuzzInput input = next_input(&rng, i, JSON_SEEDS, COUNT_OF(JSON_SEEDS));
        parsed += fuzz_json(db.engine, &input);
        free(input.bytes);
    }
    printf("    parsed %d of %d inputs\n", parsed, FUZZ_CASES);
    CHECK(parsed > 0);
    test_db_destroy(&db);
}

int main(void)
{
    printf("test_fuzz (seed 0x%llx, %d cases each)\n", (unsigned long long)FUZZ_SEED, FUZZ_CASES);
    RUN(test_fuzz_sql);
    RUN(test_fuzz_json);
    TEST_DONE();
    return 0;
}
