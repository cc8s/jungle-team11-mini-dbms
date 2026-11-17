#include "tokenizer.h"

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define MAX_IDENT_LEN (MAX_NAME - 1)
#define NON_ASCII_MIN 0x80
#define DECIMAL_BASE 10

typedef struct {
    const char *name; /* 소문자. 식별자를 소문자로 정규화한 뒤 비교하므로 대소문자를 무시하게 된다 */
    Keyword keyword;
} KeywordEntry;

static const KeywordEntry KEYWORDS[] = {
    { "select", KW_SELECT }, { "from", KW_FROM },     { "where", KW_WHERE },
    { "limit", KW_LIMIT },   { "insert", KW_INSERT }, { "into", KW_INTO },
    { "values", KW_VALUES }, { "create", KW_CREATE }, { "table", KW_TABLE },
    { "int", KW_INT },       { "varchar", KW_VARCHAR }, { "and", KW_AND },
    { "or", KW_OR },
};

typedef struct {
    char symbol;
    TokenType type;
} SingleSymbol;

static const SingleSymbol SINGLE_SYMBOLS[] = {
    { '(', TOK_LPAREN }, { ')', TOK_RPAREN }, { ',', TOK_COMMA },
    { ';', TOK_SEMI },   { '*', TOK_STAR },   { '=', TOK_EQ },
};

/* ctype.h 는 로캘과 음수 char 에 영향을 받으므로 ASCII 판단을 직접 한다 */
static bool is_letter(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

static bool is_non_ascii(unsigned char c)
{
    return c >= NON_ASCII_MIN;
}

/* 단어(식별자·숫자)를 이루는 바이트. 비 ASCII 도 넣어 "name이름" 같은 덩어리를 한 번에 읽고 한 번만 거절한다 */
static bool is_word_byte(unsigned char c)
{
    return is_letter(c) || is_digit(c) || c == '_' || is_non_ascii(c);
}

static bool is_at_end(const Tokenizer *tokenizer)
{
    return tokenizer->pos >= tokenizer->len;
}

/* 현재 위치에서 offset 만큼 뒤의 바이트. 입력 끝을 넘으면 0 */
static unsigned char peek(const Tokenizer *tokenizer, size_t offset)
{
    size_t index = tokenizer->pos + offset;
    return index < tokenizer->len ? (unsigned char)tokenizer->sql[index] : 0;
}

static ErrorCode fail(Tokenizer *tokenizer, ErrorCode code, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static ErrorCode fail(Tokenizer *tokenizer, ErrorCode code, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(tokenizer->message, sizeof(tokenizer->message), format, args);
    va_end(args);
    return code;
}

/* -- 부터 줄 끝까지. \n 은 남겨 두어 줄 번호를 skip_whitespace_and_comments 가 세게 한다 */
static void skip_line_comment(Tokenizer *tokenizer)
{
    while (!is_at_end(tokenizer) && peek(tokenizer, 0) != '\n')
        tokenizer->pos++;
}

/* 줄 번호는 \n 만 센다. \r 은 공백으로 넘기므로 \r\n 도 한 줄이 된다.
 * 주석은 문자열 밖에서만 여기로 오므로 '...--...' 안의 -- 는 주석이 아니다 */
static void skip_whitespace_and_comments(Tokenizer *tokenizer)
{
    while (!is_at_end(tokenizer)) {
        unsigned char c = peek(tokenizer, 0);
        if (c == '-' && peek(tokenizer, 1) == '-') {
            skip_line_comment(tokenizer);
            continue;
        }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            return;
        if (c == '\n')
            tokenizer->line++;
        tokenizer->pos++;
    }
}

static ErrorCode emit(Tokenizer *tokenizer, Token *token, TokenType type, size_t width)
{
    token->type = type;
    token->len = width;
    tokenizer->pos += width;
    return ERR_NONE;
}

/* 단어 바이트를 끝까지 읽고 길이를 돌려준다. 에러가 나도 단어 전체를 건너뛰게 된다 */
static size_t consume_word(Tokenizer *tokenizer, Token *token)
{
    while (!is_at_end(tokenizer) && is_word_byte(peek(tokenizer, 0)))
        tokenizer->pos++;
    token->len = (size_t)(tokenizer->sql + tokenizer->pos - token->start);
    return token->len;
}

static bool find_keyword(const char *name, Keyword *out_keyword)
{
    for (size_t i = 0; i < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); i++) {
        if (strcmp(KEYWORDS[i].name, name) == 0) {
            *out_keyword = KEYWORDS[i].keyword;
            return true;
        }
    }
    return false;
}

static bool has_non_ascii(const char *text, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (is_non_ascii((unsigned char)text[i]))
            return true;
    }
    return false;
}

static ErrorCode scan_word(Tokenizer *tokenizer, Token *token)
{
    size_t len = consume_word(tokenizer, token);
    if (has_non_ascii(token->start, len))
        return fail(tokenizer, ERR_SYNTAX, "identifier must use only ASCII letters, digits, '_'");
    if (len > MAX_IDENT_LEN)
        return fail(tokenizer, ERR_SYNTAX, "identifier longer than %d characters", MAX_IDENT_LEN);

    for (size_t i = 0; i < len; i++) {
        char c = token->start[i];
        token->text[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    token->text[len] = '\0';
    token->type = find_keyword(token->text, &token->keyword) ? TOK_KEYWORD : TOK_IDENT;
    return ERR_NONE;
}

/* 선택적 - 와 숫자. - 바로 뒤에 숫자가 있을 때만 여기로 온다 ("- 5" 는 reject_character) */
static ErrorCode scan_number(Tokenizer *tokenizer, Token *token)
{
    bool is_negative = peek(tokenizer, 0) == '-';
    if (is_negative)
        tokenizer->pos++;
    size_t len = consume_word(tokenizer, token);
    size_t sign_len = is_negative ? 1 : 0;

    /* 절댓값으로 세고 부호는 마지막에 붙인다. 음수 쪽 한계가 1 크다 (-2147483648 은 되고 2147483648 은 안 됨).
     * 한계를 넘으면 누적을 멈추므로 아주 긴 숫자에서도 int64 가 넘치지 않는다 */
    int64_t limit = is_negative ? -(int64_t)INT32_MIN : INT32_MAX;
    int64_t magnitude = 0;
    for (size_t i = sign_len; i < len; i++) {
        unsigned char c = (unsigned char)token->start[i];
        if (!is_digit(c))
            return fail(tokenizer, ERR_SYNTAX, "invalid number '%.*s'", (int)len, token->start);
        if (magnitude <= limit)
            magnitude = magnitude * DECIMAL_BASE + (c - '0');
    }
    if (magnitude > limit)
        return fail(tokenizer, ERR_OUT_OF_RANGE, "integer '%.*s' out of INT range", (int)len,
                    token->start);
    token->type = TOK_INT;
    token->int_value = (int32_t)(is_negative ? -magnitude : magnitude);
    return ERR_NONE;
}

/* 작은따옴표 문자열. 값을 복사하지 않고 길이만 세 두고, 값은 tokenizer_copy_string 이 원문에서 푼다.
 * 그래서 Token 이 힙을 갖지 않아 토큰마다 해제할 필요가 없다 */
static ErrorCode scan_string(Tokenizer *tokenizer, Token *token)
{
    bool has_nul = false;
    tokenizer->pos++; /* 여는 ' */
    for (;;) {
        if (is_at_end(tokenizer)) {
            token->len = tokenizer->len - (size_t)(token->start - tokenizer->sql);
            return fail(tokenizer, ERR_SYNTAX, "unterminated string starting at line %d",
                        token->line);
        }
        unsigned char c = peek(tokenizer, 0);
        if (c == '\'' && peek(tokenizer, 1) != '\'')
            break;
        if (c == '\'')
            tokenizer->pos++; /* '' 의 첫 번째 ' 를 건너뛰고 두 번째를 값으로 센다 */
        else if (c == '\n')
            tokenizer->line++;
        else if (c == '\0')
            has_nul = true;
        tokenizer->pos++;
        token->string_len++;
    }
    tokenizer->pos++; /* 닫는 ' */
    token->len = (size_t)(tokenizer->sql + tokenizer->pos - token->start);
    if (has_nul)
        return fail(tokenizer, ERR_SYNTAX, "NUL byte in string");
    token->type = TOK_STRING;
    return ERR_NONE;
}

/* < > ! 로 시작하는 비교 연산자. 두 글자 연산자를 먼저 본다 */
static ErrorCode scan_comparison(Tokenizer *tokenizer, Token *token, unsigned char c)
{
    bool is_followed_by_eq = peek(tokenizer, 1) == '=';
    size_t width = is_followed_by_eq ? 2 : 1;
    if (c == '<' && peek(tokenizer, 1) == '>') {
        tokenizer->pos += 2;
        return fail(tokenizer, ERR_UNSUPPORTED, "'<>' is not supported, use '!='");
    }
    if (c == '<')
        return emit(tokenizer, token, is_followed_by_eq ? TOK_LE : TOK_LT, width);
    if (c == '>')
        return emit(tokenizer, token, is_followed_by_eq ? TOK_GE : TOK_GT, width);
    if (is_followed_by_eq)
        return emit(tokenizer, token, TOK_NE, 2);
    tokenizer->pos++;
    return fail(tokenizer, ERR_SYNTAX, "unexpected '!', did you mean '!='?");
}

static ErrorCode reject_character(Tokenizer *tokenizer, unsigned char c)
{
    tokenizer->pos++;
    if (c == '"')
        return fail(tokenizer, ERR_SYNTAX, "use single quotes for strings");
    if (c == '\0')
        return fail(tokenizer, ERR_SYNTAX, "NUL byte in input");
    if (c == '-')
        return fail(tokenizer, ERR_SYNTAX, "expected digits right after '-'");
    if (c < ' ' || c > '~')
        return fail(tokenizer, ERR_SYNTAX, "unexpected byte 0x%02x", c);
    return fail(tokenizer, ERR_SYNTAX, "unexpected character '%c'", c);
}

static ErrorCode scan_symbol(Tokenizer *tokenizer, Token *token)
{
    unsigned char c = peek(tokenizer, 0);
    if (c == '<' || c == '>' || c == '!')
        return scan_comparison(tokenizer, token, c);
    if (c == '?') {
        token->param_index = tokenizer->param_count++;
        return emit(tokenizer, token, TOK_PARAM, 1);
    }

    for (size_t i = 0; i < sizeof(SINGLE_SYMBOLS) / sizeof(SINGLE_SYMBOLS[0]); i++) {
        if (SINGLE_SYMBOLS[i].symbol == (char)c)
            return emit(tokenizer, token, SINGLE_SYMBOLS[i].type, 1);
    }
    return reject_character(tokenizer, c);
}

void tokenizer_init(Tokenizer *tokenizer, const char *sql, size_t len)
{
    memset(tokenizer, 0, sizeof(*tokenizer));
    tokenizer->sql = sql;
    tokenizer->len = len;
    tokenizer->line = 1;
}

static ErrorCode scan_token(Tokenizer *tokenizer, Token *token)
{
    if (is_at_end(tokenizer)) {
        token->type = TOK_EOF;
        return ERR_NONE;
    }

    unsigned char c = peek(tokenizer, 0);
    if (is_letter(c) || c == '_' || is_non_ascii(c))
        return scan_word(tokenizer, token);
    if (is_digit(c) || (c == '-' && is_digit(peek(tokenizer, 1))))
        return scan_number(tokenizer, token);
    if (c == '\'')
        return scan_string(tokenizer, token);
    return scan_symbol(tokenizer, token);
}

/* 문장 크기는 ; 로 끊어 센다. 넘친 문장에서 한 번만 알리고, 나머지 토큰은 그대로 돌려주어
 * 호출한 쪽이 평소처럼 ; 까지 건너뛰고 다음 문장으로 넘어갈 수 있게 한다 */
static ErrorCode check_statement_size(Tokenizer *tokenizer, const Token *token)
{
    if (token->type == TOK_SEMI || token->type == TOK_EOF) {
        tokenizer->is_in_statement = false;
        return ERR_NONE;
    }
    if (!tokenizer->is_in_statement) {
        tokenizer->is_in_statement = true;
        tokenizer->statement_start = (size_t)(token->start - tokenizer->sql);
        tokenizer->is_too_large_reported = false;
    }
    size_t statement_len = tokenizer->pos - tokenizer->statement_start;
    if (statement_len <= MAX_SQL_LEN || tokenizer->is_too_large_reported)
        return ERR_NONE;
    tokenizer->is_too_large_reported = true;
    return fail(tokenizer, ERR_TOO_LARGE, "statement longer than %d bytes", MAX_SQL_LEN);
}

ErrorCode tokenizer_next(Tokenizer *tokenizer, Token *out_token)
{
    skip_whitespace_and_comments(tokenizer);
    memset(out_token, 0, sizeof(*out_token));
    out_token->line = tokenizer->line;
    out_token->start = tokenizer->sql + tokenizer->pos;

    ErrorCode error = scan_token(tokenizer, out_token);
    if (error != ERR_NONE)
        return error;
    return check_statement_size(tokenizer, out_token);
}

void tokenizer_copy_string(const Token *token, char *out_text)
{
    assert(token->type == TOK_STRING);
    /* 원문은 '...' 이고 안의 '' 는 반드시 짝으로 나온다 (scan_string 이 확인함) */
    const char *body = token->start + 1;
    const char *body_end = token->start + token->len - 1;
    size_t written = 0;
    for (const char *p = body; p < body_end; p++) {
        out_text[written++] = *p;
        if (*p == '\'')
            p++;
    }
    out_text[written] = '\0';
}
