#include "test.h"

#include "parser.h"

/* 문장 하나를 파싱하고 에러 코드를 돌려준다. 성공했으면 호출한 쪽이 statement_free 한다 */
static ErrorCode parse_one(const char *sql, Statement *out_stmt)
{
    ParseError error_info;
    return parser_parse_single(sql, strlen(sql), out_stmt, &error_info);
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
    Statement stmt;
    ParseError error;

    CHECK(parser_parse_single(sql, strlen(sql), &stmt, &error) == ERR_SYNTAX);
    CHECK(error.line == 3);
    CHECK(strstr(error.message, "FORM") != NULL); /* 문제 토큰을 원문대로 보여준다 */
    statement_free(&stmt);
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
    CHECK(parse_error("SELECT * FROM users WHERE ? = name") == ERR_SYNTAX); /* E42-06 이름 자리 */
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

/* ---- S07: INSERT ---- */

static Statement parse_insert_ok(const char *sql)
{
    Statement stmt;
    CHECK(parse_one(sql, &stmt) == ERR_NONE);
    CHECK(stmt.type == STMT_INSERT);
    return stmt;
}

static void check_string_value(const Value *value, const char *expected)
{
    CHECK(value->type == VAL_STRING);
    CHECK(value->s.len == strlen(expected));
    CHECK_STR_EQ(value->s.ptr, expected);
}

/* T07-01 */ static void test_insert_basic(void)
{
    Statement stmt = parse_insert_ok("INSERT INTO users VALUES ('kim', 25)");
    CHECK_STR_EQ(stmt.insert.table, "users");
    CHECK(stmt.insert.value_count == 2);
    check_string_value(&stmt.insert.values[0], "kim");
    CHECK(stmt.insert.values[1].type == VAL_INT);
    CHECK(stmt.insert.values[1].i == 25);
    CHECK(stmt.param_count == 0);
    statement_free(&stmt);
}

/* T07-02 */ static void test_insert_single_value(void)
{
    Statement stmt = parse_insert_ok("INSERT INTO t VALUES (1);");
    CHECK_STR_EQ(stmt.insert.table, "t");
    CHECK(stmt.insert.value_count == 1);
    CHECK(stmt.insert.values[0].i == 1);
    statement_free(&stmt);
}

/* T07-03 */ static void test_insert_negative_and_escaped_string(void)
{
    Statement stmt = parse_insert_ok("INSERT INTO t VALUES ('it''s', -1)");
    CHECK(stmt.insert.value_count == 2);
    check_string_value(&stmt.insert.values[0], "it's");
    CHECK(stmt.insert.values[1].type == VAL_INT);
    CHECK(stmt.insert.values[1].i == -1);
    statement_free(&stmt);
}

/* T07-04 */ static void test_insert_placeholders(void)
{
    Statement stmt = parse_insert_ok("INSERT INTO t VALUES (?, ?)");
    CHECK(stmt.insert.value_count == 2);
    CHECK(stmt.insert.values[0].type == VAL_PARAM);
    CHECK(stmt.insert.values[0].param_index == 0);
    CHECK(stmt.insert.values[1].type == VAL_PARAM);
    CHECK(stmt.insert.values[1].param_index == 1);
    CHECK(stmt.param_count == 2);
    statement_free(&stmt);
}

/* T07-05 */ static void test_insert_split_lines(void)
{
    Statement stmt = parse_insert_ok("INSERT INTO users\nVALUES\n('kim', 25)");
    CHECK(stmt.line == 1);
    CHECK(stmt.insert.value_count == 2);
    statement_free(&stmt);
}

static void test_insert_max_values(void)
{
    Statement stmt = parse_insert_ok(
        "INSERT INTO t VALUES (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,'sixteen')");
    CHECK(stmt.insert.value_count == MAX_COLUMNS);
    check_string_value(&stmt.insert.values[MAX_COLUMNS - 1], "sixteen");
    statement_free(&stmt);
}

/* E07-01 */ static void test_insert_without_into(void)
{
    CHECK(parse_error("INSERT users VALUES (1)") == ERR_SYNTAX);
}

/* E07-02 */ static void test_insert_without_values(void)
{
    CHECK(parse_error("INSERT INTO users ('kim')") == ERR_UNSUPPORTED);
}

/* E07-03 */ static void test_insert_empty_parens(void)
{
    CHECK(parse_error("INSERT INTO t VALUES ()") == ERR_SYNTAX);
}

/* E07-04 */ static void test_insert_trailing_comma(void)
{
    CHECK(parse_error("INSERT INTO t VALUES ('kim',)") == ERR_SYNTAX);
}

/* E07-05 */ static void test_insert_missing_rparen(void)
{
    CHECK(parse_error("INSERT INTO t VALUES ('kim', 25") == ERR_SYNTAX);
    CHECK(parse_error("INSERT INTO t VALUES ('kim', 25;") == ERR_SYNTAX);
}

/* E07-06 */ static void test_insert_without_parens(void)
{
    CHECK(parse_error("INSERT INTO t VALUES 'kim', 25") == ERR_SYNTAX);
}

/* E07-07 */ static void test_insert_too_many_values(void)
{
    CHECK(parse_error("INSERT INTO t VALUES (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17)")
          == ERR_UNSUPPORTED);
    /* 문자열 값을 만든 뒤 실패해도 새지 않아야 한다 (ASan 으로 확인) */
    CHECK(parse_error("INSERT INTO t VALUES ('a','b','c','d','e','f','g','h',"
                      "'i','j','k','l','m','n','o','p','q')") == ERR_UNSUPPORTED);
}

/* E07-08 */ static void test_insert_identifier_as_value(void)
{
    CHECK(parse_error("INSERT INTO t VALUES (kim, 25)") == ERR_SYNTAX);
    CHECK(parse_error("INSERT INTO t VALUES ('lee', kim)") == ERR_SYNTAX);
}

/* E07-09 */ static void test_insert_multiple_rows(void)
{
    CHECK(parse_error("INSERT INTO t VALUES (1), (2)") == ERR_UNSUPPORTED);
}

/* E07-10 */ static void test_insert_column_list(void)
{
    CHECK(parse_error("INSERT INTO t (name) VALUES ('a')") == ERR_UNSUPPORTED);
}

static void test_insert_extra_token_after_values(void)
{
    CHECK(parse_error("INSERT INTO t VALUES (1) 2") == ERR_SYNTAX);
}

/* ---- S08: CREATE TABLE ---- */

static Statement parse_create_ok(const char *sql)
{
    Statement stmt;
    CHECK(parse_one(sql, &stmt) == ERR_NONE);
    CHECK(stmt.type == STMT_CREATE);
    return stmt;
}

static void check_column_def(const ColumnDef *column, const char *name, ColumnType type,
                             int size)
{
    CHECK_STR_EQ(column->name, name);
    CHECK(column->type == type);
    CHECK(column->size == size);
}

/* T08-01 */ static void test_create_basic(void)
{
    Statement stmt = parse_create_ok("CREATE TABLE users (id INT, name VARCHAR(32), age INT);");
    CHECK_STR_EQ(stmt.create.table, "users");
    CHECK(stmt.create.column_count == 3);
    check_column_def(&stmt.create.columns[0], "id", COL_INT, 4);
    check_column_def(&stmt.create.columns[1], "name", COL_VARCHAR, 32);
    check_column_def(&stmt.create.columns[2], "age", COL_INT, 4);
    CHECK(stmt.line == 1);
    statement_free(&stmt);
}

/* T08-02 */ static void test_create_only_id(void)
{
    Statement stmt = parse_create_ok("CREATE TABLE t (id INT)");
    CHECK(stmt.create.column_count == 1);
    check_column_def(&stmt.create.columns[0], "id", COL_INT, 4);
    statement_free(&stmt);
}

/* T08-03 */ static void test_create_names_are_lowercased(void)
{
    Statement stmt = parse_create_ok("create table T (ID int, Name varchar(8))");
    CHECK_STR_EQ(stmt.create.table, "t");
    check_column_def(&stmt.create.columns[0], "id", COL_INT, 4);
    check_column_def(&stmt.create.columns[1], "name", COL_VARCHAR, 8);
    statement_free(&stmt);
}

static void test_create_max_columns(void)
{
    Statement stmt = parse_create_ok("CREATE TABLE t (id INT, c1 INT, c2 INT, c3 INT, c4 INT,"
                                     " c5 INT, c6 INT, c7 INT, c8 INT, c9 INT, c10 INT,"
                                     " c11 INT, c12 INT, c13 INT, c14 INT, c15 VARCHAR(2))");
    CHECK(stmt.create.column_count == MAX_COLUMNS);
    check_column_def(&stmt.create.columns[MAX_COLUMNS - 1], "c15", COL_VARCHAR, 2);
    statement_free(&stmt);
}

/* E08-01 */ static void test_create_first_column_not_id(void)
{
    CHECK(parse_error("CREATE TABLE t (name VARCHAR(8), id INT)") == ERR_UNSUPPORTED);
}

/* E08-02 */ static void test_create_id_not_int(void)
{
    CHECK(parse_error("CREATE TABLE t (id VARCHAR(8))") == ERR_UNSUPPORTED);
}

/* E08-03 */ static void test_create_without_columns(void)
{
    CHECK(parse_error("CREATE TABLE t ()") == ERR_SYNTAX);
    CHECK(parse_error("CREATE TABLE t") == ERR_SYNTAX);
}

/* E08-04 */ static void test_create_too_many_columns(void)
{
    CHECK(parse_error("CREATE TABLE t (id INT, c1 INT, c2 INT, c3 INT, c4 INT, c5 INT,"
                      " c6 INT, c7 INT, c8 INT, c9 INT, c10 INT, c11 INT, c12 INT,"
                      " c13 INT, c14 INT, c15 INT, c16 INT)") == ERR_UNSUPPORTED);
}

/* E08-05 */ static void test_create_duplicate_column(void)
{
    CHECK(parse_error("CREATE TABLE t (id INT, a INT, A INT)") == ERR_SYNTAX);
    CHECK(parse_error("CREATE TABLE t (id INT, ID INT)") == ERR_SYNTAX);
}

/* E08-06 */ static void test_create_varchar_without_size(void)
{
    CHECK(parse_error("CREATE TABLE t (id INT, a VARCHAR)") == ERR_SYNTAX);
    CHECK(parse_error("CREATE TABLE t (id INT, a VARCHAR())") == ERR_SYNTAX);
    CHECK(parse_error("CREATE TABLE t (id INT, a VARCHAR(8)") == ERR_SYNTAX);
}

/* E08-07 */ static void test_create_varchar_zero(void)
{
    CHECK(parse_error("CREATE TABLE t (id INT, a VARCHAR(0))") == ERR_OUT_OF_RANGE);
}

/* E08-08 */ static void test_create_varchar_limit(void)
{
    Statement stmt = parse_create_ok("CREATE TABLE t (id INT, a VARCHAR(1024))");
    check_column_def(&stmt.create.columns[1], "a", COL_VARCHAR, MAX_VARCHAR);
    statement_free(&stmt);
    CHECK(parse_error("CREATE TABLE t (id INT, a VARCHAR(1025))") == ERR_OUT_OF_RANGE);
}

/* E08-09 */ static void test_create_varchar_negative(void)
{
    CHECK(parse_error("CREATE TABLE t (id INT, a VARCHAR(-1))") == ERR_SYNTAX);
    CHECK(parse_error("CREATE TABLE t (id INT, a VARCHAR('8'))") == ERR_SYNTAX);
}

/* E08-10 */ static void test_create_unsupported_type(void)
{
    CHECK(parse_error("CREATE TABLE t (id INT, f FLOAT)") == ERR_UNSUPPORTED);
    CHECK(parse_error("CREATE TABLE t (id INT, f)") == ERR_SYNTAX);
}

/* E08-11 */ static void test_create_without_table_keyword(void)
{
    CHECK(parse_error("CREATE users (id INT)") == ERR_SYNTAX);
}

static void test_create_extra_token_after_columns(void)
{
    CHECK(parse_error("CREATE TABLE t (id INT) x") == ERR_SYNTAX);
    CHECK(parse_error("CREATE TABLE t (id INT, select INT)") == ERR_SYNTAX);
}

/* ---- S09: 스크립트, 에러 복구 ---- */

#define MAX_SCRIPT_STATEMENTS 128
#define ERROR_STATEMENT_COUNT 100

/* 스크립트를 끝까지 돌며 문장마다 결과를 모은다 */
typedef struct {
    int count;
    ErrorCode errors[MAX_SCRIPT_STATEMENTS];
    int numbers[MAX_SCRIPT_STATEMENTS];
    int lines[MAX_SCRIPT_STATEMENTS];
    char tables[MAX_SCRIPT_STATEMENTS][MAX_NAME];
} ScriptRun;

static void record_statement(ScriptRun *run, const Parser *parser, const Statement *stmt,
                             ErrorCode error)
{
    int i = run->count++;
    run->errors[i] = error;
    run->numbers[i] = parser_statement_number(parser);
    run->lines[i] = error == ERR_NONE ? stmt->line : parser_error_line(parser);
    run->tables[i][0] = '\0';
    if (error == ERR_NONE && stmt->type == STMT_SELECT)
        snprintf(run->tables[i], MAX_NAME, "%s", stmt->select.table);
}

static ScriptRun run_script(const char *sql)
{
    ScriptRun run = {0};
    Parser *parser = parser_create(sql, strlen(sql));
    CHECK(parser != NULL);
    Statement stmt;
    ErrorCode error;
    while (run.count < MAX_SCRIPT_STATEMENTS && parser_next_statement(parser, &stmt, &error)) {
        record_statement(&run, parser, &stmt, error);
        statement_free(&stmt);
    }
    parser_destroy(parser);
    return run;
}

/* SELECT * FROM a WHERE name = 'xxx…' 를 첫 토큰부터 마지막 ' 까지 정확히 size 바이트로 만들고 suffix 를 붙인다 */
static char *make_sized_statement(size_t size, const char *suffix)
{
    const char *prefix = "SELECT * FROM a WHERE name = '";
    size_t prefix_len = strlen(prefix);
    size_t body_len = size - prefix_len - 1;
    char *sql = malloc(size + strlen(suffix) + 1);
    CHECK(sql != NULL);
    memcpy(sql, prefix, prefix_len);
    memset(sql + prefix_len, 'x', body_len);
    sql[prefix_len + body_len] = '\'';
    strcpy(sql + size, suffix);
    return sql;
}

/* T09-01 */ static void test_script_two_statements(void)
{
    ScriptRun run = run_script("SELECT * FROM a; SELECT * FROM b;");
    CHECK(run.count == 2);
    CHECK(run.errors[0] == ERR_NONE && run.errors[1] == ERR_NONE);
    CHECK(run.numbers[0] == 1 && run.numbers[1] == 2);
    CHECK_STR_EQ(run.tables[0], "a");
    CHECK_STR_EQ(run.tables[1], "b");
}

/* T09-02 */ static void test_script_last_semicolon_optional(void)
{
    ScriptRun run = run_script("SELECT * FROM a; SELECT * FROM b");
    CHECK(run.count == 2);
    CHECK(run.errors[1] == ERR_NONE);
    CHECK_STR_EQ(run.tables[1], "b");
}

/* T09-03 */ static void test_script_continues_after_error(void)
{
    ScriptRun run = run_script("SELEC x; SELECT * FROM b;");
    CHECK(run.count == 2);
    CHECK(run.errors[0] == ERR_SYNTAX);
    CHECK(run.errors[1] == ERR_NONE);
    CHECK(run.numbers[0] == 1 && run.numbers[1] == 2); /* 에러 문장도 번호를 받는다 */
    CHECK_STR_EQ(run.tables[1], "b");
}

/* T09-04 */ static void test_script_statement_start_line(void)
{
    ScriptRun run = run_script("\n\nSELECT * FROM a;\nSELECT *\nFROM b;");
    CHECK(run.count == 2);
    CHECK(run.lines[0] == 3);
    CHECK(run.lines[1] == 4);
}

static void test_script_error_line(void)
{
    ScriptRun run = run_script("SELECT * FROM a;\nSELECT *\nFORM b;");
    CHECK(run.count == 2);
    CHECK(run.errors[1] == ERR_SYNTAX);
    CHECK(run.numbers[1] == 2);
    CHECK(run.lines[1] == 3); /* 에러는 문제 토큰의 줄 */
}

/* T09-05 */ static void test_single_statement_mode(void)
{
    Statement stmt;
    CHECK(parse_one("SELECT * FROM a;", &stmt) == ERR_NONE);
    CHECK_STR_EQ(stmt.select.table, "a");
    statement_free(&stmt);
}

/* E09-01 */ static void test_script_empty(void)
{
    CHECK(run_script("").count == 0);
    CHECK(run_script("  \n\t ").count == 0);
}

/* E09-02 */ static void test_script_only_comment(void)
{
    CHECK(run_script("-- only comment").count == 0);
}

/* E09-03 */ static void test_script_only_semicolons(void)
{
    CHECK(run_script(";;;").count == 0);
    CHECK(run_script("; -- c\n ;").count == 0);
}

/* E09-04 */ static void test_script_empty_statement_between(void)
{
    ScriptRun run = run_script("SELECT * FROM a;;SELECT * FROM b;");
    CHECK(run.count == 2);
    CHECK(run.numbers[0] == 1 && run.numbers[1] == 2); /* 빈 문장은 번호를 받지 않는다 */
    CHECK(run.errors[0] == ERR_NONE && run.errors[1] == ERR_NONE);
}

/* E09-05 */ static void test_script_unterminated_string_swallows_rest(void)
{
    ScriptRun run = run_script("INSERT INTO t VALUES ('a); SELECT * FROM t;");
    CHECK(run.count == 1);
    CHECK(run.errors[0] == ERR_SYNTAX);
}

/* E09-06 */ static void test_script_too_large_then_normal(void)
{
    char *sql = make_sized_statement(MAX_SQL_LEN + 1, "; SELECT * FROM b;");
    ScriptRun run = run_script(sql);
    CHECK(run.count == 2);
    CHECK(run.errors[0] == ERR_TOO_LARGE);
    CHECK(run.errors[1] == ERR_NONE);
    CHECK(run.numbers[1] == 2);
    CHECK_STR_EQ(run.tables[1], "b");
    free(sql);
}

/* E09-07 */ static void test_script_exact_max_statement(void)
{
    char *sql = make_sized_statement(MAX_SQL_LEN, ";");
    ScriptRun run = run_script(sql);
    CHECK(run.count == 1);
    CHECK(run.errors[0] == ERR_NONE);
    free(sql);
}

/* E09-08 */ static void test_single_mode_rejects_second_statement(void)
{
    CHECK(parse_error("SELECT * FROM a; SELECT * FROM b") == ERR_UNSUPPORTED);
    CHECK(parse_error("SELECT * FROM a;; SELECT * FROM b;") == ERR_UNSUPPORTED);
    CHECK(parse_error("SELECT * FROM a; @") == ERR_UNSUPPORTED);
}

/* E09-09 */ static void test_single_mode_allows_trailing_semicolon_and_comment(void)
{
    Statement stmt;
    CHECK(parse_one("SELECT * FROM a; -- end", &stmt) == ERR_NONE);
    statement_free(&stmt);
    CHECK(parse_one("SELECT * FROM a;;\n", &stmt) == ERR_NONE);
    statement_free(&stmt);
}

static void test_single_mode_reports_error_location(void)
{
    const char *sql = "SELECT * FROM a;\nSELECT * FROM b";
    Statement stmt;
    ParseError error;
    CHECK(parser_parse_single(sql, strlen(sql), &stmt, &error) == ERR_UNSUPPORTED);
    CHECK(error.line == 2); /* 두 번째 문장이 시작한 줄 */
    CHECK(strstr(error.message, "SELECT") != NULL);
    statement_free(&stmt);

    sql = "SELECT * FROM a";
    CHECK(parser_parse_single(sql, strlen(sql), &stmt, &error) == ERR_NONE);
    CHECK(error.line == 0); /* 성공이면 에러 정보는 비어 있다 */
    CHECK(error.message[0] == '\0');
    statement_free(&stmt);
}

static void test_single_mode_needs_a_statement(void)
{
    CHECK(parse_error("") == ERR_SYNTAX);
    CHECK(parse_error(";") == ERR_SYNTAX);
    CHECK(parse_error("-- nothing") == ERR_SYNTAX);
}

/* E09-10 */ static void test_script_many_errors_do_not_leak(void)
{
    const char *bad = "INSERT INTO t VALUES ('leak', kim);\n";
    size_t bad_len = strlen(bad);
    char *sql = malloc(bad_len * ERROR_STATEMENT_COUNT + 1);
    CHECK(sql != NULL);
    for (int i = 0; i < ERROR_STATEMENT_COUNT; i++)
        memcpy(sql + bad_len * i, bad, bad_len);
    sql[bad_len * ERROR_STATEMENT_COUNT] = '\0';

    ScriptRun run = run_script(sql);
    CHECK(run.count == ERROR_STATEMENT_COUNT);
    CHECK(run.errors[ERROR_STATEMENT_COUNT - 1] == ERR_SYNTAX);
    CHECK(run.numbers[ERROR_STATEMENT_COUNT - 1] == ERROR_STATEMENT_COUNT);
    CHECK(run.lines[ERROR_STATEMENT_COUNT - 1] == ERROR_STATEMENT_COUNT);
    free(sql);
}

static void test_script_params_numbered_per_statement(void)
{
    const char *sql = "SELECT * FROM a WHERE id = ?; INSERT INTO a VALUES (?, ?)";
    Parser *parser = parser_create(sql, strlen(sql));
    CHECK(parser != NULL);
    Statement stmt;
    ErrorCode error;
    CHECK(parser_next_statement(parser, &stmt, &error) && error == ERR_NONE);
    statement_free(&stmt);
    CHECK(parser_next_statement(parser, &stmt, &error) && error == ERR_NONE);
    CHECK(stmt.param_count == 2);
    CHECK(stmt.insert.values[0].param_index == 0);
    statement_free(&stmt);
    CHECK(!parser_next_statement(parser, &stmt, &error));
    CHECK(!parser_next_statement(parser, &stmt, &error)); /* 끝에서는 계속 false */
    parser_destroy(parser);
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
    RUN(test_insert_basic);
    RUN(test_insert_single_value);
    RUN(test_insert_negative_and_escaped_string);
    RUN(test_insert_placeholders);
    RUN(test_insert_split_lines);
    RUN(test_insert_max_values);
    RUN(test_insert_without_into);
    RUN(test_insert_without_values);
    RUN(test_insert_empty_parens);
    RUN(test_insert_trailing_comma);
    RUN(test_insert_missing_rparen);
    RUN(test_insert_without_parens);
    RUN(test_insert_too_many_values);
    RUN(test_insert_identifier_as_value);
    RUN(test_insert_multiple_rows);
    RUN(test_insert_column_list);
    RUN(test_insert_extra_token_after_values);
    RUN(test_create_basic);
    RUN(test_create_only_id);
    RUN(test_create_names_are_lowercased);
    RUN(test_create_max_columns);
    RUN(test_create_first_column_not_id);
    RUN(test_create_id_not_int);
    RUN(test_create_without_columns);
    RUN(test_create_too_many_columns);
    RUN(test_create_duplicate_column);
    RUN(test_create_varchar_without_size);
    RUN(test_create_varchar_zero);
    RUN(test_create_varchar_limit);
    RUN(test_create_varchar_negative);
    RUN(test_create_unsupported_type);
    RUN(test_create_without_table_keyword);
    RUN(test_create_extra_token_after_columns);
    RUN(test_script_two_statements);
    RUN(test_script_last_semicolon_optional);
    RUN(test_script_continues_after_error);
    RUN(test_script_statement_start_line);
    RUN(test_script_error_line);
    RUN(test_single_statement_mode);
    RUN(test_script_empty);
    RUN(test_script_only_comment);
    RUN(test_script_only_semicolons);
    RUN(test_script_empty_statement_between);
    RUN(test_script_unterminated_string_swallows_rest);
    RUN(test_script_too_large_then_normal);
    RUN(test_script_exact_max_statement);
    RUN(test_single_mode_rejects_second_statement);
    RUN(test_single_mode_allows_trailing_semicolon_and_comment);
    RUN(test_single_mode_reports_error_location);
    RUN(test_single_mode_needs_a_statement);
    RUN(test_script_many_errors_do_not_leak);
    RUN(test_script_params_numbered_per_statement);
    TEST_DONE();
    return 0;
}
