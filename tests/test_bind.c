#include "test.h"
#include "test_util.h"

#include "cli.h"
#include "engine_internal.h"

/* Prepared Statement 바인딩: SQL 의 ? 자리에 params 의 값을 넣는다. 값은 토크나이저·파서를 거치지 않고
 * 파싱이 끝난 문장의 Value 자리에 그대로 들어가므로, 따옴표나 SQL 조각이 들어 있어도 데이터일 뿐이다. */

#define USERS_SQL "CREATE TABLE users (id INT, name VARCHAR(8), age INT);"
#define MAX_TEST_PARAMS 4
#define SEED_COUNT 5

static TestDb open_db(void)
{
    TestDb db = test_db_create(USERS_SQL);
    CHECK(db.dir != NULL);
    CHECK(db.engine != NULL);
    return db;
}

/* 테스트가 만든 params 묶음. 엔진은 값을 복사하므로 실행 뒤 바로 해제해도 된다 */
typedef struct {
    Value values[MAX_TEST_PARAMS];
    int count;
} Params;

static void add_int(Params *params, int32_t number)
{
    params->values[params->count++] = value_from_int(number);
}

static void add_text(Params *params, const char *text)
{
    params->values[params->count] = value_from_string(text, strlen(text));
    CHECK(params->values[params->count].s.ptr != NULL);
    params->count++;
}

static void free_params(Params *params)
{
    for (int i = 0; i < params->count; i++)
        value_free(&params->values[i]);
    params->count = 0;
}

/* sql 을 params 로 실행한다. params 는 실행 뒤 해제해서, 엔진이 복사해 갔는지도 함께 본다 */
static Result run_bound(Engine *engine, const char *sql, Params *params)
{
    Result result = engine_execute(engine, sql, params->values, params->count);
    free_params(params);
    return result;
}

static ErrorCode run_bound_error(Engine *engine, const char *sql, Params *params)
{
    Result result = run_bound(engine, sql, params);
    ErrorCode error = result.error;
    CHECK(error == ERR_NONE ? result.message[0] == '\0' : result.message[0] != '\0');
    result_free(&result);
    return error;
}

static int32_t insert_user(Engine *engine, const char *name, int32_t age)
{
    Params params = { 0 };
    add_text(&params, name);
    add_int(&params, age);
    Result result = run_bound(engine, "INSERT INTO users VALUES (?, ?)", &params);
    CHECK(result.error == ERR_NONE);
    CHECK(result.affected_rows == 1);
    int32_t id = result.last_insert_id;
    result_free(&result);
    return id;
}

/* 0 번째 행의 name 칸이 expected 와 바이트 단위로 같은지 */
static void check_name(const Result *result, int64_t row, const char *expected)
{
    size_t len = 0;
    const char *name = result_get_str(result, row, 1, &len);
    CHECK(len == strlen(expected));
    CHECK(memcmp(name, expected, len) == 0);
}

static int64_t user_count(Engine *engine)
{
    Result result = engine_execute(engine, "SELECT * FROM users", NULL, 0);
    CHECK(result.error == ERR_NONE);
    int64_t count = result.row_count;
    result_free(&result);
    return count;
}

/* ---- 정상 바인딩 ---- */

/* T42-01 */
static void test_insert_with_params(void)
{
    TestDb db = open_db();
    CHECK(insert_user(db.engine, "kim", 25) == 1);

    Result result = engine_execute(db.engine, "SELECT * FROM users", NULL, 0);
    CHECK(result.error == ERR_NONE);
    CHECK(result.row_count == 1);
    check_name(&result, 0, "kim");
    CHECK(result_get_int(&result, 0, 2) == 25);
    result_free(&result);
    test_db_destroy(&db);
}

static void seed_users(Engine *engine)
{
    const char *names[SEED_COUNT] = { "kim", "lee", "park", "kim", "choi" };
    for (int i = 0; i < SEED_COUNT; i++)
        CHECK(insert_user(engine, names[i], 20 + i) == i + 1);
}

/* T42-02 */
static void test_select_by_id_param_uses_index(void)
{
    TestDb db = open_db();
    seed_users(db.engine);
    Params params = { 0 };
    add_int(&params, 3);
    Result result = run_bound(db.engine, "SELECT * FROM users WHERE id = ?", &params);
    CHECK(result.error == ERR_NONE);
    CHECK(result.row_count == 1);
    CHECK(result.used_index);
    CHECK(result_get_int(&result, 0, 0) == 3);
    check_name(&result, 0, "park");
    result_free(&result);
    test_db_destroy(&db);
}

/* T42-03 */
static void test_select_by_name_param(void)
{
    TestDb db = open_db();
    seed_users(db.engine);
    Params params = { 0 };
    add_text(&params, "kim");
    Result result = run_bound(db.engine, "SELECT id FROM users WHERE name = ?", &params);
    CHECK(result.error == ERR_NONE);
    CHECK(result.row_count == 2);
    CHECK(!result.used_index);
    CHECK(result_get_int(&result, 0, 0) == 1);
    CHECK(result_get_int(&result, 1, 0) == 4);
    result_free(&result);
    test_db_destroy(&db);
}

/* T42-04: 따옴표, ?, ; 가 든 값도 이스케이프 없이 그대로 저장되고 그대로 찾아진다 */
static void test_quote_in_param_is_plain_data(void)
{
    TestDb db = open_db();
    const char *tricky[] = { "it's", "?", "a;b", "'--" };
    for (int i = 0; i < (int)(sizeof(tricky) / sizeof(tricky[0])); i++) {
        int32_t id = insert_user(db.engine, tricky[i], 30);
        Params params = { 0 };
        add_text(&params, tricky[i]);
        Result result = run_bound(db.engine, "SELECT * FROM users WHERE name = ?", &params);
        CHECK(result.error == ERR_NONE);
        CHECK(result.row_count == 1);
        CHECK(result_get_int(&result, 0, 0) == id);
        check_name(&result, 0, tricky[i]);
        result_free(&result);
    }
    CHECK(user_count(db.engine) == 4);
    test_db_destroy(&db);
}

/* ? 가 없는 문장은 params 없이 그대로 돈다 (바인딩은 할 일이 없다) */
static void test_no_params_still_works(void)
{
    TestDb db = open_db();
    Result result = engine_execute(db.engine, "INSERT INTO users VALUES ('lee', 30)", NULL, 0);
    CHECK(result.error == ERR_NONE);
    result_free(&result);
    CHECK(user_count(db.engine) == 1);
    test_db_destroy(&db);
}

/* ---- 개수·타입·길이 ---- */

/* E42-01, E42-02: 개수가 맞지 않으면 실행하지 않는다 (락도 잡지 않는다) */
static void test_param_count_mismatch(void)
{
    TestDb db = open_db();
    Params params = { 0 };
    add_text(&params, "kim");
    CHECK(run_bound_error(db.engine, "INSERT INTO users VALUES (?, ?)", &params) == ERR_PARAM);
    add_text(&params, "kim");
    add_int(&params, 25);
    add_int(&params, 26);
    CHECK(run_bound_error(db.engine, "INSERT INTO users VALUES (?, ?)", &params) == ERR_PARAM);
    CHECK(run_bound_error(db.engine, "INSERT INTO users VALUES (?, ?)", &params) == ERR_PARAM);

    add_int(&params, 1); /* E42-02 */
    CHECK(run_bound_error(db.engine, "SELECT * FROM users", &params) == ERR_PARAM);
    CHECK(engine_lock_acquire_count(db.engine) == 0);
    CHECK(user_count(db.engine) == 0);
    test_db_destroy(&db);
}

/* E42-03: 바인딩한 값도 암묵 변환 없이 컬럼 타입과 정확히 맞아야 한다 */
static void test_param_type_mismatch(void)
{
    TestDb db = open_db();
    Params params = { 0 };
    add_text(&params, "kim");
    add_text(&params, "25"); /* INT 자리에 문자열 */
    CHECK(run_bound_error(db.engine, "INSERT INTO users VALUES (?, ?)", &params) ==
          ERR_TYPE_MISMATCH);
    add_int(&params, 7); /* VARCHAR 자리에 정수 */
    add_int(&params, 25);
    CHECK(run_bound_error(db.engine, "INSERT INTO users VALUES (?, ?)", &params) ==
          ERR_TYPE_MISMATCH);
    add_text(&params, "3");
    CHECK(run_bound_error(db.engine, "SELECT * FROM users WHERE id = ?", &params) ==
          ERR_TYPE_MISMATCH);
    CHECK(user_count(db.engine) == 0);
    test_db_destroy(&db);
}

/* E42-04: VARCHAR(8) 에 8 바이트는 들어가고 9 바이트는 자르지 않고 거절 */
static void test_param_too_long(void)
{
    TestDb db = open_db();
    CHECK(insert_user(db.engine, "abcdefgh", 1) == 1);
    Params params = { 0 };
    add_text(&params, "abcdefghi");
    add_int(&params, 1);
    CHECK(run_bound_error(db.engine, "INSERT INTO users VALUES (?, ?)", &params) ==
          ERR_VALUE_TOO_LONG);
    CHECK(user_count(db.engine) == 1);
    test_db_destroy(&db);
}

/* VARCHAR 는 \0 으로 패딩되므로 \0 이 든 값은 저장하면 잘려 보인다. 바인딩에서 거절한다 */
static void test_param_with_nul_byte(void)
{
    TestDb db = open_db();
    Params params = { 0 };
    params.values[params.count++] = value_from_string("a\0b", 3);
    add_int(&params, 1);
    CHECK(run_bound_error(db.engine, "INSERT INTO users VALUES (?, ?)", &params) == ERR_PARAM);
    CHECK(user_count(db.engine) == 0);
    test_db_destroy(&db);
}

/* ---- 값 자리에만 ---- */

/* E42-05, E42-06, E42-07: 이름 자리와 LIMIT 자리의 ? 는 문법 에러 (값 자리에만 허용) */
static void test_placeholder_outside_value_position(void)
{
    TestDb db = open_db();
    Params params = { 0 };
    add_text(&params, "users");
    CHECK(run_bound_error(db.engine, "SELECT * FROM ?", &params) == ERR_SYNTAX);
    add_text(&params, "users");
    CHECK(run_bound_error(db.engine, "INSERT INTO ? VALUES ('kim', 1)", &params) == ERR_SYNTAX);
    add_text(&params, "name"); /* E42-06 */
    CHECK(run_bound_error(db.engine, "SELECT ? FROM users", &params) == ERR_SYNTAX);
    add_text(&params, "name");
    CHECK(run_bound_error(db.engine, "SELECT * FROM users WHERE ? = 'kim'", &params) ==
          ERR_SYNTAX);
    add_int(&params, 1); /* E42-07 */
    CHECK(run_bound_error(db.engine, "SELECT * FROM users LIMIT ?", &params) == ERR_SYNTAX);
    test_db_destroy(&db);
}

/* E42-08: API 는 요청당 한 문장. 두 번째 문장이 있으면 아무것도 실행하지 않는다 */
static void test_two_statements_rejected(void)
{
    TestDb db = open_db();
    Params params = { 0 };
    CHECK(run_bound_error(db.engine, "SELECT * FROM users; SELECT * FROM users", &params) ==
          ERR_UNSUPPORTED);
    add_text(&params, "kim");
    add_int(&params, 1);
    CHECK(run_bound_error(db.engine,
                          "INSERT INTO users VALUES (?, ?); INSERT INTO users VALUES ('x', 2)",
                          &params) == ERR_UNSUPPORTED);
    CHECK(user_count(db.engine) == 0);
    test_db_destroy(&db);
}

/* CLI 스크립트 경로에는 바인딩이 없다: ? 는 그대로 ERR_PARAM */
static void count_errors(const Result *result, int stmt_index, void *ctx)
{
    (void)stmt_index;
    if (result->error == ERR_PARAM)
        (*(int *)ctx)++;
}

static void test_script_placeholder_is_param_error(void)
{
    TestDb db = open_db();
    int param_errors = 0;
    CHECK(engine_execute_script(db.engine, "INSERT INTO users VALUES (?, 1);", count_errors,
                                &param_errors) == 1);
    CHECK(param_errors == 1);
    CHECK(user_count(db.engine) == 0);
    test_db_destroy(&db);
}

/* ==== S43 인젝션 시연과 큰 입력 ==== */

/* 이름 검색 입력칸에 공격자가 넣는 값. 이어 붙이면 '...' 를 닫고 INSERT 를 덧붙인 뒤 남은 ' 를 주석으로 지운다 */
#define ATTACK "kim'; INSERT INTO users VALUES ('hacker', 99); --"
#define CONCAT_PREFIX "SELECT * FROM users WHERE name = '"
#define CONCAT_SUFFIX "'"
#define HUGE_LEN 65536
#define QUOTE_COUNT 10000
#define CLI_EXIT_OK 0

/* 개발자가 문자열 이어 붙이기로 만든 SQL (하면 안 되는 방식) */
static char *concat_query(const char *input)
{
    size_t len = strlen(CONCAT_PREFIX) + strlen(input) + strlen(CONCAT_SUFFIX);
    char *sql = malloc(len + 1);
    CHECK(sql != NULL);
    snprintf(sql, len + 1, "%s%s%s", CONCAT_PREFIX, input, CONCAT_SUFFIX);
    return sql;
}

static int64_t count_named(Engine *engine, const char *name)
{
    Params params = { 0 };
    add_text(&params, name);
    Result result = run_bound(engine, "SELECT * FROM users WHERE name = ?", &params);
    CHECK(result.error == ERR_NONE);
    int64_t count = result.row_count;
    result_free(&result);
    return count;
}

/* T43-01: 바인딩하면 공격 문자열 전체가 이름 하나다. 그런 이름은 없으므로 0 행, 삽입 없음 */
static void test_injection_with_binding_is_data(void)
{
    TestDb db = open_db();
    insert_user(db.engine, "kim", 25);
    CHECK(count_named(db.engine, ATTACK) == 0); /* 'kim' 이 있어도 'kim' 으로 끊기지 않는다 */
    CHECK(user_count(db.engine) == 1);
    CHECK(count_named(db.engine, "hacker") == 0);
    test_db_destroy(&db);
}

/* T43-02: 이어 붙이면 SQL 이 두 문장이 된다. API 의 한 문장 제한이 2 차 방어선으로 막는다 */
static void test_injection_by_concat_blocked_by_single_statement(void)
{
    TestDb db = open_db();
    insert_user(db.engine, "kim", 25);
    char *sql = concat_query(ATTACK);
    Result result = engine_execute(db.engine, sql, NULL, 0);
    CHECK(result.error == ERR_UNSUPPORTED); /* 첫 SELECT 도 실행되지 않는다 */
    CHECK(result.row_count == 0);
    result_free(&result);
    free(sql);
    CHECK(user_count(db.engine) == 1);
    CHECK(count_named(db.engine, "hacker") == 0);
    test_db_destroy(&db);
}

/* T43-03: CLI 스크립트 모드는 여러 문장을 허용한다. 같은 이어 붙이기가 여기서는 통한다 (대조군) */
static void test_injection_by_concat_succeeds_in_script_mode(void)
{
    TestDb db = open_db();
    insert_user(db.engine, "kim", 25);
    engine_close(db.engine); /* CLI 가 같은 디렉터리를 따로 연다 */
    db.engine = NULL;

    char *sql = concat_query(ATTACK);
    char path[TEST_PATH_MAX];
    CHECK(test_write_file(db.dir, "attack.sql", sql, path));
    free(sql);
    char *argv[] = { "minidb", "-d", db.dir, "-q", path };
    TestCapture out;
    TestCapture err;
    test_capture_open(&out);
    test_capture_open(&err);
    CHECK(cli_run((int)(sizeof(argv) / sizeof(argv[0])), argv, out.stream, err.stream) ==
          CLI_EXIT_OK);
    free(test_capture_close(&out));
    free(test_capture_close(&err));

    test_db_reopen(&db);
    CHECK(db.engine != NULL);
    CHECK(count_named(db.engine, "hacker") == 1); /* 공격이 성공했다: 바인딩을 써야 하는 이유 */
    CHECK(user_count(db.engine) == 2);
    test_db_destroy(&db);
}

/* E43-05: 주석으로 뒷부분을 지우려는 값도 바인딩하면 그 글자 그대로 */
static void test_comment_in_param_is_data(void)
{
    TestDb db = open_db();
    int32_t id = insert_user(db.engine, "'x' --", 1);
    CHECK(count_named(db.engine, "'x' --") == 1);
    Params params = { 0 };
    add_int(&params, id);
    Result result = run_bound(db.engine, "SELECT * FROM users WHERE id = ?", &params);
    CHECK(result.error == ERR_NONE);
    check_name(&result, 0, "'x' --");
    result_free(&result);
    test_db_destroy(&db);
}

/* ch 를 count 번 반복한 문자열 (호출한 쪽이 free) */
static char *repeat_char(char ch, size_t count)
{
    char *text = malloc(count + 1);
    CHECK(text != NULL);
    memset(text, ch, count);
    text[count] = '\0';
    return text;
}

static ErrorCode run_prefixed(Engine *engine, const char *prefix, const char *body)
{
    size_t len = strlen(prefix) + strlen(body);
    char *sql = malloc(len + 1);
    CHECK(sql != NULL);
    snprintf(sql, len + 1, "%s%s", prefix, body);
    Result result = engine_execute(engine, sql, NULL, 0);
    ErrorCode error = result.error;
    result_free(&result);
    free(sql);
    return error;
}

/* E43-01: 64KB 식별자는 식별자 길이 (31 바이트) 검사에서 먼저 걸려 문법 에러. 같은 길이를 문자열
 * 리터럴로 넣으면 문장 크기 제한 (64KB) 에 걸린다. 바인딩 값이면 컬럼 길이 검사에 걸린다 */
static void test_huge_identifier(void)
{
    TestDb db = open_db();
    char *huge = repeat_char('a', HUGE_LEN);
    CHECK(run_prefixed(db.engine, "SELECT * FROM ", huge) == ERR_SYNTAX);
    char *literal = malloc(HUGE_LEN + 3);
    CHECK(literal != NULL);
    snprintf(literal, HUGE_LEN + 3, "'%s'", huge);
    CHECK(run_prefixed(db.engine, "SELECT * FROM users WHERE name = ", literal) == ERR_TOO_LARGE);
    free(literal);
    huge[HUGE_LEN / 2] = '\0'; /* 32KB: 문장 제한 안쪽 */
    CHECK(run_prefixed(db.engine, "SELECT * FROM ", huge) == ERR_SYNTAX);

    CHECK(count_named(db.engine, huge) == 0); /* WHERE 값으로는 그냥 맞는 행이 없다 */
    Params params = { 0 };
    add_text(&params, huge);
    add_int(&params, 1);
    CHECK(run_bound_error(db.engine, "INSERT INTO users VALUES (?, ?)", &params) ==
          ERR_VALUE_TOO_LONG);
    free(huge);
    CHECK(user_count(db.engine) == 0);
    test_db_destroy(&db);
}

/* E43-02: 따옴표 1 만 개. 짝수 개면 '' 이스케이프가 이어진 문자열 하나, 홀수 개면 닫히지 않은 문자열 */
static void test_ten_thousand_quotes(void)
{
    TestDb db = open_db();
    char *quotes = repeat_char('\'', QUOTE_COUNT + 1);
    const char *prefix = "SELECT * FROM users WHERE name = ";
    CHECK(run_prefixed(db.engine, prefix, quotes) == ERR_SYNTAX); /* 홀수: 닫히지 않음 */
    quotes[QUOTE_COUNT] = '\0';
    CHECK(run_prefixed(db.engine, prefix, quotes) == ERR_NONE); /* 짝수: 따옴표 4999 개짜리 값 */
    CHECK(run_prefixed(db.engine, "", quotes) == ERR_SYNTAX);  /* 문장 자리에 따옴표만 */
    CHECK(count_named(db.engine, quotes) == 0);                 /* 바인딩: 따옴표 1 만 개짜리 이름 */
    free(quotes);
    CHECK(user_count(db.engine) == 0);
    test_db_destroy(&db);
}

int main(void)
{
    printf("test_bind\n");
    RUN(test_insert_with_params);
    RUN(test_select_by_id_param_uses_index);
    RUN(test_select_by_name_param);
    RUN(test_quote_in_param_is_plain_data);
    RUN(test_no_params_still_works);
    RUN(test_param_count_mismatch);
    RUN(test_param_type_mismatch);
    RUN(test_param_too_long);
    RUN(test_param_with_nul_byte);
    RUN(test_placeholder_outside_value_position);
    RUN(test_two_statements_rejected);
    RUN(test_script_placeholder_is_param_error);
    RUN(test_injection_with_binding_is_data);
    RUN(test_injection_by_concat_blocked_by_single_statement);
    RUN(test_injection_by_concat_succeeds_in_script_mode);
    RUN(test_comment_in_param_is_data);
    RUN(test_huge_identifier);
    RUN(test_ten_thousand_quotes);
    TEST_DONE();
    return 0;
}
