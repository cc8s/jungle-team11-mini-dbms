#include "test.h"
#include "test_util.h"

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
    TEST_DONE();
    return 0;
}
