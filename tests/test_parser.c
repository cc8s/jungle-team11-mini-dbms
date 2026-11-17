#include "test.h"

#include "parser.h"

/* 문장 하나를 파싱하고 에러 코드를 돌려준다. 성공했으면 호출한 쪽이 statement_free 한다 */
static ErrorCode parse_one(const char *sql, Statement *out_stmt)
{
    Parser *parser = parser_create(sql, strlen(sql));
    CHECK(parser != NULL);
    ErrorCode error = parser_parse_statement(parser, out_stmt);
    parser_destroy(parser);
    return error;
}

static ErrorCode parse_error(const char *sql)
{
    Statement stmt;
    ErrorCode error = parse_one(sql, &stmt);
    statement_free(&stmt); /* 실패한 문장도 해제가 안전해야 한다 */
    return error;
}

static Statement parse_select_ok(const char *sql)
{
    Statement stmt;
    CHECK(parse_one(sql, &stmt) == ERR_NONE);
    CHECK(stmt.type == STMT_SELECT);
    return stmt;
}

/* T05-01 */ static void test_select_all(void)
{
    Statement stmt = parse_select_ok("SELECT * FROM users");
    CHECK(stmt.select.select_all);
    CHECK(stmt.select.column_count == 0);
    CHECK_STR_EQ(stmt.select.table, "users");
    CHECK(!stmt.select.has_where);
    CHECK(!stmt.select.has_limit);
    CHECK(stmt.line == 1);
    statement_free(&stmt);
}

/* T05-02 */ static void test_select_column_list_keeps_order(void)
{
    Statement stmt = parse_select_ok("SELECT age, name FROM users");
    CHECK(!stmt.select.select_all);
    CHECK(stmt.select.column_count == 2);
    CHECK_STR_EQ(stmt.select.columns[0], "age");
    CHECK_STR_EQ(stmt.select.columns[1], "name");
    statement_free(&stmt);
}

/* T05-03 */ static void test_select_single_column(void)
{
    Statement stmt = parse_select_ok("SELECT name FROM users");
    CHECK(stmt.select.column_count == 1);
    CHECK_STR_EQ(stmt.select.columns[0], "name");
    statement_free(&stmt);
}

/* T05-04 */ static void test_select_repeated_column_is_kept(void)
{
    Statement stmt = parse_select_ok("SELECT name, name FROM users");
    CHECK(stmt.select.column_count == 2);
    CHECK_STR_EQ(stmt.select.columns[0], "name");
    CHECK_STR_EQ(stmt.select.columns[1], "name");
    statement_free(&stmt);
}

/* T05-05 */ static void test_select_names_are_lowercased(void)
{
    Statement stmt = parse_select_ok("SELECT NAME FROM USERS");
    CHECK_STR_EQ(stmt.select.columns[0], "name");
    CHECK_STR_EQ(stmt.select.table, "users");
    statement_free(&stmt);
}

static void test_select_with_semicolon_and_line(void)
{
    Statement stmt = parse_select_ok("\n\nSELECT * FROM users;");
    CHECK(stmt.line == 3);
    statement_free(&stmt);
}

/* E05-01 */ static void test_select_without_columns(void)
{
    CHECK(parse_error("SELECT FROM users") == ERR_SYNTAX);
}

/* E05-02 */ static void test_select_trailing_comma(void)
{
    CHECK(parse_error("SELECT name, FROM users") == ERR_SYNTAX);
}

/* E05-03 */ static void test_select_star_mixed_with_columns(void)
{
    CHECK(parse_error("SELECT *, name FROM users") == ERR_SYNTAX);
}

/* E05-04 */ static void test_select_without_table(void)
{
    CHECK(parse_error("SELECT * FROM") == ERR_SYNTAX);
}

/* E05-05 */ static void test_select_from_typo_reports_token_and_line(void)
{
    const char *sql = "SELECT *\n\nFORM users";
    Parser *parser = parser_create(sql, strlen(sql));
    CHECK(parser != NULL);
    Statement stmt;

    CHECK(parser_parse_statement(parser, &stmt) == ERR_SYNTAX);
    CHECK(parser_error_line(parser) == 3);
    CHECK(strstr(parser_error_message(parser), "FORM") != NULL); /* 문제 토큰을 원문대로 보여준다 */
    statement_free(&stmt);
    parser_destroy(parser);
}

/* E05-06 */ static void test_select_keyword_as_table(void)
{
    CHECK(parse_error("SELECT * FROM select") == ERR_SYNTAX);
}

/* E05-07 */ static void test_select_too_many_columns(void)
{
    CHECK(parse_error("SELECT a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p FROM t") == ERR_NONE);
    CHECK(parse_error("SELECT a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q FROM t") == ERR_UNSUPPORTED);
}

/* E05-08 */ static void test_select_extra_token_after_table(void)
{
    CHECK(parse_error("SELECT * FROM users extra") == ERR_SYNTAX);
}

/* E05-09 */ static void test_select_unsupported_clause(void)
{
    CHECK(parse_error("SELECT * FROM users ORDER BY id") == ERR_UNSUPPORTED);
}

/* 토크나이저 에러는 그 코드 그대로 전달된다 */
static void test_tokenizer_error_is_reported(void)
{
    CHECK(parse_error("SELECT @ FROM users") == ERR_SYNTAX);
    CHECK(parse_error("SELECT * FROM users <> 1") == ERR_UNSUPPORTED);
}

static void test_unsupported_statement(void)
{
    CHECK(parse_error("UPDATE users") == ERR_UNSUPPORTED);
    CHECK(parse_error("DELETE FROM users") == ERR_UNSUPPORTED);
}

/* ---- S06: WHERE, LIMIT ---- */

/* T06-01 */ static void test_where_int_condition(void)
{
    Statement stmt = parse_select_ok("SELECT * FROM users WHERE id = 3");
    CHECK(stmt.select.has_where);
    CHECK_STR_EQ(stmt.select.where.column, "id");
    CHECK(stmt.select.where.op == OP_EQ);
    CHECK(stmt.select.where.value.type == VAL_INT);
    CHECK(stmt.select.where.value.i == 3);
    CHECK(!stmt.select.has_limit);
    statement_free(&stmt);
}

/* T06-02 */ static void test_where_string_condition(void)
{
    Statement stmt = parse_select_ok("SELECT * FROM users WHERE name = 'it''s'");
    CHECK(stmt.select.where.value.type == VAL_STRING);
    CHECK(stmt.select.where.value.s.len == 4);
    CHECK(memcmp(stmt.select.where.value.s.ptr, "it's", 4) == 0);
    statement_free(&stmt);
}

/* T06-03 */ static void test_where_all_operators(void)
{
    const struct {
        const char *sql;
        CompareOp op;
    } cases[] = {
        { "SELECT * FROM t WHERE a = 1", OP_EQ },  { "SELECT * FROM t WHERE a != 1", OP_NE },
        { "SELECT * FROM t WHERE a < 1", OP_LT },  { "SELECT * FROM t WHERE a > 1", OP_GT },
        { "SELECT * FROM t WHERE a <= 1", OP_LE }, { "SELECT * FROM t WHERE a >= 1", OP_GE },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Statement stmt = parse_select_ok(cases[i].sql);
        CHECK(stmt.select.where.op == cases[i].op);
        statement_free(&stmt);
    }
}

/* T06-04 */ static void test_limit(void)
{
    Statement stmt = parse_select_ok("SELECT * FROM users LIMIT 10");
    CHECK(!stmt.select.has_where);
    CHECK(stmt.select.has_limit);
    CHECK(stmt.select.limit == 10);
    statement_free(&stmt);
}

/* T06-05 */ static void test_where_and_limit(void)
{
    Statement stmt = parse_select_ok("SELECT name FROM users WHERE age > 20 LIMIT 5;");
    CHECK(stmt.select.has_where);
    CHECK_STR_EQ(stmt.select.where.column, "age");
    CHECK(stmt.select.where.op == OP_GT);
    CHECK(stmt.select.where.value.i == 20);
    CHECK(stmt.select.has_limit);
    CHECK(stmt.select.limit == 5);
    statement_free(&stmt);
}

/* T06-06 */ static void test_where_placeholder(void)
{
    Statement stmt = parse_select_ok("SELECT * FROM users WHERE id = ?");
    CHECK(stmt.select.where.value.type == VAL_PARAM);
    CHECK(stmt.select.where.value.param_index == 0);
    CHECK(stmt.param_count == 1);
    statement_free(&stmt);
}

static void test_statement_without_placeholder_has_no_params(void)
{
    Statement stmt = parse_select_ok("SELECT * FROM users WHERE id = 1");
    CHECK(stmt.param_count == 0);
    statement_free(&stmt);
}

/* E06-01 */ static void test_where_without_condition(void)
{
    CHECK(parse_error("SELECT * FROM users WHERE") == ERR_SYNTAX);
}

/* E06-02 */ static void test_where_without_value(void)
{
    CHECK(parse_error("SELECT * FROM users WHERE id =") == ERR_SYNTAX);
}

/* E06-03 */ static void test_where_without_operator(void)
{
    CHECK(parse_error("SELECT * FROM users WHERE id 3") == ERR_SYNTAX);
}

/* E06-04 */ static void test_where_value_on_left(void)
{
    CHECK(parse_error("SELECT * FROM users WHERE 3 = id") == ERR_UNSUPPORTED);
    CHECK(parse_error("SELECT * FROM users WHERE 'kim' = name") == ERR_UNSUPPORTED);
}

/* E06-05 */ static void test_where_column_to_column(void)
{
    CHECK(parse_error("SELECT * FROM users WHERE id = age") == ERR_UNSUPPORTED);
}

/* E06-06 */ static void test_where_and(void)
{
    CHECK(parse_error("SELECT * FROM users WHERE id = 1 AND age = 2") == ERR_UNSUPPORTED);
    /* 문자열 값을 만든 뒤 실패해도 누수가 없어야 한다 (ASan) */
    CHECK(parse_error("SELECT * FROM users WHERE name = 'kim' AND age = 2") == ERR_UNSUPPORTED);
}

/* E06-07 */ static void test_where_or(void)
{
    CHECK(parse_error("SELECT * FROM users WHERE id = 1 OR id = 2") == ERR_UNSUPPORTED);
}

/* E06-08 */ static void test_limit_zero(void)
{
    Statement stmt = parse_select_ok("SELECT * FROM users LIMIT 0");
    CHECK(stmt.select.has_limit);
    CHECK(stmt.select.limit == 0);
    statement_free(&stmt);
}

/* E06-09 */ static void test_limit_negative(void)
{
    CHECK(parse_error("SELECT * FROM users LIMIT -1") == ERR_SYNTAX);
}

/* E06-10 */ static void test_limit_without_value(void)
{
    CHECK(parse_error("SELECT * FROM users LIMIT") == ERR_SYNTAX);
}

/* E06-11 */ static void test_limit_string(void)
{
    CHECK(parse_error("SELECT * FROM users LIMIT '5'") == ERR_SYNTAX);
}

/* E06-12 */ static void test_limit_before_where(void)
{
    CHECK(parse_error("SELECT * FROM users LIMIT 5 WHERE id = 1") == ERR_SYNTAX);
}

/* E06-13 */ static void test_where_twice(void)
{
    CHECK(parse_error("SELECT * FROM users WHERE id = 1 WHERE id = 2") == ERR_SYNTAX);
}

/* E06-14 */ static void test_where_value_hidden_by_comment(void)
{
    CHECK(parse_error("SELECT * FROM users WHERE age = --5") == ERR_SYNTAX);
}

int main(void)
{
    RUN(test_select_all);
    RUN(test_select_column_list_keeps_order);
    RUN(test_select_single_column);
    RUN(test_select_repeated_column_is_kept);
    RUN(test_select_names_are_lowercased);
    RUN(test_select_with_semicolon_and_line);
    RUN(test_select_without_columns);
    RUN(test_select_trailing_comma);
    RUN(test_select_star_mixed_with_columns);
    RUN(test_select_without_table);
    RUN(test_select_from_typo_reports_token_and_line);
    RUN(test_select_keyword_as_table);
    RUN(test_select_too_many_columns);
    RUN(test_select_extra_token_after_table);
    RUN(test_select_unsupported_clause);
    RUN(test_tokenizer_error_is_reported);
    RUN(test_unsupported_statement);

    RUN(test_where_int_condition);
    RUN(test_where_string_condition);
    RUN(test_where_all_operators);
    RUN(test_limit);
    RUN(test_where_and_limit);
    RUN(test_where_placeholder);
    RUN(test_statement_without_placeholder_has_no_params);
    RUN(test_where_without_condition);
    RUN(test_where_without_value);
    RUN(test_where_without_operator);
    RUN(test_where_value_on_left);
    RUN(test_where_column_to_column);
    RUN(test_where_and);
    RUN(test_where_or);
    RUN(test_limit_zero);
    RUN(test_limit_negative);
    RUN(test_limit_without_value);
    RUN(test_limit_string);
    RUN(test_limit_before_where);
    RUN(test_where_twice);
    RUN(test_where_value_hidden_by_comment);
    TEST_DONE();
    return 0;
}
