#include "test.h"
#include "test_util.h"

#include <fcntl.h>
#include <unistd.h>

#include "parser.h"
#include "table.h"
#include "validator.h"

#define USERS_SQL "CREATE TABLE users (id INT, name VARCHAR(8), age INT)"
#define ORDERS_SQL "CREATE TABLE orders (id INT, item VARCHAR(4))"
#define TABLE_COUNT 2
#define USERS_COLUMN_COUNT 3
#define NAME_OFFSET 4
#define AGE_OFFSET 12
#define MAX_FILE_BYTES 4096

/* 임시 디렉터리에 users, orders 테이블을 만들어 연다 */
typedef struct {
    char *dir;
    char users_path[TEST_PATH_MAX];
    Table *tables[TABLE_COUNT];
} Schema;

static Table *create_and_open(const char *dir, const char *sql, char *out_path)
{
    Statement stmt;
    ParseError parse_error;
    CHECK(parser_parse_single(sql, strlen(sql), &stmt, &parse_error) == ERR_NONE);
    CHECK(table_build_path(dir, stmt.create.table, out_path, TEST_PATH_MAX));
    CHECK(table_create(out_path, &stmt.create) == ERR_NONE);
    ErrorCode error;
    Table *table = table_open(out_path, &error);
    CHECK(table != NULL);
    return table;
}

static Schema schema_open(void)
{
    Schema schema;
    schema.dir = test_make_temp_dir();
    CHECK(schema.dir != NULL);
    char orders_path[TEST_PATH_MAX];
    schema.tables[0] = create_and_open(schema.dir, USERS_SQL, schema.users_path);
    schema.tables[1] = create_and_open(schema.dir, ORDERS_SQL, orders_path);
    return schema;
}

static void schema_close(Schema *schema)
{
    for (int i = 0; i < TABLE_COUNT; i++)
        table_close(schema->tables[i]);
    test_remove_dir(schema->dir);
    free(schema->dir);
}

/* sql 을 파싱해 검증하고 에러 코드를 돌려준다. 성공하면 out_plan 을 채운다 */
static ErrorCode validate_sql(const Schema *schema, const char *sql, Plan *out_plan)
{
    Statement stmt;
    ParseError parse_error;
    CHECK(parser_parse_single(sql, strlen(sql), &stmt, &parse_error) == ERR_NONE);
    char message[MAX_MESSAGE];
    ErrorCode error = validator_check(&stmt, schema->tables, TABLE_COUNT, out_plan, message);
    CHECK(error == ERR_NONE ? message[0] == '\0' : message[0] != '\0'); /* 실패면 설명이 있다 */
    statement_free(&stmt);
    return error;
}

static ErrorCode validate(const char *sql)
{
    Schema schema = schema_open();
    Plan plan;
    ErrorCode error = validate_sql(&schema, sql, &plan);
    schema_close(&schema);
    return error;
}

static void check_plan_column(const Plan *plan, int i, const char *name, ColumnType type,
                              int size, int offset, int table_index)
{
    CHECK_STR_EQ(plan->columns[i].name, name);
    CHECK(plan->columns[i].type == type);
    CHECK(plan->columns[i].size == size);
    CHECK(plan->columns[i].offset == offset);
    CHECK(plan->column_indexes[i] == table_index);
}

/* T13-01 */ static void test_valid_insert(void)
{
    Schema schema = schema_open();
    Plan plan;
    CHECK(validate_sql(&schema, "INSERT INTO users VALUES ('kim', 25)", &plan) == ERR_NONE);
    CHECK(plan.table == schema.tables[0]);
    CHECK(validate_sql(&schema, "INSERT INTO orders VALUES ('pen')", &plan) == ERR_NONE);
    CHECK(plan.table == schema.tables[1]);
    schema_close(&schema);
}

/* T13-02 */ static void test_valid_select(void)
{
    Schema schema = schema_open();
    Plan plan;
    CHECK(validate_sql(&schema, "SELECT * FROM users", &plan) == ERR_NONE);
    CHECK(plan.table == schema.tables[0]);
    CHECK(plan.column_count == USERS_COLUMN_COUNT);
    check_plan_column(&plan, 0, "id", COL_INT, 4, 0, 0);
    check_plan_column(&plan, 1, "name", COL_VARCHAR, 8, NAME_OFFSET, 1);
    check_plan_column(&plan, 2, "age", COL_INT, 4, AGE_OFFSET, 2);
    CHECK(!plan.has_where);

    CHECK(validate_sql(&schema, "SELECT age, name, age FROM users WHERE name = 'kim'", &plan)
          == ERR_NONE);
    CHECK(plan.column_count == 3); /* 순서와 중복을 그대로 */
    check_plan_column(&plan, 0, "age", COL_INT, 4, AGE_OFFSET, 2);
    check_plan_column(&plan, 1, "name", COL_VARCHAR, 8, NAME_OFFSET, 1);
    check_plan_column(&plan, 2, "age", COL_INT, 4, AGE_OFFSET, 2);
    CHECK(plan.has_where);
    CHECK(plan.where_column == 1);
    CHECK(plan.where_offset == NAME_OFFSET);
    schema_close(&schema);
}

/* E13-01, E13-02 */ static void test_unknown_table(void)
{
    CHECK(validate("SELECT * FROM user") == ERR_UNKNOWN_TABLE);
    CHECK(validate("INSERT INTO user VALUES ('kim', 25)") == ERR_UNKNOWN_TABLE);
    CHECK(validate("SELECT * FROM USERS") == ERR_NONE);
}

/* E13-03, E13-04 */ static void test_unknown_column(void)
{
    CHECK(validate("SELECT nmae FROM users") == ERR_UNKNOWN_COLUMN);
    CHECK(validate("SELECT name, nmae FROM users") == ERR_UNKNOWN_COLUMN);
    CHECK(validate("SELECT * FROM users WHERE nmae = 'a'") == ERR_UNKNOWN_COLUMN);
    CHECK(validate("SELECT item FROM users") == ERR_UNKNOWN_COLUMN); /* 다른 테이블의 컬럼 */
}

/* E13-05, E13-06, E13-07 */ static void test_insert_value_count(void)
{
    CHECK(validate("INSERT INTO users VALUES ('kim')") == ERR_VALUE_COUNT);
    CHECK(validate("INSERT INTO users VALUES (1, 'kim', 25)") == ERR_AUTO_ID);
    CHECK(validate("INSERT INTO users VALUES (1, 2, 'kim', 25)") == ERR_VALUE_COUNT);
}

/* E13-08, E13-09 */ static void test_insert_type_mismatch(void)
{
    CHECK(validate("INSERT INTO users VALUES ('kim', '25')") == ERR_TYPE_MISMATCH);
    CHECK(validate("INSERT INTO users VALUES (123, 25)") == ERR_TYPE_MISMATCH);
}

/* E13-10, E13-11, E13-12 */ static void test_insert_varchar_length(void)
{
    CHECK(validate("INSERT INTO users VALUES ('abcdefgh', 1)") == ERR_NONE);
    CHECK(validate("INSERT INTO users VALUES ('abcdefghi', 1)") == ERR_VALUE_TOO_LONG);
    CHECK(validate("INSERT INTO users VALUES ('김철수', 1)") == ERR_VALUE_TOO_LONG);
    CHECK(validate("INSERT INTO users VALUES ('', 1)") == ERR_NONE);
}

/* E13-13, E13-14 */ static void test_where_type_mismatch(void)
{
    CHECK(validate("SELECT * FROM users WHERE age = 'x'") == ERR_TYPE_MISMATCH);
    CHECK(validate("SELECT * FROM users WHERE name = 3") == ERR_TYPE_MISMATCH);
    CHECK(validate("SELECT * FROM users WHERE id = '1'") == ERR_TYPE_MISMATCH);
    CHECK(validate("SELECT * FROM users WHERE id >= 1 LIMIT 2") == ERR_NONE);
    /* 칸보다 긴 문자열과 비교하는 것은 에러가 아니다 (맞는 행이 없을 뿐) */
    CHECK(validate("SELECT * FROM users WHERE name = 'abcdefghijk'") == ERR_NONE);
}

/* E13-15 */ static void test_placeholder_on_cli_path(void)
{
    CHECK(validate("SELECT * FROM users WHERE id = ?") == ERR_PARAM);
    CHECK(validate("INSERT INTO users VALUES (?, 25)") == ERR_PARAM);
}

/* E13-16 */ static void test_create_on_normal_path(void)
{
    CHECK(validate("CREATE TABLE x (id INT)") == ERR_UNSUPPORTED);
    CHECK(validate("CREATE TABLE users (id INT)") == ERR_UNSUPPORTED);
}

static size_t read_file(const char *path, uint8_t *bytes)
{
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    ssize_t n = pread(fd, bytes, MAX_FILE_BYTES, 0);
    CHECK(n >= 0);
    close(fd);
    return (size_t)n;
}

/* E13-17 */ static void test_failed_validation_leaves_file(void)
{
    Schema schema = schema_open();
    uint8_t before[MAX_FILE_BYTES];
    uint8_t after[MAX_FILE_BYTES];
    size_t before_len = read_file(schema.users_path, before);

    const char *bad[] = {
        "INSERT INTO users VALUES ('kim')",
        "INSERT INTO users VALUES (1, 'kim', 25)",
        "INSERT INTO users VALUES ('kim', '25')",
        "INSERT INTO users VALUES ('abcdefghi', 1)",
        "INSERT INTO users VALUES (?, 1)",
        "SELECT nmae FROM users",
        "CREATE TABLE users (id INT)",
    };
    Plan plan;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(validate_sql(&schema, bad[i], &plan) != ERR_NONE);

    CHECK(read_file(schema.users_path, after) == before_len);
    CHECK(memcmp(before, after, before_len) == 0);
    CHECK(table_record_count(schema.tables[0]) == 0);
    schema_close(&schema);
}

static void test_find_table(void)
{
    Schema schema = schema_open();
    CHECK(validator_find_table(schema.tables, TABLE_COUNT, "orders") == schema.tables[1]);
    CHECK(validator_find_table(schema.tables, TABLE_COUNT, "nope") == NULL);
    CHECK(validator_find_table(NULL, 0, "users") == NULL); /* 테이블 없는 엔진 */
    schema_close(&schema);
}

int main(void)
{
    RUN(test_valid_insert);
    RUN(test_valid_select);
    RUN(test_unknown_table);
    RUN(test_unknown_column);
    RUN(test_insert_value_count);
    RUN(test_insert_type_mismatch);
    RUN(test_insert_varchar_length);
    RUN(test_where_type_mismatch);
    RUN(test_placeholder_on_cli_path);
    RUN(test_create_on_normal_path);
    RUN(test_failed_validation_leaves_file);
    RUN(test_find_table);
    TEST_DONE();
    return 0;
}
