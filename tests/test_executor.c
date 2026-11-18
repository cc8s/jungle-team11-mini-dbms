#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>

#include "cli.h"
#include "executor.h"
#include "test.h"

/* 레포의 schema/users.schema 를 쓴다: id INT, name VARCHAR(32), email VARCHAR(64), age INT */
#define SCHEMA_DIR "schema"

static char data_dir[64];
static struct executor ex;
static char *out_buf;
static size_t out_len;
static int failures;

static void open_db(void)
{
    if (data_dir[0] == '\0') {
        strcpy(data_dir, "/tmp/minidb-exec-XXXXXX");
        CHECK(mkdtemp(data_dir) != NULL);
    }
    CHECK(executor_init(&ex, SCHEMA_DIR, data_dir) == 0);
}

/* 테스트마다 빈 데이터 디렉터리에서 시작한다 */
static void fresh_db(void)
{
    char path[128];

    open_db();
    snprintf(path, sizeof(path), "%s/users.tbl", data_dir);
    unlink(path);
}

/* sql 을 실행하고 출력 전체를 out_buf 에 담는다 */
static const char *run(const char *sql)
{
    free(out_buf);
    FILE *out = open_memstream(&out_buf, &out_len);
    CHECK(out != NULL);
    failures = cli_run_sql(&ex, sql, out);
    fclose(out);
    return out_buf;
}

/* TC-EXEC-001 EP: INSERT 후 SELECT * — 출력 형식과 id 자동 부여 */
static void test_insert_then_select(void)
{
    fresh_db();
    CHECK_STR_EQ(run("INSERT INTO users VALUES ('kim', 'kim@a.com', 20);"),
                 "INSERT 1 (id = 1)\n");
    CHECK_STR_EQ(run("INSERT INTO users VALUES ('lee', 'lee@a.com', 31);"),
                 "INSERT 1 (id = 2)\n");
    CHECK_STR_EQ(run("SELECT * FROM users;"), "id | name | email | age\n"
                                              "1 | kim | kim@a.com | 20\n"
                                              "2 | lee | lee@a.com | 31\n"
                                              "(2 rows)\n");
    CHECK(failures == 0);
    executor_close(&ex);
}

/* TC-EXEC-002 EP: 컬럼 목록 INSERT — 순서가 달라도 되고, 빠진 컬럼은 0 / 빈 문자열 */
static void test_insert_partial_columns(void)
{
    fresh_db();
    run("INSERT INTO users (age, name) VALUES (40, 'park');");
    CHECK_STR_EQ(run("SELECT * FROM users"), "id | name | email | age\n"
                                             "1 | park |  | 40\n"
                                             "(1 row)\n");
    run("INSERT INTO users (email) VALUES ('x@y');");
    CHECK_STR_EQ(run("SELECT id, age, name FROM users WHERE id = 2"), "id | age | name\n"
                                                                      "2 | 0 | \n"
                                                                      "(1 row)\n");
    executor_close(&ex);
}

/* TC-EXEC-003 EP: WHERE = (정수, 문자열), 맞는 행 없음, 컬럼 대소문자 무시 */
static void test_where_equal(void)
{
    fresh_db();
    run("INSERT INTO users VALUES ('kim', 'a', 20);"
        "INSERT INTO users VALUES ('lee', 'b', 20);"
        "INSERT INTO users VALUES ('kim', 'c', 30);");
    CHECK_STR_EQ(run("SELECT id FROM users WHERE name = 'kim'"), "id\n1\n3\n(2 rows)\n");
    CHECK_STR_EQ(run("SELECT id FROM users WHERE AGE = 20"), "id\n1\n2\n(2 rows)\n");
    CHECK_STR_EQ(run("SELECT id FROM users WHERE name = 'nobody'"), "id\n(0 rows)\n");
    CHECK_STR_EQ(run("SELECT id FROM users WHERE name = 'ki'"), "id\n(0 rows)\n"); /* 접두사 X */
    executor_close(&ex);
}

/* TC-EXEC-004 BVA: BETWEEN 은 양 끝 포함, low > high 면 0행 */
static void test_between(void)
{
    fresh_db();
    for (int i = 0; i < 10; i++) {
        run("INSERT INTO users VALUES ('u', 'e', 1);");
    }
    CHECK_STR_EQ(run("SELECT id FROM users WHERE id BETWEEN 3 AND 5"), "id\n3\n4\n5\n(3 rows)\n");
    CHECK_STR_EQ(run("SELECT id FROM users WHERE id BETWEEN 10 AND 99"), "id\n10\n(1 row)\n");
    CHECK_STR_EQ(run("SELECT id FROM users WHERE id BETWEEN 5 AND 3"), "id\n(0 rows)\n");
    CHECK_STR_EQ(run("SELECT id FROM users WHERE age BETWEEN 1 AND 1"),
                 "id\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n(10 rows)\n");
    executor_close(&ex);
}

/* TC-EXEC-005 DT: 스키마 대조 오류 — 문장마다 하나씩, 실패한 문장은 아무것도 쓰지 않는다 */
static void test_schema_errors(void)
{
    static const struct {
        const char *sql;
        const char *want;
    } cases[] = {
        {"SELECT * FROM nope", "ERROR: table 'nope' does not exist\n"},
        {"SELECT phone FROM users", "ERROR: unknown column 'phone' in table 'users'\n"},
        {"SELECT * FROM users WHERE phone = 1", "ERROR: unknown column 'phone' in table 'users'\n"},
        {"INSERT INTO users (phone) VALUES (1)", "ERROR: unknown column 'phone' in table 'users'\n"},
        {"INSERT INTO users (id, name) VALUES (9, 'x')", "ERROR: id is assigned automatically\n"},
        {"INSERT INTO users (name, name) VALUES ('a', 'b')", "ERROR: column 'name' given twice\n"},
        {"INSERT INTO users VALUES ('a', 'b')",
         "ERROR: table 'users' expects 3 values (id is automatic), got 2\n"},
        {"INSERT INTO users (age) VALUES ('old')", "ERROR: column 'age' expects INT\n"},
        {"INSERT INTO users (name) VALUES (7)", "ERROR: column 'name' expects a string\n"},
        {"SELECT * FROM users WHERE age = 'x'", "ERROR: column 'age' expects INT\n"},
        {"SELECT * FROM users WHERE name BETWEEN 1 AND 2",
         "ERROR: BETWEEN needs an INT column ('name' is VARCHAR)\n"},
    };

    fresh_db();
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CHECK_STR_EQ(run(cases[i].sql), cases[i].want);
        CHECK(failures == 1);
    }
    CHECK_STR_EQ(run("SELECT id FROM users"), "id\n(0 rows)\n");
    executor_close(&ex);
}

/* TC-EXEC-006 BVA: VARCHAR(32) 에 32바이트 통과, 33바이트 거절 (D001) */
static void test_varchar_limit(void)
{
    char sql[256];
    char name[40];

    fresh_db();
    memset(name, 'n', 32);
    name[32] = '\0';
    snprintf(sql, sizeof(sql), "INSERT INTO users (name) VALUES ('%s')", name);
    CHECK_STR_EQ(run(sql), "INSERT 1 (id = 1)\n");

    strcat(name, "n");
    snprintf(sql, sizeof(sql), "INSERT INTO users (name) VALUES ('%s')", name);
    CHECK_STR_EQ(run(sql), "ERROR: value too long for column 'name' (max 32 bytes)\n");
    executor_close(&ex);
}

/* TC-EXEC-007 EG: 실패한 INSERT 는 id 를 쓰지 않는다 (다음 id 가 이어진다) */
static void test_failed_insert_keeps_id(void)
{
    fresh_db();
    run("INSERT INTO users (name) VALUES ('a')");
    run("INSERT INTO users (age) VALUES ('bad')");
    CHECK_STR_EQ(run("INSERT INTO users (name) VALUES ('b')"), "INSERT 1 (id = 2)\n");
    executor_close(&ex);
}

/* TC-EXEC-008 EP: 여러 문장 중 하나가 틀려도 나머지를 실행하고, 실패 수를 센다 */
static void test_continue_after_error(void)
{
    fresh_db();
    CHECK_STR_EQ(run("INSERT INTO users (name) VALUES ('a');\n"
                     "SELECT * FORM users;\n"
                     "SELECT name FROM users;\n"),
                 "INSERT 1 (id = 1)\n"
                 "ERROR: syntax error near 'FORM': expected FROM\n"
                 "name\na\n(1 row)\n");
    CHECK(failures == 1);
    executor_close(&ex);
}

/* TC-EXEC-009 ST: 닫았다 다시 열면 데이터와 id 가 이어진다 */
static void test_reopen(void)
{
    fresh_db();
    run("INSERT INTO users (name) VALUES ('a'); INSERT INTO users (name) VALUES ('b');");
    executor_close(&ex);

    open_db();
    CHECK_STR_EQ(run("SELECT id, name FROM users"), "id | name\n1 | a\n2 | b\n(2 rows)\n");
    CHECK_STR_EQ(run("INSERT INTO users (name) VALUES ('c')"), "INSERT 1 (id = 3)\n");
    executor_close(&ex);
}

/* TC-EXEC-010 EG: 헤더의 next_id 가 실제보다 늦어도(flush 전에 죽음) 중복 id 를 내지 않는다 (D008) */
static void test_stale_header_next_id(void)
{
    char path[128];
    int64_t stale = 1;

    fresh_db();
    run("INSERT INTO users (name) VALUES ('a'); INSERT INTO users (name) VALUES ('b');");
    executor_close(&ex);

    snprintf(path, sizeof(path), "%s/users.tbl", data_dir);
    int fd = open(path, O_WRONLY);
    CHECK(fd >= 0);
    CHECK(pwrite(fd, &stale, sizeof(stale), 8) == sizeof(stale)); /* 헤더의 next_id 자리 */
    close(fd);

    open_db();
    CHECK_STR_EQ(run("INSERT INTO users (name) VALUES ('c')"), "INSERT 1 (id = 3)\n");
    executor_close(&ex);
}

/* TC-EXEC-011 EP: 빈 입력, 공백과 ; 뿐 */
static void test_empty_sql(void)
{
    fresh_db();
    CHECK_STR_EQ(run(""), "");
    CHECK_STR_EQ(run(" ;; \n"), "");
    CHECK(failures == 0);
    executor_close(&ex);
}

int main(void)
{
    RUN(test_insert_then_select);
    RUN(test_insert_partial_columns);
    RUN(test_where_equal);
    RUN(test_between);
    RUN(test_schema_errors);
    RUN(test_varchar_limit);
    RUN(test_failed_insert_keeps_id);
    RUN(test_continue_after_error);
    RUN(test_reopen);
    RUN(test_stale_header_next_id);
    RUN(test_empty_sql);
    free(out_buf);
    TEST_DONE();
    return 0;
}
