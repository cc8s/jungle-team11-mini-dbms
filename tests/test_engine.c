#include "test.h"
#include "test_util.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define USERS_SQL "CREATE TABLE users (id INT, name VARCHAR(8), age INT);"
#define ORDERS_SQL "CREATE TABLE orders (id INT, item VARCHAR(4));"
#define FILE_MODE 0644

static TestDb open_db(const char *create_sql)
{
    TestDb db = test_db_create(create_sql);
    CHECK(db.dir != NULL);
    CHECK(db.engine != NULL);
    return db;
}

/* sql 을 실행하고 에러 코드만 본다 */
static ErrorCode run_error(Engine *engine, const char *sql)
{
    Result result = engine_execute(engine, sql, NULL, 0);
    ErrorCode error = result.error;
    CHECK(error == ERR_NONE ? result.message[0] == '\0' : result.message[0] != '\0');
    result_free(&result);
    return error;
}

/* INSERT 를 실행하고 부여된 id 를 돌려준다 */
static int32_t insert_ok(Engine *engine, const char *sql)
{
    Result result = engine_execute(engine, sql, NULL, 0);
    CHECK(result.error == ERR_NONE);
    CHECK(result.affected_rows == 1);
    CHECK(result.elapsed_ms >= 0);
    int32_t id = result.last_insert_id;
    result_free(&result);
    return id;
}

static void write_file(const char *dir, const char *name, const char *text)
{
    char path[TEST_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, FILE_MODE);
    CHECK(fd >= 0);
    CHECK(pwrite(fd, text, strlen(text), 0) == (ssize_t)strlen(text));
    close(fd);
}

static Engine *open_engine_error(const char *dir, ErrorCode *out_error, char *out_message)
{
    Engine *engine = engine_open(dir, out_error, out_message);
    CHECK(engine == NULL);
    CHECK(out_message[0] != '\0');
    return engine;
}

/* T14-01 */ static void test_open_two_tables(void)
{
    TestDb db = open_db(USERS_SQL ORDERS_SQL);
    CHECK(insert_ok(db.engine, "INSERT INTO users VALUES ('kim', 25)") == 1);
    CHECK(insert_ok(db.engine, "INSERT INTO orders VALUES ('pen')") == 1);
    test_db_destroy(&db);
}

/* T14-02 */ static void test_insert(void)
{
    TestDb db = open_db(USERS_SQL);
    Result result = engine_execute(db.engine, "INSERT INTO users VALUES ('kim', 25);", NULL, 0);
    CHECK(result.error == ERR_NONE);
    CHECK(result.affected_rows == 1);
    CHECK(result.last_insert_id == 1);
    CHECK(result.row_count == 0);
    CHECK(result.rows == NULL);
    result_free(&result);
    test_db_destroy(&db);
}

/* T14-03 */ static void test_insert_three_times(void)
{
    TestDb db = open_db(USERS_SQL);
    CHECK(insert_ok(db.engine, "INSERT INTO users VALUES ('kim', 25)") == 1);
    CHECK(insert_ok(db.engine, "INSERT INTO users VALUES ('lee', 30)") == 2);
    CHECK(insert_ok(db.engine, "INSERT INTO users VALUES ('park', 22)") == 3);
    test_db_destroy(&db);
}

/* T14-04 */ static void test_insert_after_reopen(void)
{
    TestDb db = open_db(USERS_SQL);
    for (int i = 0; i < 3; i++)
        insert_ok(db.engine, "INSERT INTO users VALUES ('kim', 25)");
    test_db_reopen(&db);
    CHECK(db.engine != NULL);
    CHECK(insert_ok(db.engine, "INSERT INTO users VALUES ('choi', 41)") == 4);
    test_db_destroy(&db);
}

/* E14-01 */ static void test_open_empty_dir(void)
{
    TestDb db = open_db(NULL);
    CHECK(run_error(db.engine, "INSERT INTO users VALUES ('kim', 25)") == ERR_UNKNOWN_TABLE);
    CHECK(run_error(db.engine, "SELECT * FROM users") == ERR_UNKNOWN_TABLE);
    test_db_destroy(&db);
}

/* E14-02 */ static void test_open_missing_dir(void)
{
    char *dir = test_make_temp_dir();
    CHECK(dir != NULL);
    char missing[TEST_PATH_MAX];
    snprintf(missing, sizeof(missing), "%s/missing", dir);
    ErrorCode error = ERR_NONE;
    char message[MAX_MESSAGE];
    open_engine_error(missing, &error, message);
    CHECK(error == ERR_IO);
    test_remove_dir(dir);
    free(dir);
}

/* E14-03 */ static void test_open_ignores_other_files(void)
{
    TestDb db = open_db(USERS_SQL);
    write_file(db.dir, "README", "not a table");
    write_file(db.dir, "a.tbl.bak", "not a table");
    write_file(db.dir, "notes.TBL", "not a table"); /* 확장자는 정확히 .tbl 만 */
    test_db_reopen(&db);
    CHECK(db.engine != NULL);
    CHECK(insert_ok(db.engine, "INSERT INTO users VALUES ('kim', 25)") == 1);
    test_db_destroy(&db);
}

/* E14-04 */ static void test_open_fails_on_corrupt_table(void)
{
    TestDb db = open_db(USERS_SQL);
    write_file(db.dir, "broken.tbl", "this is not a table header");
    engine_close(db.engine);
    db.engine = NULL;

    ErrorCode error = ERR_NONE;
    char message[MAX_MESSAGE];
    open_engine_error(db.dir, &error, message);
    CHECK(error == ERR_CORRUPT);
    CHECK(strstr(message, "broken.tbl") != NULL);
    test_db_destroy(&db);
}

/* E14-05 */ static void test_failed_insert_keeps_id(void)
{
    TestDb db = open_db(USERS_SQL);
    CHECK(run_error(db.engine, "INSERT INTO users VALUES ('kim')") == ERR_VALUE_COUNT);
    CHECK(run_error(db.engine, "INSERT INTO users VALUES ('toolongname', 1)") ==
          ERR_VALUE_TOO_LONG);
    CHECK(run_error(db.engine, "INSERT INTO users VALUES ('kim', 25") == ERR_SYNTAX);
    CHECK(insert_ok(db.engine, "INSERT INTO users VALUES ('kim', 25)") == 1);
    test_db_destroy(&db);
}

/* E14-06 */ static void test_execute_empty_sql(void)
{
    TestDb db = open_db(USERS_SQL);
    CHECK(run_error(db.engine, "") == ERR_SYNTAX);
    CHECK(run_error(db.engine, " ; -- nothing") == ERR_SYNTAX);
    test_db_destroy(&db);
}

static void test_execute_reports_line(void)
{
    TestDb db = open_db(USERS_SQL);
    Result result = engine_execute(db.engine, "\n\nINSERT INTO users\nVALUES ('kim')", NULL, 0);
    CHECK(result.error == ERR_VALUE_COUNT);
    CHECK(result.line == 3); /* 검증 에러는 문장 시작 줄 */
    result_free(&result);

    result = engine_execute(db.engine, "INSERT INTO users\nVALUES (kim, 1)", NULL, 0);
    CHECK(result.error == ERR_SYNTAX);
    CHECK(result.line == 2); /* 문법 에러는 문제 토큰의 줄 */
    result_free(&result);
    test_db_destroy(&db);
}

static void test_execute_rejects_create_and_params(void)
{
    TestDb db = open_db(USERS_SQL);
    CHECK(run_error(db.engine, "CREATE TABLE t (id INT)") == ERR_UNSUPPORTED);
    CHECK(run_error(db.engine, "INSERT INTO users VALUES (?, 1)") == ERR_PARAM);
    CHECK(run_error(db.engine, "INSERT INTO users VALUES ('a', 1); INSERT INTO users "
                               "VALUES ('b', 2)") == ERR_UNSUPPORTED); /* 한 문장만 */
    CHECK(insert_ok(db.engine, "INSERT INTO users VALUES ('kim', 25)") == 1);
    test_db_destroy(&db);
}

static void test_table_name_from_upper_case_file(void)
{
    TestDb db = open_db(NULL);
    CHECK(test_create_tables(db.dir, "CREATE TABLE people (id INT)"));
    char from[TEST_PATH_MAX];
    char to[TEST_PATH_MAX];
    snprintf(from, sizeof(from), "%s/people.tbl", db.dir);
    snprintf(to, sizeof(to), "%s/People.tbl", db.dir);
    CHECK(rename(from, to) == 0);
    test_db_reopen(&db);
    CHECK(db.engine != NULL);
    CHECK(run_error(db.engine, "SELECT * FROM PEOPLE") != ERR_UNKNOWN_TABLE); /* 소문자 이름 */
    test_db_destroy(&db);
}

static void test_close_null_is_safe(void)
{
    engine_close(NULL);
}

int main(void)
{
    RUN(test_open_two_tables);
    RUN(test_insert);
    RUN(test_insert_three_times);
    RUN(test_insert_after_reopen);
    RUN(test_open_empty_dir);
    RUN(test_open_missing_dir);
    RUN(test_open_ignores_other_files);
    RUN(test_open_fails_on_corrupt_table);
    RUN(test_failed_insert_keeps_id);
    RUN(test_execute_empty_sql);
    RUN(test_execute_reports_line);
    RUN(test_execute_rejects_create_and_params);
    RUN(test_table_name_from_upper_case_file);
    RUN(test_close_null_is_safe);
    TEST_DONE();
    return 0;
}
