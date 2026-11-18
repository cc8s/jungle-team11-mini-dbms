#include "parser.h"
#include "test.h"

static struct statement stmt;
static char err[256];

static enum parse_result parse(const char *sql)
{
    size_t consumed;

    return parser_parse(sql, &consumed, &stmt, err, sizeof(err));
}

/* TC-PARSE-001 EP: 컬럼 목록이 있는 INSERT */
static void test_insert_with_columns(void)
{
    CHECK(parse("INSERT INTO users (name, age) VALUES ('kim', 20);") == PARSE_OK);
    CHECK(stmt.type == STATEMENT_INSERT);
    CHECK_STR_EQ(stmt.table, "users");
    CHECK(stmt.column_count == 2);
    CHECK_STR_EQ(stmt.columns[0], "name");
    CHECK_STR_EQ(stmt.columns[1], "age");
    CHECK(stmt.value_count == 2);
    CHECK(stmt.values[0].type == SQL_VALUE_STRING);
    CHECK_STR_EQ(stmt.values[0].str_value, "kim");
    CHECK(stmt.values[1].type == SQL_VALUE_INT && stmt.values[1].int_value == 20);
}

/* TC-PARSE-002 EP: 컬럼 목록 없는 INSERT, 소문자 키워드, ';' 없이 끝 */
static void test_insert_without_columns(void)
{
    CHECK(parse("insert into users values ('lee', 31)") == PARSE_OK);
    CHECK(stmt.type == STATEMENT_INSERT);
    CHECK(stmt.column_count == 0);
    CHECK(stmt.value_count == 2);
}

/* TC-PARSE-003 EP: SELECT * 와 컬럼 목록 */
static void test_select_columns(void)
{
    CHECK(parse("SELECT * FROM users") == PARSE_OK);
    CHECK(stmt.type == STATEMENT_SELECT && stmt.is_select_all);
    CHECK(stmt.where.type == WHERE_NONE);

    CHECK(parse("SELECT id, name FROM users;") == PARSE_OK);
    CHECK(!stmt.is_select_all && stmt.column_count == 2);
    CHECK_STR_EQ(stmt.columns[1], "name");
}

/* TC-PARSE-004 EP: WHERE c = v (정수, 문자열) */
static void test_where_equal(void)
{
    CHECK(parse("SELECT * FROM users WHERE id = 3") == PARSE_OK);
    CHECK(stmt.where.type == WHERE_EQUAL);
    CHECK_STR_EQ(stmt.where.column, "id");
    CHECK(stmt.where.value.type == SQL_VALUE_INT && stmt.where.value.int_value == 3);

    CHECK(parse("SELECT name FROM users WHERE name = 'kim'") == PARSE_OK);
    CHECK(stmt.where.value.type == SQL_VALUE_STRING);
    CHECK_STR_EQ(stmt.where.value.str_value, "kim");
}

/* TC-PARSE-005 EP: WHERE id BETWEEN a AND b */
static void test_where_between(void)
{
    CHECK(parse("SELECT * FROM users WHERE id BETWEEN 10 AND 20;") == PARSE_OK);
    CHECK(stmt.where.type == WHERE_BETWEEN);
    CHECK_STR_EQ(stmt.where.column, "id");
    CHECK(stmt.where.low == 10 && stmt.where.high == 20);
}

/* TC-PARSE-006 EG: 오타 키워드는 근처 토큰을 짚는다 (D006 오류 예시) */
static void test_syntax_error_near(void)
{
    CHECK(parse("SELECT * FORM users") == PARSE_ERROR);
    CHECK(strstr(err, "near 'FORM'") != NULL);

    CHECK(parse("DELETE FROM users") == PARSE_ERROR);
    CHECK(strstr(err, "near 'DELETE'") != NULL);
}

/* TC-PARSE-007 EG: 입력이 중간에 끝남 */
static void test_unexpected_end(void)
{
    CHECK(parse("SELECT * FROM") == PARSE_ERROR);
    CHECK(strstr(err, "end of input") != NULL);

    CHECK(parse("INSERT INTO users VALUES ('a', 1") == PARSE_ERROR);
}

/* TC-PARSE-008 EG: 컬럼 수와 값 수가 다름 */
static void test_column_value_mismatch(void)
{
    CHECK(parse("INSERT INTO users (name, age) VALUES ('kim')") == PARSE_ERROR);
    CHECK(strstr(err, "does not match") != NULL);
}

/* TC-PARSE-009 EG: 문장 뒤에 남는 토큰, BETWEEN 에 문자열 */
static void test_trailing_and_bad_between(void)
{
    CHECK(parse("SELECT * FROM users extra") == PARSE_ERROR);
    CHECK(strstr(err, "near 'extra'") != NULL);

    CHECK(parse("SELECT * FROM users WHERE id BETWEEN 'a' AND 3") == PARSE_ERROR);
}

/* TC-PARSE-010 EG: 토크나이저 오류가 그대로 전달된다 */
static void test_token_error(void)
{
    CHECK(parse("INSERT INTO users VALUES ('kim)") == PARSE_ERROR);
    CHECK(strstr(err, "unterminated string") != NULL);
}

/* TC-PARSE-011 EP: 여러 문장을 consumed 로 이어 읽기. 빈 문장은 건너뛴다 */
static void test_multiple_statements(void)
{
    const char *sql = "INSERT INTO t VALUES (1);;\n SELECT * FROM t;  \n";
    size_t consumed;

    CHECK(parser_parse(sql, &consumed, &stmt, err, sizeof(err)) == PARSE_OK);
    CHECK(stmt.type == STATEMENT_INSERT);
    sql += consumed;
    CHECK(parser_parse(sql, &consumed, &stmt, err, sizeof(err)) == PARSE_OK);
    CHECK(stmt.type == STATEMENT_SELECT);
    sql += consumed;
    CHECK(parser_parse(sql, &consumed, &stmt, err, sizeof(err)) == PARSE_EMPTY);
}

/* TC-PARSE-012 EG: 실패한 문장 뒤의 문장은 계속 읽을 수 있다 */
static void test_recover_after_error(void)
{
    const char *sql = "SELECT * FORM t; SELECT * FROM t;";
    size_t consumed;

    CHECK(parser_parse(sql, &consumed, &stmt, err, sizeof(err)) == PARSE_ERROR);
    sql += consumed;
    CHECK(parser_parse(sql, &consumed, &stmt, err, sizeof(err)) == PARSE_OK);
    CHECK(stmt.type == STATEMENT_SELECT);
}

/* TC-PARSE-013 BVA: 컬럼 16개는 통과, 17개는 오류 */
static void test_column_limit(void)
{
    char sql[512] = "SELECT c0";
    char col[16];

    for (int i = 1; i < SQL_MAX_COLUMNS; i++) {
        snprintf(col, sizeof(col), ", c%d", i);
        strcat(sql, col);
    }
    strcat(sql, " FROM t");
    CHECK(parse(sql) == PARSE_OK);
    CHECK(stmt.column_count == SQL_MAX_COLUMNS);

    strcpy(strstr(sql, " FROM"), ", extra FROM t");
    CHECK(parse(sql) == PARSE_ERROR);
    CHECK(strstr(err, "too many columns") != NULL);
}

/* TC-PARSE-014 EP: 공백과 ; 뿐인 입력은 빈 문장 */
static void test_empty_input(void)
{
    CHECK(parse("") == PARSE_EMPTY);
    CHECK(parse("  ;\n; ") == PARSE_EMPTY);
}

int main(void)
{
    RUN(test_insert_with_columns);
    RUN(test_insert_without_columns);
    RUN(test_select_columns);
    RUN(test_where_equal);
    RUN(test_where_between);
    RUN(test_syntax_error_near);
    RUN(test_unexpected_end);
    RUN(test_column_value_mismatch);
    RUN(test_trailing_and_bad_between);
    RUN(test_token_error);
    RUN(test_multiple_statements);
    RUN(test_recover_after_error);
    RUN(test_column_limit);
    RUN(test_empty_input);
    TEST_DONE();
    return 0;
}
