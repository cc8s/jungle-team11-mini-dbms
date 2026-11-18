#include "test.h"
#include "tokenizer.h"

/* 입력을 끝까지 토큰으로 나눠 types 에 담고 개수를 돌려준다. EOF 와 ERROR 도 담는다. */
static int tokenize_all(const char *src, struct token *toks, int max)
{
    struct tokenizer tz;
    int n = 0;

    tokenizer_init(&tz, src);
    while (n < max) {
        tokenizer_next(&tz, &toks[n]);
        enum token_type t = toks[n++].type;
        if (t == TOKEN_EOF || t == TOKEN_ERROR) {
            break;
        }
    }
    return n;
}

/* TC-TOK-001 EP: 문장 하나의 토큰 종류와 순서 */
static void test_select_statement(void)
{
    struct token t[16];
    int n = tokenize_all("SELECT * FROM users WHERE id = 3;", t, 16);

    CHECK(n == 10); /* 토큰 9개 + EOF */
    CHECK(t[0].type == TOKEN_IDENT && token_is_keyword(&t[0], "select"));
    CHECK(t[1].type == TOKEN_STAR);
    CHECK(t[2].type == TOKEN_IDENT && token_is_keyword(&t[2], "FROM"));
    CHECK_STR_EQ(t[3].text, "users");
    CHECK(t[5].type == TOKEN_IDENT && strcmp(t[5].text, "id") == 0);
    CHECK(t[6].type == TOKEN_EQUAL);
    CHECK(t[7].type == TOKEN_INT && t[7].int_value == 3);
    CHECK(t[8].type == TOKEN_SEMICOLON);
}

/* TC-TOK-002 EP: 괄호, 쉼표, 문자열, 음수 */
static void test_insert_symbols(void)
{
    struct token t[16];
    int n = tokenize_all("VALUES ('kim', -7)", t, 16);

    CHECK(n == 7);
    CHECK(t[1].type == TOKEN_LPAREN);
    CHECK(t[2].type == TOKEN_STRING);
    CHECK_STR_EQ(t[2].text, "kim");
    CHECK(t[3].type == TOKEN_COMMA);
    CHECK(t[4].type == TOKEN_INT && t[4].int_value == -7);
    CHECK(t[5].type == TOKEN_RPAREN);
    CHECK(t[6].type == TOKEN_EOF);
}

/* TC-TOK-003 EG: 문자열 안의 '' 는 ' 하나, 공백과 ; 는 글자로 남는다 */
static void test_string_escape(void)
{
    struct token t[4];

    tokenize_all("'it''s; ok'", t, 4);
    CHECK(t[0].type == TOKEN_STRING);
    CHECK_STR_EQ(t[0].text, "it's; ok");
    CHECK(t[1].type == TOKEN_EOF);

    tokenize_all("''", t, 4);
    CHECK(t[0].type == TOKEN_STRING);
    CHECK_STR_EQ(t[0].text, "");
}

/* TC-TOK-004 EG: 한글(UTF-8 여러 바이트)은 문자열 안에서 그대로 */
static void test_utf8_string(void)
{
    struct token t[2];

    tokenize_all("'김정글'", t, 2);
    CHECK(t[0].type == TOKEN_STRING);
    CHECK_STR_EQ(t[0].text, "김정글");
}

/* TC-TOK-005 EG: 닫히지 않은 문자열, 알 수 없는 문자, 숫자 뒤 글자 */
static void test_errors(void)
{
    struct token t[4];

    tokenize_all("'abc", t, 4);
    CHECK(t[0].type == TOKEN_ERROR);
    CHECK_STR_EQ(t[0].text, "unterminated string");

    int n = tokenize_all("id # 3", t, 4);
    CHECK(n == 2 && t[1].type == TOKEN_ERROR && t[1].pos == 3);

    tokenize_all("12abc", t, 4);
    CHECK(t[0].type == TOKEN_ERROR);
}

/* TC-TOK-006 BVA: long long 경계 */
static void test_int_range(void)
{
    struct token t[2];

    tokenize_all("9223372036854775807", t, 2);
    CHECK(t[0].type == TOKEN_INT && t[0].int_value == 9223372036854775807LL);

    tokenize_all("-9223372036854775808", t, 2);
    CHECK(t[0].type == TOKEN_INT && t[0].int_value == (-9223372036854775807LL - 1));

    tokenize_all("9223372036854775808", t, 2);
    CHECK(t[0].type == TOKEN_ERROR);
}

/* TC-TOK-007 BVA: 문자열 최대 길이 255 바이트는 통과, 256 은 오류 */
static void test_string_length_limit(void)
{
    char src[TOKEN_TEXT_MAX + 8];
    struct token t[2];

    src[0] = '\'';
    memset(src + 1, 'a', TOKEN_TEXT_MAX - 1);
    src[TOKEN_TEXT_MAX] = '\'';
    src[TOKEN_TEXT_MAX + 1] = '\0';
    tokenize_all(src, t, 2);
    CHECK(t[0].type == TOKEN_STRING && strlen(t[0].text) == TOKEN_TEXT_MAX - 1);

    memset(src + 1, 'a', TOKEN_TEXT_MAX);
    src[TOKEN_TEXT_MAX + 1] = '\'';
    src[TOKEN_TEXT_MAX + 2] = '\0';
    tokenize_all(src, t, 2);
    CHECK(t[0].type == TOKEN_ERROR);
}

/* TC-TOK-008 EP: 빈 입력과 공백·줄바꿈만 */
static void test_empty(void)
{
    struct token t[2];

    CHECK(tokenize_all("", t, 2) == 1 && t[0].type == TOKEN_EOF);
    CHECK(tokenize_all(" \n\t ", t, 2) == 1 && t[0].type == TOKEN_EOF);
}

/* TC-TOK-009 EP: -- 줄 주석은 건너뛴다. 문자열 안의 -- 와 음수 -1 은 그대로 */
static void test_line_comment(void)
{
    struct token t[8];

    int n = tokenize_all("-- 처음\nSELECT -- 중간\n'a--b' -1 -- 끝", t, 8);
    CHECK(n == 4);
    CHECK(token_is_keyword(&t[0], "SELECT"));
    CHECK(t[1].type == TOKEN_STRING);
    CHECK_STR_EQ(t[1].text, "a--b");
    CHECK(t[2].type == TOKEN_INT && t[2].int_value == -1);
    CHECK(t[3].type == TOKEN_EOF);

    CHECK(tokenize_all("-- 주석만", t, 8) == 1 && t[0].type == TOKEN_EOF);
}

int main(void)
{
    RUN(test_line_comment);
    RUN(test_select_statement);
    RUN(test_insert_symbols);
    RUN(test_string_escape);
    RUN(test_utf8_string);
    RUN(test_errors);
    RUN(test_int_range);
    RUN(test_string_length_limit);
    RUN(test_empty);
    TEST_DONE();
    return 0;
}
