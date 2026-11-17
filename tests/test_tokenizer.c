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

/* ---- S04: 리터럴, 주석, 제한 ---- */

static Token single_token(const char *sql, size_t len)
{
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, sql, len);
    Token token = next_ok(&tokenizer);
    CHECK(next_ok(&tokenizer).type == TOK_EOF);
    return token;
}

static int32_t single_int(const char *sql)
{
    Token token = single_token(sql, strlen(sql));
    CHECK(token.type == TOK_INT);
    return token.int_value;
}

/* 문자열 토큰 하나를 읽어 이스케이프를 푼 값이 expected 와 같은지 본다 */
static void check_single_string(const char *sql, const char *expected, size_t expected_len)
{
    Token token = single_token(sql, strlen(sql));
    CHECK(token.type == TOK_STRING);
    CHECK(token.string_len == expected_len);

    char *text = malloc(token.string_len + 1);
    CHECK(text != NULL);
    tokenizer_copy_string(&token, text);
    CHECK(memcmp(text, expected, expected_len) == 0);
    CHECK(text[expected_len] == '\0');
    free(text);
}

/* T04-01 */ static void test_integer(void)
{
    CHECK(single_int("42") == 42);
}

/* T04-02 */ static void test_negative_integer(void)
{
    CHECK(single_int("-7") == -7);
}

/* T04-03 */ static void test_string(void)
{
    check_single_string("'kim'", "kim", 3);
}

/* T04-04 */ static void test_string_quote_escape(void)
{
    check_single_string("'it''s'", "it's", 4);
}

/* T04-05 */ static void test_string_with_spaces(void)
{
    check_single_string("'kim lee, park'", "kim lee, park", 13);
}

/* T04-06 */ static void test_korean_string(void)
{
    check_single_string("'김철수'", "김철수", 9);
}

/* T04-07 */ static void test_line_comment(void)
{
    const char *sql = "SELECT -- 주석\n*";
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, sql, strlen(sql));

    CHECK(next_ok(&tokenizer).keyword == KW_SELECT);
    Token star = next_ok(&tokenizer);
    CHECK(star.type == TOK_STAR);
    CHECK(star.line == 2);
    CHECK(next_ok(&tokenizer).type == TOK_EOF);
}

/* T04-08 */ static void test_placeholders_are_numbered(void)
{
    const char *sql = "? ?";
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, sql, strlen(sql));

    for (int i = 0; i < 2; i++) {
        Token token = next_ok(&tokenizer);
        CHECK(token.type == TOK_PARAM);
        CHECK(token.param_index == i);
    }
}

/* E04-01 */ static void test_int_max(void)
{
    CHECK(single_int("2147483647") == INT32_MAX);
}

/* E04-02 */ static void test_int_max_plus_one(void)
{
    CHECK(first_error("2147483648") == ERR_OUT_OF_RANGE);
}

/* E04-03 */ static void test_int_min(void)
{
    CHECK(single_int("-2147483648") == INT32_MIN);
}

/* E04-04 */ static void test_int_min_minus_one(void)
{
    CHECK(first_error("-2147483649") == ERR_OUT_OF_RANGE);
}

/* E04-05 */ static void test_very_long_number(void)
{
    CHECK(first_error("999999999999999999999999999999") == ERR_OUT_OF_RANGE);
    CHECK(first_error("-999999999999999999999999999999") == ERR_OUT_OF_RANGE);
}

/* E04-06 */ static void test_leading_zeros(void)
{
    CHECK(single_int("007") == 7);
}

/* E04-07 */ static void test_negative_zero(void)
{
    CHECK(single_int("-0") == 0);
}

/* E04-08 */ static void test_minus_followed_by_space(void)
{
    CHECK(first_error("- 5") == ERR_SYNTAX);
}

/* E04-09 */ static void test_double_minus_starts_comment(void)
{
    const char *sql = "age = --5";
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, sql, strlen(sql));

    CHECK(next_ok(&tokenizer).type == TOK_IDENT);
    CHECK(next_ok(&tokenizer).type == TOK_EQ);
    CHECK(next_ok(&tokenizer).type == TOK_EOF);
}

/* E04-10 */ static void test_number_followed_by_letters(void)
{
    CHECK(first_error("12abc") == ERR_SYNTAX);
}

/* E04-11 */ static void test_empty_string(void)
{
    check_single_string("''", "", 0);
}

/* E04-12 */ static void test_string_of_single_quote(void)
{
    check_single_string("''''", "'", 1);
}

/* E04-13 */ static void test_unterminated_string_reports_start_line(void)
{
    const char *sql = "SELECT\n'kim\nlee";
    TokenizeFailure failure = first_failure(sql, strlen(sql));
    CHECK(failure.error == ERR_SYNTAX);
    CHECK(failure.line == 2);
}

/* E04-14 */ static void test_multiline_string(void)
{
    const char *sql = "'a\nb' x";
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, sql, strlen(sql));

    Token string = next_ok(&tokenizer);
    CHECK(string.type == TOK_STRING);
    CHECK(string.line == 1);
    char text[4];
    tokenizer_copy_string(&string, text);
    CHECK_STR_EQ(text, "a\nb");
    CHECK(next_ok(&tokenizer).line == 2);
}

/* E04-15 */ static void test_double_minus_inside_string(void)
{
    check_single_string("'a--b'", "a--b", 4);
}

/* E04-16 */ static void test_quote_inside_comment(void)
{
    const char *sql = "-- it's\nSELECT";
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, sql, strlen(sql));

    Token token = next_ok(&tokenizer);
    CHECK(token.keyword == KW_SELECT);
    CHECK(token.line == 2);
}

/* E04-17 */ static void test_comment_at_end_without_newline(void)
{
    const char *sql = "SELECT -- end";
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, sql, strlen(sql));

    CHECK(next_ok(&tokenizer).keyword == KW_SELECT);
    CHECK(next_ok(&tokenizer).type == TOK_EOF);
}

/* E04-18 */ static void test_nul_byte_inside_string(void)
{
    const char sql[] = { '\'', 'a', '\0', 'b', '\'' };
    CHECK(first_failure(sql, sizeof(sql)).error == ERR_SYNTAX);
}

/* E04-19 */ static void test_question_mark_inside_string(void)
{
    check_single_string("'what?'", "what?", 5);
}

/* E04-20 */ static void test_backslash_is_plain_character(void)
{
    const char *sql = "'a\\'b'";
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, sql, strlen(sql));

    Token string = next_ok(&tokenizer);
    CHECK(string.type == TOK_STRING);
    CHECK(string.string_len == 2); /* a\ */
    CHECK(next_ok(&tokenizer).type == TOK_IDENT);
    Token token;
    CHECK(tokenizer_next(&tokenizer, &token) == ERR_SYNTAX); /* 닫히지 않은 ' */
}

/* 문장 하나(첫 토큰 ~ 마지막 토큰)가 MAX_SQL_LEN 바이트를 넘으면 ERR_TOO_LARGE, ; 뒤 문장은 새로 센다 */
static char *make_string_statement(size_t statement_len)
{
    /* 'xxx...x'; SELECT  — 문자열 토큰이 statement_len 바이트 */
    const char *tail = "; SELECT";
    char *sql = malloc(statement_len + strlen(tail) + 1);
    CHECK(sql != NULL);
    memset(sql, 'x', statement_len);
    sql[0] = '\'';
    sql[statement_len - 1] = '\'';
    strcpy(sql + statement_len, tail);
    return sql;
}

static void test_statement_size_limit(void)
{
    char *exact = make_string_statement(MAX_SQL_LEN);
    CHECK(first_error(exact) == ERR_NONE);
    free(exact);

    char *too_large = make_string_statement(MAX_SQL_LEN + 1);
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, too_large, strlen(too_large));
    Token token;
    CHECK(tokenizer_next(&tokenizer, &token) == ERR_TOO_LARGE);
    CHECK(next_ok(&tokenizer).type == TOK_SEMI);
    CHECK(next_ok(&tokenizer).keyword == KW_SELECT); /* 다음 문장은 정상 */
    free(too_large);
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

    RUN(test_integer);
    RUN(test_negative_integer);
    RUN(test_string);
    RUN(test_string_quote_escape);
    RUN(test_string_with_spaces);
    RUN(test_korean_string);
    RUN(test_line_comment);
    RUN(test_placeholders_are_numbered);
    RUN(test_int_max);
    RUN(test_int_max_plus_one);
    RUN(test_int_min);
    RUN(test_int_min_minus_one);
    RUN(test_very_long_number);
    RUN(test_leading_zeros);
    RUN(test_negative_zero);
    RUN(test_minus_followed_by_space);
    RUN(test_double_minus_starts_comment);
    RUN(test_number_followed_by_letters);
    RUN(test_empty_string);
    RUN(test_string_of_single_quote);
    RUN(test_unterminated_string_reports_start_line);
    RUN(test_multiline_string);
    RUN(test_double_minus_inside_string);
    RUN(test_quote_inside_comment);
    RUN(test_comment_at_end_without_newline);
    RUN(test_nul_byte_inside_string);
    RUN(test_question_mark_inside_string);
    RUN(test_backslash_is_plain_character);
    RUN(test_statement_size_limit);
    TEST_DONE();
    return 0;
}
