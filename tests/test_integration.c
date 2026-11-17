#include "test.h"
#include "test_util.h"

#include <sys/stat.h>

#include "cli.h"

/* Phase 1 통합 시나리오. CLI 를 "프로그램 실행 한 번" 으로 보고 여러 번 돌린 뒤
 * 엔진으로 다시 열어 저장된 값을 확인한다. */

#define USERS_SCHEMA "CREATE TABLE users (id INT, name VARCHAR(16), age INT);\n"
#define ORDERS_SCHEMA "CREATE TABLE orders (id INT, item VARCHAR(8), qty INT);\n"
#define USER_COUNT 100
#define SCRIPT_STATEMENTS 1000
#define ERROR_EVERY 7      /* 일곱 번째 문장마다 에러 문장을 넣는다 */
#define LINE_MAX_LEN 96
#define DIR_MODE 0755

typedef struct {
    char *dir;
    char data_dir[TEST_PATH_MAX];
} Workspace;

static Workspace workspace_make(void)
{
    Workspace ws;
    ws.dir = test_make_temp_dir();
    CHECK(ws.dir != NULL);
    snprintf(ws.data_dir, sizeof(ws.data_dir), "%s/data", ws.dir);
    CHECK(mkdir(ws.data_dir, DIR_MODE) == 0);
    return ws;
}

static void workspace_remove(Workspace *ws)
{
    test_remove_dir(ws->dir);
    free(ws->dir);
}

/* 출력은 버리고 종료 코드만 본다 */
static int run_cli(int argc, char **argv)
{
    TestCapture out;
    TestCapture err;
    test_capture_open(&out);
    test_capture_open(&err);
    CHECK(out.stream != NULL && err.stream != NULL);
    int code = cli_run(argc, argv, out.stream, err.stream);
    free(test_capture_close(&out));
    free(test_capture_close(&err));
    return code;
}

static void cli_init(Workspace *ws, const char *name, const char *schema_text)
{
    char schema[TEST_PATH_MAX];
    CHECK(test_write_file(ws->dir, name, schema_text, schema));
    char *argv[] = { "minidb", "-d", ws->data_dir, "init", schema, NULL };
    CHECK(run_cli(5, argv) == CLI_EXIT_OK);
}

static int cli_execute(Workspace *ws, const char *sql_text)
{
    char sql[TEST_PATH_MAX];
    CHECK(test_write_file(ws->dir, "query.sql", sql_text, sql));
    char *argv[] = { "minidb", "-d", ws->data_dir, "-q", sql, NULL };
    return run_cli(5, argv);
}

static Engine *open_engine(const Workspace *ws)
{
    ErrorCode error;
    char message[MAX_MESSAGE];
    Engine *engine = engine_open(ws->data_dir, &error, message);
    CHECK(engine != NULL);
    return engine;
}

static Result select_ok(Engine *engine, const char *sql)
{
    Result result = engine_execute(engine, sql, NULL, 0);
    CHECK(result.error == ERR_NONE);
    return result;
}

static int64_t count_rows(Engine *engine, const char *sql)
{
    Result result = select_ok(engine, sql);
    int64_t count = result.row_count;
    result_free(&result);
    return count;
}

static void check_str(const Result *result, int64_t row, int col, const char *expected)
{
    size_t len = 0;
    const char *text = result_get_str(result, row, col, &len);
    CHECK(len == strlen(expected) && memcmp(text, expected, len) == 0);
}

/* "INSERT INTO users VALUES ('user<i>', <i % 50>);" 를 count 줄 이어 붙인다 (호출한 쪽이 free) */
static char *users_insert_script(int first, int count)
{
    char *script = malloc((size_t)count * LINE_MAX_LEN + 1);
    CHECK(script != NULL);
    size_t len = 0;
    for (int i = first; i < first + count; i++)
        len += (size_t)snprintf(script + len, LINE_MAX_LEN,
                                "INSERT INTO users VALUES ('user%d', %d);\n", i, i % 50);
    return script;
}

/* T19-01 */ static void test_init_insert_100_and_select(void)
{
    Workspace ws = workspace_make();
    cli_init(&ws, "users.sql", USERS_SCHEMA);
    char *script = users_insert_script(1, USER_COUNT);
    CHECK(cli_execute(&ws, script) == CLI_EXIT_OK);
    free(script);

    Engine *engine = open_engine(&ws);
    CHECK(count_rows(engine, "SELECT * FROM users") == USER_COUNT);
    Result result = select_ok(engine, "SELECT name, age FROM users WHERE id = 42");
    CHECK(result.row_count == 1);
    check_str(&result, 0, 0, "user42");
    CHECK(result_get_int(&result, 0, 1) == 42);
    result_free(&result);
    CHECK(count_rows(engine, "SELECT * FROM users WHERE age = 0") == 2);   /* 50, 100 */
    CHECK(count_rows(engine, "SELECT * FROM users WHERE age < 10") == 20);
    CHECK(count_rows(engine, "SELECT * FROM users WHERE id >= 91") == 10);
    CHECK(count_rows(engine, "SELECT * FROM users WHERE name = 'user7'") == 1);
    CHECK(count_rows(engine, "SELECT id FROM users WHERE age != 0 LIMIT 5") == 5);
    engine_close(engine);
    workspace_remove(&ws);
}

/* T19-02 */ static void test_data_survives_restart(void)
{
    Workspace ws = workspace_make();
    cli_init(&ws, "users.sql", USERS_SCHEMA);
    CHECK(cli_execute(&ws, "INSERT INTO users VALUES ('kim', 25);\n"
                           "INSERT INTO users VALUES ('lee', 30);\n") == CLI_EXIT_OK);
    /* 프로그램을 다시 실행: 새 엔진이 파일에서 next_id 를 읽어 이어서 부여한다 */
    CHECK(cli_execute(&ws, "INSERT INTO users VALUES ('park', 22);\n") == CLI_EXIT_OK);

    Engine *engine = open_engine(&ws);
    Result result = select_ok(engine, "SELECT * FROM users");
    CHECK(result.row_count == 3);
    for (int i = 0; i < 3; i++)
        CHECK(result_get_int(&result, i, 0) == i + 1);
    check_str(&result, 2, 1, "park");
    result_free(&result);
    engine_close(engine);
    workspace_remove(&ws);
}

/* 종류가 다른 에러 문장: 문법, 값 개수, 컬럼, id 직접 지정, 타입 */
static const char *const BAD_STATEMENTS[] = {
    "SELEC * FROM users;",
    "INSERT INTO users VALUES ('only_name');",
    "SELECT nmae FROM users;",
    "INSERT INTO users VALUES (1, 'x', 2);",
    "SELECT * FROM users WHERE age = 'x';",
};

/* 문장 i 를 script 에 덧붙인다. ERROR_EVERY 번째마다 종류를 돌려 가며 에러 문장 */
static size_t append_statement(char *script, size_t len, int i)
{
    int bad_count = (int)(sizeof(BAD_STATEMENTS) / sizeof(BAD_STATEMENTS[0]));
    if (i % ERROR_EVERY == 0)
        return len + (size_t)snprintf(script + len, LINE_MAX_LEN, "%s\n",
                                      BAD_STATEMENTS[(i / ERROR_EVERY) % bad_count]);
    if (i % 2 == 0)
        return len + (size_t)snprintf(script + len, LINE_MAX_LEN,
                                      "SELECT * FROM users WHERE id = %d;\n", i);
    return len + (size_t)snprintf(script + len, LINE_MAX_LEN,
                                  "INSERT INTO users VALUES ('u%d', %d);\n", i, i);
}

static void count_error(const Result *result, int stmt_index, void *ctx)
{
    (void)stmt_index;
    int *errors = ctx;
    *errors += result->error != ERR_NONE;
}

/* T19-03 */ static void test_large_mixed_script(void)
{
    Workspace ws = workspace_make();
    cli_init(&ws, "users.sql", USERS_SCHEMA);
    char *script = malloc((size_t)SCRIPT_STATEMENTS * LINE_MAX_LEN + 1);
    CHECK(script != NULL);
    size_t len = 0;
    int expected_errors = 0;
    int expected_inserts = 0;
    for (int i = 1; i <= SCRIPT_STATEMENTS; i++) {
        len = append_statement(script, len, i);
        expected_errors += i % ERROR_EVERY == 0;
        expected_inserts += i % ERROR_EVERY != 0 && i % 2 == 1;
    }
    CHECK(cli_execute(&ws, script) == CLI_EXIT_SQL_ERROR);

    Engine *engine = open_engine(&ws);
    CHECK(count_rows(engine, "SELECT * FROM users") == expected_inserts);
    int errors = 0;
    CHECK(engine_execute_script(engine, script, count_error, &errors) == expected_errors);
    CHECK(errors == expected_errors);
    engine_close(engine);
    free(script);
    workspace_remove(&ws);
}

/* T19-04 */ static void test_two_tables_do_not_mix(void)
{
    Workspace ws = workspace_make();
    cli_init(&ws, "users.sql", USERS_SCHEMA);
    cli_init(&ws, "orders.sql", ORDERS_SCHEMA);
    CHECK(cli_execute(&ws, "INSERT INTO users VALUES ('kim', 25);\n"
                           "INSERT INTO orders VALUES ('pen', 3);\n"
                           "INSERT INTO users VALUES ('lee', 30);\n"
                           "INSERT INTO orders VALUES ('ink', 1);\n"
                           "INSERT INTO orders VALUES ('cup', 2);\n"
                           "SELECT * FROM users;\n"
                           "SELECT * FROM orders WHERE qty > 1;\n") == CLI_EXIT_OK);

    Engine *engine = open_engine(&ws);
    Result users = select_ok(engine, "SELECT * FROM users");
    CHECK(users.row_count == 2);
    check_str(&users, 1, 1, "lee");
    CHECK(result_get_int(&users, 1, 0) == 2);
    result_free(&users);
    Result orders = select_ok(engine, "SELECT item FROM orders");
    CHECK(orders.row_count == 3);
    check_str(&orders, 0, 0, "pen");
    check_str(&orders, 2, 0, "cup");
    result_free(&orders);
    CHECK(engine_execute(engine, "SELECT item FROM users", NULL, 0).error ==
          ERR_UNKNOWN_COLUMN); /* 다른 테이블의 컬럼은 보이지 않는다 */
    engine_close(engine);
    workspace_remove(&ws);
}

int main(void)
{
    RUN(test_init_insert_100_and_select);
    RUN(test_data_survives_restart);
    RUN(test_large_mixed_script);
    RUN(test_two_tables_do_not_mix);
    TEST_DONE();
    return 0;
}
