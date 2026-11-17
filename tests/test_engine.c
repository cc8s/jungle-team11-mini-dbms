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

/* ---- S15: SELECT ---- */

#define BATCH_EDGE_ROWS 257  /* 스캔 묶음(256) 경계를 넘는 행 수 */
#define MANY_ROWS 10000
#define USERS_NAME_COLUMN 1
#define INSERT_SQL_MAX 128

/* kim 25, lee 30, park 22 를 넣은 users 테이블 */
static TestDb open_people(void)
{
    TestDb db = open_db(USERS_SQL);
    insert_ok(db.engine, "INSERT INTO users VALUES ('kim', 25)");
    insert_ok(db.engine, "INSERT INTO users VALUES ('lee', 30)");
    insert_ok(db.engine, "INSERT INTO users VALUES ('park', 22)");
    return db;
}

static Result select_ok(Engine *engine, const char *sql)
{
    Result result = engine_execute(engine, sql, NULL, 0);
    CHECK(result.error == ERR_NONE);
    CHECK(!result.used_index);
    return result;
}

static int64_t select_count(Engine *engine, const char *sql)
{
    Result result = select_ok(engine, sql);
    int64_t count = result.row_count;
    result_free(&result);
    return count;
}

static void check_cell_str(const Result *result, int64_t row, int col, const char *expected)
{
    size_t len = 0;
    const char *text = result_get_str(result, row, col, &len);
    CHECK(len == strlen(expected));
    CHECK(memcmp(text, expected, len) == 0);
}

/* 결과의 name 열(col)이 names 와 순서대로 같은지 */
static void check_names(Engine *engine, const char *sql, int col, const char *const *names,
                        int count)
{
    Result result = select_ok(engine, sql);
    CHECK(result.row_count == count);
    for (int i = 0; i < count; i++)
        check_cell_str(&result, i, col, names[i]);
    result_free(&result);
}

/* T15-01 */ static void test_select_all(void)
{
    TestDb db = open_people();
    Result result = select_ok(db.engine, "SELECT * FROM users");
    CHECK(result.row_count == 3);
    CHECK(result.column_count == 3);
    CHECK_STR_EQ(result.columns[0].name, "id");
    CHECK_STR_EQ(result.columns[1].name, "name");
    CHECK_STR_EQ(result.columns[2].name, "age");
    for (int i = 0; i < 3; i++)
        CHECK(result_get_int(&result, i, 0) == i + 1);
    check_cell_str(&result, 2, 1, "park");
    CHECK(result_get_int(&result, 2, 2) == 22);
    CHECK(result.affected_rows == 0);
    result_free(&result);
    test_db_destroy(&db);
}

/* T15-02 */ static void test_select_columns_in_order(void)
{
    TestDb db = open_people();
    Result result = select_ok(db.engine, "SELECT age, name FROM users");
    CHECK(result.column_count == 2);
    CHECK_STR_EQ(result.columns[0].name, "age");
    CHECK(result.columns[0].type == COL_INT);
    CHECK_STR_EQ(result.columns[1].name, "name");
    CHECK(result.columns[1].type == COL_VARCHAR);
    CHECK(result.columns[1].size == 8);
    CHECK(result_get_int(&result, 1, 0) == 30);
    check_cell_str(&result, 1, 1, "lee");
    result_free(&result);
    test_db_destroy(&db);
}

/* T15-03, T15-04, T15-06 */ static void test_select_where(void)
{
    TestDb db = open_people();
    const char *lee[] = { "lee" };
    check_names(db.engine, "SELECT * FROM users WHERE id = 2", USERS_NAME_COLUMN, lee, 1);
    const char *over_24[] = { "kim", "lee" };
    check_names(db.engine, "SELECT * FROM users WHERE age > 24", USERS_NAME_COLUMN, over_24, 2);
    const char *park[] = { "park" };
    check_names(db.engine, "SELECT name FROM users WHERE name = 'park'", 0, park, 1);
    test_db_destroy(&db);
}

/* T15-05, E15-11 */ static void test_select_all_operators(void)
{
    TestDb db = open_people(); /* age: 25, 30, 22 */
    CHECK(select_count(db.engine, "SELECT * FROM users WHERE age = 25") == 1);
    CHECK(select_count(db.engine, "SELECT * FROM users WHERE age != 25") == 2);
    CHECK(select_count(db.engine, "SELECT * FROM users WHERE age < 25") == 1);
    CHECK(select_count(db.engine, "SELECT * FROM users WHERE age > 25") == 1);
    CHECK(select_count(db.engine, "SELECT * FROM users WHERE age <= 25") == 2);
    CHECK(select_count(db.engine, "SELECT * FROM users WHERE age >= 25") == 2);
    CHECK(select_count(db.engine, "SELECT * FROM users WHERE id != 2") == 2);
    test_db_destroy(&db);
}

/* T15-07, E15-03, E15-04 */ static void test_select_limit(void)
{
    TestDb db = open_people();
    const char *first_two[] = { "kim", "lee" };
    check_names(db.engine, "SELECT * FROM users LIMIT 2", USERS_NAME_COLUMN, first_two, 2);
    CHECK(select_count(db.engine, "SELECT * FROM users LIMIT 0") == 0);
    CHECK(select_count(db.engine, "SELECT * FROM users LIMIT 100") == 3);
    const char *young[] = { "kim" };
    check_names(db.engine, "SELECT * FROM users WHERE age < 30 LIMIT 1", USERS_NAME_COLUMN,
                young, 1);
    test_db_destroy(&db);
}

/* E15-01 */ static void test_select_empty_table(void)
{
    TestDb db = open_db(USERS_SQL);
    Result result = select_ok(db.engine, "SELECT name FROM users");
    CHECK(result.row_count == 0);
    CHECK(result.column_count == 1);
    CHECK_STR_EQ(result.columns[0].name, "name");
    result_free(&result);
    test_db_destroy(&db);
}

/* E15-02, E15-05 */ static void test_select_no_match(void)
{
    TestDb db = open_people();
    CHECK(select_count(db.engine, "SELECT * FROM users WHERE age > 100") == 0);
    CHECK(select_count(db.engine, "SELECT * FROM users WHERE name = 'KIM'") == 0);
    CHECK(select_count(db.engine, "SELECT * FROM users WHERE name = 'kimchi_long'") == 0);
    test_db_destroy(&db);
}

/* E15-06, E15-07 */ static void test_select_string_compare(void)
{
    TestDb db = open_db("CREATE TABLE words (id INT, w VARCHAR(4));");
    insert_ok(db.engine, "INSERT INTO words VALUES ('ab')");
    insert_ok(db.engine, "INSERT INTO words VALUES ('abcd')");
    insert_ok(db.engine, "INSERT INTO words VALUES ('b')");
    CHECK(select_count(db.engine, "SELECT * FROM words WHERE w < 'abc'") == 1);   /* ab */
    CHECK(select_count(db.engine, "SELECT * FROM words WHERE w > 'abc'") == 2);   /* abcd, b */
    CHECK(select_count(db.engine, "SELECT * FROM words WHERE w = 'abcd'") == 1);  /* 꽉 찬 칸 */
    CHECK(select_count(db.engine, "SELECT * FROM words WHERE w = 'abc'") == 0);
    CHECK(select_count(db.engine, "SELECT * FROM words WHERE w >= 'ab'") == 3);
    CHECK(select_count(db.engine, "SELECT * FROM words WHERE w < ''") == 0);
    test_db_destroy(&db);
}

/* E15-08 */ static void test_select_negative(void)
{
    TestDb db = open_people();
    insert_ok(db.engine, "INSERT INTO users VALUES ('neg', -1)");
    const char *neg[] = { "neg" };
    check_names(db.engine, "SELECT * FROM users WHERE age < 0", USERS_NAME_COLUMN, neg, 1);
    test_db_destroy(&db);
}

static void insert_many(Engine *engine, int count)
{
    for (int i = 0; i < count; i++) {
        char sql[INSERT_SQL_MAX];
        snprintf(sql, sizeof(sql), "INSERT INTO users VALUES ('u%d', %d)", i, i);
        insert_ok(engine, sql);
    }
}

/* E15-09 */ static void test_select_across_batch_boundary(void)
{
    TestDb db = open_db(USERS_SQL);
    insert_many(db.engine, BATCH_EDGE_ROWS);
    Result result = select_ok(db.engine, "SELECT * FROM users");
    CHECK(result.row_count == BATCH_EDGE_ROWS);
    for (int i = 0; i < BATCH_EDGE_ROWS; i++)
        CHECK(result_get_int(&result, i, 0) == i + 1);
    result_free(&result);
    result = select_ok(db.engine, "SELECT * FROM users WHERE id = 257");
    CHECK(result.row_count == 1);
    check_cell_str(&result, 0, USERS_NAME_COLUMN, "u256");
    result_free(&result);
    test_db_destroy(&db);
}

/* E15-10 */ static void test_select_same_column_twice(void)
{
    TestDb db = open_people();
    Result result = select_ok(db.engine, "SELECT name, name FROM users WHERE id = 1");
    CHECK(result.column_count == 2);
    check_cell_str(&result, 0, 0, "kim");
    check_cell_str(&result, 0, 1, "kim");
    result_free(&result);
    test_db_destroy(&db);
}

/* E15-12 */ static void test_select_many_rows(void)
{
    TestDb db = open_db(USERS_SQL);
    insert_many(db.engine, MANY_ROWS);
    Result result = select_ok(db.engine, "SELECT * FROM users");
    CHECK(result.row_count == MANY_ROWS);
    CHECK(result_get_int(&result, MANY_ROWS - 1, 0) == MANY_ROWS);
    CHECK(result_get_int(&result, MANY_ROWS - 1, 2) == MANY_ROWS - 1);
    result_free(&result);
    CHECK(select_count(db.engine, "SELECT * FROM users WHERE age >= 5000") == MANY_ROWS / 2);
    test_db_destroy(&db);
}

static void test_select_persists_after_reopen(void)
{
    TestDb db = open_people();
    test_db_reopen(&db);
    CHECK(db.engine != NULL);
    CHECK(select_count(db.engine, "SELECT * FROM users") == 3);
    test_db_destroy(&db);
}

/* ---- S16: 스크립트 실행 ---- */

#define MAX_SCRIPT_RESULTS 16
#define MAX_TABLE_FILE 8192

/* 콜백이 받은 결과를 모아 둔다. Result 는 콜백이 끝나면 엔진이 해제하므로 필요한 값만 복사한다 */
typedef struct {
    int count;
    ErrorCode errors[MAX_SCRIPT_RESULTS];
    int indexes[MAX_SCRIPT_RESULTS];
    int lines[MAX_SCRIPT_RESULTS];
    int64_t row_counts[MAX_SCRIPT_RESULTS];
    int32_t last_ids[MAX_SCRIPT_RESULTS];
} ScriptLog;

static void record_result(const Result *result, int stmt_index, void *ctx)
{
    ScriptLog *log = ctx;
    CHECK(log->count < MAX_SCRIPT_RESULTS);
    int i = log->count++;
    log->errors[i] = result->error;
    log->indexes[i] = stmt_index;
    log->lines[i] = result->line;
    log->row_counts[i] = result->row_count;
    log->last_ids[i] = result->last_insert_id;
    CHECK(result->elapsed_ms >= 0);
}

static ScriptLog run_script(Engine *engine, const char *script, int *out_error_count)
{
    ScriptLog log = { 0 };
    *out_error_count = engine_execute_script(engine, script, record_result, &log);
    return log;
}

static size_t read_table_file(const TestDb *db, const char *table, uint8_t *bytes)
{
    char path[TEST_PATH_MAX];
    CHECK(table_build_path(db->dir, table, path, sizeof(path)));
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    ssize_t n = pread(fd, bytes, MAX_TABLE_FILE, 0);
    CHECK(n >= 0);
    close(fd);
    return (size_t)n;
}

/* T16-01, T16-02, T16-03 */ static void test_script_insert_then_select(void)
{
    TestDb db = open_db(USERS_SQL);
    int errors = -1;
    ScriptLog log = run_script(db.engine,
                               "INSERT INTO users VALUES ('kim', 25);\n"
                               "INSERT INTO users VALUES ('lee', 30);\n"
                               "SELECT * FROM users WHERE age > 20;\n",
                               &errors);
    CHECK(errors == 0);
    CHECK(log.count == 3);
    CHECK(log.last_ids[0] == 1 && log.last_ids[1] == 2);
    CHECK(log.row_counts[2] == 2); /* 앞 문장의 INSERT 가 보인다 */
    for (int i = 0; i < 3; i++) {
        CHECK(log.errors[i] == ERR_NONE);
        CHECK(log.indexes[i] == i + 1);
    }
    test_db_destroy(&db);
}

/* E16-01 */ static void test_script_empty(void)
{
    TestDb db = open_db(USERS_SQL);
    int errors = -1;
    CHECK(run_script(db.engine, "", &errors).count == 0);
    CHECK(errors == 0);
    CHECK(run_script(db.engine, " ;; -- only comment\n", &errors).count == 0);
    CHECK(errors == 0);
    test_db_destroy(&db);
}

/* E16-02, E16-03 */ static void test_script_mixed_errors(void)
{
    TestDb db = open_db(USERS_SQL);
    int errors = -1;
    ScriptLog log = run_script(db.engine,
                               "INSERT INTO users VALUES ('kim', 25);\n"
                               "SELEC * FROM users;\n"
                               "\n"
                               "INSERT INTO users\n  VALUES ('lee');\n"
                               "SELECT * FROM users;",
                               &errors);
    CHECK(errors == 2);
    CHECK(log.count == 4);
    CHECK(log.errors[0] == ERR_NONE);
    CHECK(log.errors[1] == ERR_SYNTAX);
    CHECK(log.errors[2] == ERR_VALUE_COUNT);
    CHECK(log.errors[3] == ERR_NONE);
    for (int i = 0; i < 4; i++)
        CHECK(log.indexes[i] == i + 1);
    CHECK(log.lines[1] == 2); /* 문법 에러: 문제 토큰 SELEC 의 줄 */
    CHECK(log.lines[2] == 4); /* 검증 에러: 문장 시작 줄 */
    CHECK(log.row_counts[3] == 1);
    test_db_destroy(&db);
}

/* E16-04 */ static void test_script_all_errors_leave_file(void)
{
    TestDb db = open_db(USERS_SQL);
    insert_ok(db.engine, "INSERT INTO users VALUES ('kim', 25)");
    uint8_t before[MAX_TABLE_FILE];
    uint8_t after[MAX_TABLE_FILE];
    size_t before_len = read_table_file(&db, "users", before);

    int errors = -1;
    ScriptLog log = run_script(db.engine,
                               "INSERT INTO users VALUES ('kim');"
                               "INSERT INTO nope VALUES (1);"
                               "INSERT INTO users VALUES (1, 'x', 2);"
                               "SELECT nmae FROM users;"
                               "INSERT INTO users VALUES ('kim', '25');",
                               &errors);
    CHECK(errors == 5);
    CHECK(log.count == 5);
    CHECK(read_table_file(&db, "users", after) == before_len);
    CHECK(memcmp(before, after, before_len) == 0);
    CHECK(insert_ok(db.engine, "INSERT INTO users VALUES ('lee', 30)") == 2);
    test_db_destroy(&db);
}

static void test_script_create_is_rejected(void)
{
    TestDb db = open_db(USERS_SQL);
    int errors = -1;
    ScriptLog log = run_script(db.engine, "CREATE TABLE t (id INT); SELECT * FROM users",
                               &errors);
    CHECK(errors == 1);
    CHECK(log.errors[0] == ERR_UNSUPPORTED);
    CHECK(log.errors[1] == ERR_NONE);
    test_db_destroy(&db);
}

static void test_script_without_callback(void)
{
    TestDb db = open_db(USERS_SQL);
    CHECK(engine_execute_script(db.engine, "INSERT INTO users VALUES ('kim', 25); SELEC",
                                NULL, NULL) == 1);
    CHECK(select_count(db.engine, "SELECT * FROM users") == 1);
    test_db_destroy(&db);
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
    RUN(test_select_all);
    RUN(test_select_columns_in_order);
    RUN(test_select_where);
    RUN(test_select_all_operators);
    RUN(test_select_limit);
    RUN(test_select_empty_table);
    RUN(test_select_no_match);
    RUN(test_select_string_compare);
    RUN(test_select_negative);
    RUN(test_select_across_batch_boundary);
    RUN(test_select_same_column_twice);
    RUN(test_select_many_rows);
    RUN(test_select_persists_after_reopen);
    RUN(test_script_insert_then_select);
    RUN(test_script_empty);
    RUN(test_script_mixed_errors);
    RUN(test_script_all_errors_leave_file);
    RUN(test_script_create_is_rejected);
    RUN(test_script_without_callback);
    TEST_DONE();
    return 0;
}
