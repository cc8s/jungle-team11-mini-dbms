#include "test.h"

#include "tokenizer.h"

#define LONGEST_IDENT_LEN (MAX_NAME - 1)
#define TOO_LONG_IDENT_LEN MAX_NAME

typedef struct {
    ErrorCode error;
    int line;
} TokenizeFailure;

/* 다음 토큰을 읽고 성공했는지 확인한다 */
static Token next_ok(Tokenizer *tokenizer)
{
    Token token;
    CHECK(tokenizer_next(tokenizer, &token) == ERR_NONE);
    return token;
}

/* 입력의 토큰을 EOF 나 첫 에러까지 읽고 첫 에러를 돌려준다 (에러가 없으면 ERR_NONE) */
static TokenizeFailure first_failure(const char *sql, size_t len)
{
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, sql, len);
    Token token;
    for (;;) {
        ErrorCode error = tokenizer_next(&tokenizer, &token);
        if (error != ERR_NONE)
            return (TokenizeFailure){ .error = error, .line = token.line };
        if (token.type == TOK_EOF)
            return (TokenizeFailure){ .error = ERR_NONE, .line = token.line };
    }
}

static ErrorCode first_error(const char *sql)
{
    return first_failure(sql, strlen(sql)).error;
}

static void check_only_eof(const char *sql)
{
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, sql, strlen(sql));
    CHECK(next_ok(&tokenizer).type == TOK_EOF);
}

/* T03-01 */ static void test_keywords_ignore_case(void)
{
    const char *sql = "select SeLeCt SELECT";
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, sql, strlen(sql));

    for (int i = 0; i < 3; i++) {
        Token token = next_ok(&tokenizer);
        CHECK(token.type == TOK_KEYWORD);
        CHECK(token.keyword == KW_SELECT);
    }
    CHECK(next_ok(&tokenizer).type == TOK_EOF);
}

static void test_every_keyword_is_recognized(void)
{
    const char *sql = "SELECT FROM WHERE LIMIT INSERT INTO VALUES CREATE TABLE INT VARCHAR AND OR";
    const Keyword expected[] = { KW_SELECT, KW_FROM,  KW_WHERE, KW_LIMIT, KW_INSERT,
                                 KW_INTO,   KW_VALUES, KW_CREATE, KW_TABLE, KW_INT,
                                 KW_VARCHAR, KW_AND,   KW_OR };
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, sql, strlen(sql));

    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        Token token = next_ok(&tokenizer);
        CHECK(token.type == TOK_KEYWORD);
        CHECK(token.keyword == expected[i]);
    }
    CHECK(next_ok(&tokenizer).type == TOK_EOF);
}

/* T03-02 */ static void test_identifier_is_lowercased(void)
{
    const char *sql = "Users";
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, sql, strlen(sql));

    Token token = next_ok(&tokenizer);
    CHECK(token.type == TOK_IDENT);
    CHECK_STR_EQ(token.text, "users");
    /* 에러 메시지용 원문은 대소문자를 그대로 가리킨다 */
    CHECK(token.start == sql);
    CHECK(token.len == 5);
}

/* T03-03 */ static void test_symbols_and_comparisons(void)
{
    const char *sql = "( ) , ; * = != < > <= >=";
    const TokenType expected[] = { TOK_LPAREN, TOK_RPAREN, TOK_COMMA, TOK_SEMI,
                                   TOK_STAR,   TOK_EQ,     TOK_NE,    TOK_LT,
                                   TOK_GT,     TOK_LE,     TOK_GE };
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, sql, strlen(sql));

    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
        CHECK(next_ok(&tokenizer).type == expected[i]);
    CHECK(next_ok(&tokenizer).type == TOK_EOF);
}

/* T03-04 */ static void test_adjacent_tokens(void)
{
    const char *sql = "age>=20;";
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, sql, strlen(sql));

    Token column = next_ok(&tokenizer);
    CHECK(column.type == TOK_IDENT);
    CHECK_STR_EQ(column.text, "age");
    CHECK(next_ok(&tokenizer).type == TOK_GE);
    Token number = next_ok(&tokenizer);
    CHECK(number.type == TOK_INT);
    CHECK(number.int_value == 20);
    CHECK(next_ok(&tokenizer).type == TOK_SEMI);
    CHECK(next_ok(&tokenizer).type == TOK_EOF);
}

/* T03-05 */ static void test_tokens_record_line(void)
{
    const char *sql = "SELECT\n*\nFROM";
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, sql, strlen(sql));

    CHECK(next_ok(&tokenizer).line == 1);
    CHECK(next_ok(&tokenizer).line == 2);
    CHECK(next_ok(&tokenizer).line == 3);
}

/* T03-06 */ static void test_last_token_is_eof(void)
{
    const char *sql = "SELECT";
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, sql, strlen(sql));

    CHECK(next_ok(&tokenizer).type == TOK_KEYWORD);
    CHECK(next_ok(&tokenizer).type == TOK_EOF);
    CHECK(next_ok(&tokenizer).type == TOK_EOF); /* 끝에서 계속 불러도 EOF */
}

/* E03-01 */ static void test_empty_input_is_eof(void)
{
    check_only_eof("");
}

/* E03-02 */ static void test_whitespace_only_is_eof(void)
{
    check_only_eof("  \t\n ");
}

/* E03-03 */ static void test_crlf_counts_as_one_line(void)
{
    const char *sql = "SELECT\r\n*";
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, sql, strlen(sql));

    CHECK(next_ok(&tokenizer).line == 1);
    Token star = next_ok(&tokenizer);
    CHECK(star.type == TOK_STAR);
    CHECK(star.line == 2);
}

/* E03-04 */ static void test_identifier_of_max_length(void)
{
    char sql[LONGEST_IDENT_LEN + 1];
    memset(sql, 'a', LONGEST_IDENT_LEN);
    sql[LONGEST_IDENT_LEN] = '\0';
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, sql, strlen(sql));

    Token token = next_ok(&tokenizer);
    CHECK(token.type == TOK_IDENT);
    CHECK(strlen(token.text) == LONGEST_IDENT_LEN);
}

/* E03-05 */ static void test_identifier_too_long(void)
{
    char sql[TOO_LONG_IDENT_LEN + 1];
    memset(sql, 'a', TOO_LONG_IDENT_LEN);
    sql[TOO_LONG_IDENT_LEN] = '\0';

    CHECK(first_error(sql) == ERR_SYNTAX);
}

/* E03-06 */ static void test_identifier_with_underscore_and_digit(void)
{
    const char *sql = "_col1";
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, sql, strlen(sql));

    Token token = next_ok(&tokenizer);
    CHECK(token.type == TOK_IDENT);
    CHECK_STR_EQ(token.text, "_col1");
}

/* E03-07 */ static void test_identifier_starting_with_digit(void)
{
    CHECK(first_error("1abc") == ERR_SYNTAX);
}

/* E03-08 */ static void test_lone_bang(void)
{
    CHECK(first_error("!") == ERR_SYNTAX);
}

/* E03-09 */ static void test_angle_not_equal_is_unsupported(void)
{
    CHECK(first_error("<>") == ERR_UNSUPPORTED);
}

/* E03-10 */ static void test_double_quote(void)
{
    CHECK(first_error("\"kim\"") == ERR_SYNTAX);
}

/* E03-11 */ static void test_unknown_characters_report_line(void)
{
    const char *inputs[] = { "SELECT\n@", "SELECT\n#", "SELECT\n`" };
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        TokenizeFailure failure = first_failure(inputs[i], strlen(inputs[i]));
        CHECK(failure.error == ERR_SYNTAX);
        CHECK(failure.line == 2);
    }
}

/* E03-12 */ static void test_non_ascii_identifier(void)
{
    CHECK(first_error("이름") == ERR_SYNTAX);
    CHECK(first_error("name이름") == ERR_SYNTAX);
}

/* E03-13 */ static void test_nul_byte_in_input(void)
{
    const char sql[] = { 'S', 'E', 'L', '\0', 'E', 'C', 'T' };
    CHECK(first_failure(sql, sizeof(sql)).error == ERR_SYNTAX);
}

/* 에러가 나도 문제 문자를 건너뛰어 다음 토큰을 읽을 수 있어야 한다 (S09 에러 복구의 전제) */
static void test_tokenizer_advances_after_error(void)
{
    const char *sql = "@ ;";
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, sql, strlen(sql));

    Token token;
    CHECK(tokenizer_next(&tokenizer, &token) == ERR_SYNTAX);
    CHECK(tokenizer.message[0] != '\0');
    CHECK(next_ok(&tokenizer).type == TOK_SEMI);
}

int main(void)
{
    RUN(test_keywords_ignore_case);
    RUN(test_every_keyword_is_recognized);
    RUN(test_identifier_is_lowercased);
    RUN(test_symbols_and_comparisons);
    RUN(test_adjacent_tokens);
    RUN(test_tokens_record_line);
    RUN(test_last_token_is_eof);
    RUN(test_empty_input_is_eof);
    RUN(test_whitespace_only_is_eof);
    RUN(test_crlf_counts_as_one_line);
    RUN(test_identifier_of_max_length);
    RUN(test_identifier_too_long);
    RUN(test_identifier_with_underscore_and_digit);
    RUN(test_identifier_starting_with_digit);
    RUN(test_lone_bang);
    RUN(test_angle_not_equal_is_unsupported);
    RUN(test_double_quote);
    RUN(test_unknown_characters_report_line);
    RUN(test_non_ascii_identifier);
    RUN(test_nul_byte_in_input);
    RUN(test_tokenizer_advances_after_error);
    TEST_DONE();
    return 0;
}
