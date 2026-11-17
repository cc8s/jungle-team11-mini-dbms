#include "test.h"
#include "test_util.h"

#include <sys/stat.h>

#include "cli.h"

#define USERS_SCHEMA "CREATE TABLE users (id INT, name VARCHAR(8), age INT);\n"
#define EXIT_OK 0
#define EXIT_SQL_ERROR 1
#define EXIT_USAGE 2
#define DIR_MODE 0755

/* cli_run 한 번의 결과 */
typedef struct {
    int code;
    char *out;
    char *err;
} CliRun;

static CliRun run_cli(int argc, char **argv)
{
    TestCapture out;
    TestCapture err;
    test_capture_open(&out);
    test_capture_open(&err);
    CHECK(out.stream != NULL && err.stream != NULL);
    CliRun run;
    run.code = cli_run(argc, argv, out.stream, err.stream);
    run.out = test_capture_close(&out);
    run.err = test_capture_close(&err);
    return run;
}

static void cli_run_free(CliRun *run)
{
    free(run->out);
    free(run->err);
}

static bool path_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0;
}

/* 임시 디렉터리 하나: dir/data 를 데이터 디렉터리로 쓴다 */
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

/* ws 의 데이터 디렉터리에 schema_text 로 init 한다 */
static CliRun run_init(Workspace *ws, const char *schema_text)
{
    char schema[TEST_PATH_MAX];
    CHECK(test_write_file(ws->dir, "schema.sql", schema_text, schema));
    char *argv[] = { "minidb", "-d", ws->data_dir, "init", schema, NULL };
    return run_cli(5, argv);
}

static void init_users(Workspace *ws)
{
    CliRun run = run_init(ws, USERS_SCHEMA);
    CHECK(run.code == EXIT_OK);
    cli_run_free(&run);
}

/* ws 의 데이터 디렉터리에서 sql_text 를 실행한다 (quiet 이면 -q) */
static CliRun run_sql(Workspace *ws, const char *sql_text, bool quiet)
{
    char sql[TEST_PATH_MAX];
    CHECK(test_write_file(ws->dir, "query.sql", sql_text, sql));
    if (quiet) {
        char *argv[] = { "minidb", "-d", ws->data_dir, "-q", sql, NULL };
        return run_cli(5, argv);
    }
    char *argv[] = { "minidb", "-d", ws->data_dir, sql, NULL };
    return run_cli(4, argv);
}

/* T18-01 */ static void test_init_then_run(void)
{
    Workspace ws = workspace_make();
    CliRun run = run_init(&ws, USERS_SCHEMA);
    CHECK(run.code == EXIT_OK);
    CHECK(strstr(run.out, "users") != NULL);
    cli_run_free(&run);
    char table_path[TEST_PATH_MAX];
    CHECK(table_build_path(ws.data_dir, "users", table_path, sizeof(table_path)));
    CHECK(path_exists(table_path));

    run = run_sql(&ws, "INSERT INTO users VALUES ('kim', 25);\nSELECT * FROM users;\n", false);
    CHECK(run.code == EXIT_OK);
    CHECK(strstr(run.out, "Query OK, 1 row affected") != NULL);
    CHECK(strstr(run.out, "| id | name | age |") != NULL);
    CHECK(strstr(run.out, "|  1 | kim  |  25 |") != NULL);
    CHECK(strstr(run.out, "1 row in set") != NULL);
    CHECK(run.err[0] == '\0');
    cli_run_free(&run);
    workspace_remove(&ws);
}

/* T18-02 */ static void test_run_with_errors(void)
{
    Workspace ws = workspace_make();
    init_users(&ws);
    CliRun run = run_sql(&ws,
                         "INSERT INTO users VALUES ('kim', 25);\n"
                         "SELEC * FROM users;\n"
                         "SELECT * FROM users;\n",
                         false);
    CHECK(run.code == EXIT_SQL_ERROR);
    CHECK(strstr(run.err, "ERROR (line 2, statement 2):") != NULL);
    CHECK(strstr(run.out, "1 row in set") != NULL); /* 에러 뒤 문장도 실행됨 */
    CHECK(strstr(run.out, "ERROR") == NULL);
    cli_run_free(&run);
    workspace_remove(&ws);
}

/* T18-03 */ static void test_data_dir_option(void)
{
    Workspace first = workspace_make();
    Workspace second = workspace_make();
    init_users(&second);
    CliRun run = run_sql(&first, "SELECT * FROM users;", false);
    CHECK(run.code == EXIT_SQL_ERROR); /* first 에는 users 가 없다 */
    cli_run_free(&run);
    run = run_sql(&second, "SELECT * FROM users;", false);
    CHECK(run.code == EXIT_OK);
    CHECK(strstr(run.out, "Empty set") != NULL);
    cli_run_free(&run);
    workspace_remove(&first);
    workspace_remove(&second);
}

/* T18-04 */ static void test_quiet_option(void)
{
    Workspace ws = workspace_make();
    init_users(&ws);
    CliRun run = run_sql(&ws,
                         "INSERT INTO users VALUES ('kim', 25);\n"
                         "SELECT * FROM users;\n"
                         "SELECT nmae FROM users;\n",
                         true);
    CHECK(run.code == EXIT_SQL_ERROR);
    CHECK(strstr(run.out, "+----") == NULL);
    CHECK(strstr(run.out, "Query OK") == NULL);
    CHECK(strstr(run.out, "3 statements, 1 errors") != NULL);
    CHECK(strstr(run.err, "ERROR (line 3, statement 3):") != NULL); /* 에러는 그대로 */
    cli_run_free(&run);
    workspace_remove(&ws);
}

/* E18-01 */ static void test_no_arguments(void)
{
    char *argv[] = { "minidb", NULL };
    CliRun run = run_cli(1, argv);
    CHECK(run.code == EXIT_USAGE);
    CHECK(strstr(run.err, "usage") != NULL);
    cli_run_free(&run);
}

/* E18-02 */ static void test_missing_sql_file(void)
{
    Workspace ws = workspace_make();
    char missing[TEST_PATH_MAX];
    snprintf(missing, sizeof(missing), "%s/nope.sql", ws.dir);
    char *argv[] = { "minidb", "-d", ws.data_dir, missing, NULL };
    CliRun run = run_cli(4, argv);
    CHECK(run.code == EXIT_USAGE);
    CHECK(strstr(run.err, "nope.sql") != NULL);
    cli_run_free(&run);
    workspace_remove(&ws);
}

/* E18-03 */ static void test_empty_sql_file(void)
{
    Workspace ws = workspace_make();
    init_users(&ws);
    CliRun run = run_sql(&ws, "", false);
    CHECK(run.code == EXIT_OK);
    CHECK(run.out[0] == '\0' && run.err[0] == '\0');
    cli_run_free(&run);
    workspace_remove(&ws);
}

/* E18-04 */ static void test_missing_data_dir(void)
{
    Workspace ws = workspace_make();
    char sql[TEST_PATH_MAX];
    CHECK(test_write_file(ws.dir, "query.sql", "SELECT * FROM users;", sql));
    char missing[TEST_PATH_MAX];
    snprintf(missing, sizeof(missing), "%s/no_data", ws.dir);
    char *argv[] = { "minidb", "-d", missing, sql, NULL };
    CliRun run = run_cli(4, argv);
    CHECK(run.code == EXIT_USAGE);
    cli_run_free(&run);

    CHECK(test_write_file(ws.dir, "schema.sql", USERS_SCHEMA, sql));
    char *init_argv[] = { "minidb", "-d", missing, "init", sql, NULL };
    run = run_cli(5, init_argv);
    CHECK(run.code == EXIT_USAGE);
    cli_run_free(&run);
    workspace_remove(&ws);
}

/* E18-05 */ static void test_init_existing_table(void)
{
    Workspace ws = workspace_make();
    init_users(&ws);
    CliRun run = run_sql(&ws, "INSERT INTO users VALUES ('kim', 25);", false);
    cli_run_free(&run);

    run = run_init(&ws, USERS_SCHEMA);
    CHECK(run.code == EXIT_SQL_ERROR);
    CHECK(strstr(run.err, "already exists") != NULL);
    cli_run_free(&run);
    run = run_sql(&ws, "SELECT * FROM users;", false);
    CHECK(strstr(run.out, "1 row in set") != NULL); /* 기존 데이터 유지 */
    cli_run_free(&run);
    workspace_remove(&ws);
}

static bool users_table_exists(const Workspace *ws)
{
    char path[TEST_PATH_MAX];
    CHECK(table_build_path(ws->data_dir, "users", path, sizeof(path)));
    return path_exists(path);
}

/* E18-06, E18-07 */ static void test_init_rejects_bad_schema(void)
{
    Workspace ws = workspace_make();
    const char *bad[] = {
        "SELECT * FROM users;",
        USERS_SCHEMA "CREATE TABLE orders (id INT);",
        USERS_SCHEMA "INSERT INTO users VALUES ('kim', 1);",
        "CREATE TABLE users (name VARCHAR(8));",
        "",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CliRun run = run_init(&ws, bad[i]);
        CHECK(run.code == EXIT_SQL_ERROR);
        CHECK(strstr(run.err, "ERROR") != NULL);
        cli_run_free(&run);
        CHECK(!users_table_exists(&ws));
    }
    workspace_remove(&ws);
}

/* E18-08 */ static void test_unknown_option(void)
{
    char *argv[] = { "minidb", "-x", "query.sql", NULL };
    CliRun run = run_cli(3, argv);
    CHECK(run.code == EXIT_USAGE);
    CHECK(strstr(run.err, "usage") != NULL);
    cli_run_free(&run);

    char *missing_value[] = { "minidb", "-d", NULL };
    run = run_cli(2, missing_value);
    CHECK(run.code == EXIT_USAGE);
    cli_run_free(&run);

    char *extra[] = { "minidb", "a.sql", "b.sql", NULL };
    run = run_cli(3, extra);
    CHECK(run.code == EXIT_USAGE);
    cli_run_free(&run);

    char *init_without_file[] = { "minidb", "init", NULL };
    run = run_cli(2, init_without_file);
    CHECK(run.code == EXIT_USAGE);
    cli_run_free(&run);
}

/* E18-09 */ static void test_directory_as_sql_file(void)
{
    Workspace ws = workspace_make();
    char *argv[] = { "minidb", "-d", ws.data_dir, ws.dir, NULL };
    CliRun run = run_cli(4, argv);
    CHECK(run.code == EXIT_USAGE);
    cli_run_free(&run);
    workspace_remove(&ws);
}

static void test_nul_byte_in_sql_file(void)
{
    Workspace ws = workspace_make();
    init_users(&ws);
    char sql[TEST_PATH_MAX];
    snprintf(sql, sizeof(sql), "%s/nul.sql", ws.dir);
    FILE *file = fopen(sql, "w");
    CHECK(file != NULL);
    const char bytes[] = "INSERT INTO users VALUES ('kim', 25);\0"
                         "INSERT INTO users VALUES ('a', 1);";
    CHECK(fwrite(bytes, 1, sizeof(bytes) - 1, file) == sizeof(bytes) - 1);
    fclose(file);
    char *argv[] = { "minidb", "-d", ws.data_dir, sql, NULL };
    CliRun run = run_cli(4, argv);
    CHECK(run.code == EXIT_SQL_ERROR); /* \0 뒤를 조용히 버리지 않는다 */
    cli_run_free(&run);
    run = run_sql(&ws, "SELECT * FROM users;", false);
    CHECK(strstr(run.out, "Empty set") != NULL); /* 아무 문장도 실행하지 않았다 */
    cli_run_free(&run);
    workspace_remove(&ws);
}

/* ---- S28 대량 데이터 도구 ---- */

/* 이름이 user100000 (10 자) 까지 들어가도록 VARCHAR(32) */
#define BULK_SCHEMA "CREATE TABLE users (id INT, name VARCHAR(32), age INT);\n"
#define GEN_SQL_COMMAND "python3 tools/gen_sql.py"
#define COMMAND_SIZE (TEST_PATH_MAX + 64)
#define GEN_SMALL_COUNT 10
#define GEN_BULK_COUNT 100000
#define LINE_SIZE 128

/* tools/gen_sql.py count 의 출력을 ws->dir/name 에 쓰고 경로를 남긴다. 테스트는 저장소 루트에서 돈다 */
static void generate_sql(const Workspace *ws, int count, const char *name, char *out_path)
{
    snprintf(out_path, TEST_PATH_MAX, "%s/%s", ws->dir, name);
    char command[COMMAND_SIZE];
    snprintf(command, sizeof(command), GEN_SQL_COMMAND " %d > '%s'", count, out_path);
    CHECK(system(command) == 0);
}

static CliRun run_sql_file(Workspace *ws, char *path)
{
    char *argv[] = { "minidb", "-d", ws->data_dir, "-q", path, NULL };
    return run_cli(5, argv);
}

static int32_t users_row_count(const Workspace *ws)
{
    char path[COMMAND_SIZE]; /* data_dir (최대 TEST_PATH_MAX) + "/users.tbl" */
    snprintf(path, sizeof(path), "%s/users.tbl", ws->data_dir);
    ErrorCode error = ERR_NONE;
    Table *table = table_open(path, &error);
    CHECK(table != NULL);
    int32_t count = table_record_count(table);
    table_close(table);
    return count;
}

/* T28-01 */ static void test_gen_sql_lines(void)
{
    Workspace ws = workspace_make();
    char path[TEST_PATH_MAX];
    generate_sql(&ws, GEN_SMALL_COUNT, "small.sql", path);
    FILE *file = fopen(path, "r"); /* 테스트 확인용 (엔진 I/O 규칙 대상 아님) */
    CHECK(file != NULL);
    char line[LINE_SIZE];
    int count = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        count++;
        char expected[LINE_SIZE];
        snprintf(expected, sizeof(expected), "INSERT INTO users VALUES ('user%d', ", count);
        CHECK(strncmp(line, expected, strlen(expected)) == 0);
        CHECK(strstr(line, ");\n") != NULL);
    }
    fclose(file);
    CHECK(count == GEN_SMALL_COUNT);
    workspace_remove(&ws);
}

/* T28-02, E28-03 (ASan 빌드에서 문장마다 누수 없음) */ static void test_run_generated_bulk(void)
{
    Workspace ws = workspace_make();
    CliRun init = run_init(&ws, BULK_SCHEMA);
    CHECK(init.code == EXIT_OK);
    cli_run_free(&init);
    char path[TEST_PATH_MAX];
    generate_sql(&ws, GEN_BULK_COUNT, "bulk.sql", path);
    CliRun run = run_sql_file(&ws, path);
    CHECK(run.code == EXIT_OK);
    CHECK(strstr(run.out, "100000 statements, 0 errors") != NULL);
    CHECK(run.err[0] == '\0');
    cli_run_free(&run);
    CHECK(users_row_count(&ws) == GEN_BULK_COUNT);
    workspace_remove(&ws);
}

/* T28-03 */ static void test_quiet_prints_only_summary(void)
{
    Workspace ws = workspace_make();
    CliRun init = run_init(&ws, BULK_SCHEMA);
    cli_run_free(&init);
    char path[TEST_PATH_MAX];
    generate_sql(&ws, GEN_SMALL_COUNT, "small.sql", path);
    CliRun run = run_sql_file(&ws, path);
    CHECK(run.code == EXIT_OK);
    CHECK_STR_EQ(run.out, "10 statements, 0 errors\n");
    CHECK(run.err[0] == '\0');
    cli_run_free(&run);
    workspace_remove(&ws);
}

/* E28-01 */ static void test_gen_sql_zero(void)
{
    Workspace ws = workspace_make();
    char path[TEST_PATH_MAX];
    generate_sql(&ws, 0, "empty.sql", path);
    struct stat info;
    CHECK(stat(path, &info) == 0);
    CHECK(info.st_size == 0);
    workspace_remove(&ws);
}

/* E28-02 */ static void test_run_generated_twice(void)
{
    Workspace ws = workspace_make();
    CliRun init = run_init(&ws, BULK_SCHEMA);
    cli_run_free(&init);
    char path[TEST_PATH_MAX];
    generate_sql(&ws, GEN_SMALL_COUNT, "small.sql", path);
    for (int i = 0; i < 2; i++) {
        CliRun run = run_sql_file(&ws, path);
        CHECK(run.code == EXIT_OK);
        cli_run_free(&run);
    }
    CHECK(users_row_count(&ws) == 2 * GEN_SMALL_COUNT); /* id 1..2N */
    CliRun select = run_sql(&ws, "SELECT id FROM users WHERE name = 'user1';", false);
    CHECK(select.code == EXIT_OK);
    CHECK(strstr(select.out, "2 rows") != NULL); /* 이름은 중복될 수 있다 */
    CHECK(strstr(select.out, "| 11 ") != NULL);
    cli_run_free(&select);
    workspace_remove(&ws);
}

int main(void)
{
    RUN(test_init_then_run);
    RUN(test_run_with_errors);
    RUN(test_data_dir_option);
    RUN(test_quiet_option);
    RUN(test_no_arguments);
    RUN(test_missing_sql_file);
    RUN(test_empty_sql_file);
    RUN(test_missing_data_dir);
    RUN(test_init_existing_table);
    RUN(test_init_rejects_bad_schema);
    RUN(test_unknown_option);
    RUN(test_directory_as_sql_file);
    RUN(test_nul_byte_in_sql_file);
    RUN(test_gen_sql_lines);
    RUN(test_run_generated_bulk);
    RUN(test_quiet_prints_only_summary);
    RUN(test_gen_sql_zero);
    RUN(test_run_generated_twice);
    TEST_DONE();
    return 0;
}
