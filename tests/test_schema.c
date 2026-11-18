#include "row.h"
#include "schema.h"
#include "test.h"

static struct schema s;
static char err[256];

#define USERS_SCHEMA                                                                           \
    "# 예시\n"                                                                                 \
    "id INT\n"                                                                                 \
    "name VARCHAR(8)   # 이름\n"                                                               \
    "\n"                                                                                       \
    "age int\n"

/* TC-SCHEMA-001 EP: 컬럼 순서, 크기, 오프셋, 행 크기. 주석·빈 줄·소문자 타입 */
static void test_parse_layout(void)
{
    CHECK(schema_parse(USERS_SCHEMA, "users", &s, err, sizeof(err)) == 0);
    CHECK_STR_EQ(s.table, "users");
    CHECK(s.column_count == 3);
    CHECK_STR_EQ(s.columns[1].name, "name");
    CHECK(s.columns[0].type == COLUMN_INT && s.columns[0].offset == 0 && s.columns[0].size == 8);
    CHECK(s.columns[1].type == COLUMN_VARCHAR && s.columns[1].offset == 8 &&
          s.columns[1].size == 8);
    CHECK(s.columns[2].type == COLUMN_INT && s.columns[2].offset == 16);
    CHECK(s.row_size == 24);
}

/* TC-SCHEMA-002 EP: 컬럼 찾기는 대소문자 무시 */
static void test_find_column(void)
{
    CHECK(schema_parse(USERS_SCHEMA, "users", &s, err, sizeof(err)) == 0);
    CHECK(schema_find_column(&s, "AGE") == 2);
    CHECK(schema_find_column(&s, "email") == -1);
}

/* TC-SCHEMA-003 EG: 첫 컬럼이 id INT 가 아니면 오류 */
static void test_first_column_must_be_id(void)
{
    CHECK(schema_parse("name VARCHAR(8)\nid INT\n", "t", &s, err, sizeof(err)) < 0);
    CHECK(strstr(err, "id INT") != NULL);
    CHECK(schema_parse("id VARCHAR(8)\n", "t", &s, err, sizeof(err)) < 0);
}

/* TC-SCHEMA-004 BVA: VARCHAR 길이 1, 255 통과 / 0, 256, 없음 오류 */
static void test_varchar_length(void)
{
    CHECK(schema_parse("id INT\na VARCHAR(1)\nb VARCHAR(255)\n", "t", &s, err, sizeof(err)) == 0);
    CHECK(s.row_size == 8 + 1 + 255);
    CHECK(schema_parse("id INT\na VARCHAR(0)\n", "t", &s, err, sizeof(err)) < 0);
    CHECK(schema_parse("id INT\na VARCHAR(256)\n", "t", &s, err, sizeof(err)) < 0);
    CHECK(schema_parse("id INT\na VARCHAR\n", "t", &s, err, sizeof(err)) < 0);
    CHECK(schema_parse("id INT\na VARCHAR(8\n", "t", &s, err, sizeof(err)) < 0);
}

/* TC-SCHEMA-005 EG: 알 수 없는 타입, 중복 컬럼, 남는 토큰, 줄 번호 */
static void test_bad_lines(void)
{
    CHECK(schema_parse("id INT\nx FLOAT\n", "t", &s, err, sizeof(err)) < 0);
    CHECK(strstr(err, "line 2") != NULL);
    CHECK(schema_parse("id INT\nid INT\n", "t", &s, err, sizeof(err)) < 0);
    CHECK(strstr(err, "duplicate") != NULL);
    CHECK(schema_parse("id INT extra\n", "t", &s, err, sizeof(err)) < 0);
    CHECK(schema_parse("# 비어 있음\n\n", "t", &s, err, sizeof(err)) < 0);
}

/* TC-SCHEMA-006 BVA: 컬럼 16개 통과, 17개 오류 */
static void test_column_limit(void)
{
    char text[512] = "id INT\n";
    char line[32];

    for (int i = 1; i < SCHEMA_MAX_COLUMNS; i++) {
        snprintf(line, sizeof(line), "c%d INT\n", i);
        strcat(text, line);
    }
    CHECK(schema_parse(text, "t", &s, err, sizeof(err)) == 0);
    CHECK(s.column_count == SCHEMA_MAX_COLUMNS);
    strcat(text, "extra INT\n");
    CHECK(schema_parse(text, "t", &s, err, sizeof(err)) < 0);
}

/* TC-SCHEMA-007 EG: 경로를 벗어나는 테이블 이름, 없는 테이블 */
static void test_load_errors(void)
{
    CHECK(schema_load("schema", "../etc/passwd", &s, err, sizeof(err)) < 0);
    CHECK(strstr(err, "invalid table name") != NULL);
    CHECK(schema_load("schema", "no_such_table", &s, err, sizeof(err)) < 0);
    CHECK(strstr(err, "does not exist") != NULL);
}

/* TC-SCHEMA-008 EP: 레포에 들어 있는 users 스키마를 읽는다 */
static void test_load_users(void)
{
    CHECK(schema_load("schema", "users", &s, err, sizeof(err)) == 0);
    CHECK(schema_find_column(&s, "id") == 0);
    CHECK(schema_find_column(&s, "name") > 0);
}

/* TC-ROW-001 EP: INT 와 VARCHAR 를 쓰고 그대로 읽는다 */
static void test_row_round_trip(void)
{
    char row[64];
    char name[16];

    CHECK(schema_parse(USERS_SCHEMA, "users", &s, err, sizeof(err)) == 0);
    memset(row, 0xAB, sizeof(row));
    row_set_int(&s, 0, row, 42);
    CHECK(row_set_string(&s, 1, row, "kim") == 0);
    row_set_int(&s, 2, row, -7);
    CHECK(row_get_int(&s, 0, row) == 42);
    CHECK(row_get_int(&s, 2, row) == -7);
    row_get_string(&s, 1, row, name, sizeof(name));
    CHECK_STR_EQ(name, "kim");
    CHECK(row[8 + 3] == 0 && row[8 + 7] == 0); /* 남는 자리는 0 */
}

/* TC-ROW-002 BVA: VARCHAR(8) 에 8바이트 통과(끝 NUL 없음), 9바이트 거절(D001) */
static void test_row_string_limit(void)
{
    char row[64] = {0};
    char name[16];

    CHECK(schema_parse(USERS_SCHEMA, "users", &s, err, sizeof(err)) == 0);
    row_set_int(&s, 2, row, 99);
    CHECK(row_set_string(&s, 1, row, "abcdefgh") == 0);
    row_get_string(&s, 1, row, name, sizeof(name));
    CHECK_STR_EQ(name, "abcdefgh");
    CHECK(row_get_int(&s, 2, row) == 99); /* 옆 컬럼을 넘어 쓰지 않았다 */

    CHECK(row_set_string(&s, 1, row, "abcdefghi") < 0);
    row_get_string(&s, 1, row, name, sizeof(name));
    CHECK_STR_EQ(name, "abcdefgh"); /* 거절하면 원래 값 그대로 */
}

/* TC-ROW-003 EG: VARCHAR 는 바이트 기준. 한글 3글자 = 9바이트 */
static void test_row_utf8_bytes(void)
{
    char row[64] = {0};

    CHECK(schema_parse(USERS_SCHEMA, "users", &s, err, sizeof(err)) == 0);
    CHECK(row_set_string(&s, 1, row, "김정") == 0);   /* 6바이트 */
    CHECK(row_set_string(&s, 1, row, "김정글") < 0); /* 9바이트 */
}

int main(void)
{
    RUN(test_parse_layout);
    RUN(test_find_column);
    RUN(test_first_column_must_be_id);
    RUN(test_varchar_length);
    RUN(test_bad_lines);
    RUN(test_column_limit);
    RUN(test_load_errors);
    RUN(test_load_users);
    RUN(test_row_round_trip);
    RUN(test_row_string_limit);
    RUN(test_row_utf8_bytes);
    TEST_DONE();
    return 0;
}
