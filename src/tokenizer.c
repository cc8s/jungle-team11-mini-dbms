#include "tokenizer.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void tokenizer_init(struct tokenizer *tz, const char *src)
{
    tz->src = src;
    tz->pos = 0;
}

static void set_error(struct token *out, const char *msg)
{
    out->type = TOKEN_ERROR;
    snprintf(out->text, sizeof(out->text), "%s", msg);
}

/* 공백과 -- 줄 주석을 건너뛴다. 주석은 줄 끝(또는 입력 끝)까지 */
static void skip_whitespace(struct tokenizer *tz)
{
    for (;;) {
        while (isspace((unsigned char)tz->src[tz->pos])) {
            tz->pos++;
        }
        if (tz->src[tz->pos] != '-' || tz->src[tz->pos + 1] != '-') {
            return;
        }
        while (tz->src[tz->pos] != '\0' && tz->src[tz->pos] != '\n') {
            tz->pos++;
        }
    }
}

static bool is_ident_start(char c)
{
    return isalpha((unsigned char)c) || c == '_';
}

static bool is_ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static void read_ident(struct tokenizer *tz, struct token *out)
{
    size_t start = tz->pos;

    while (is_ident_char(tz->src[tz->pos])) {
        tz->pos++;
    }
    size_t len = tz->pos - start;
    if (len >= TOKEN_TEXT_MAX) {
        set_error(out, "identifier too long");
        return;
    }
    out->type = TOKEN_IDENT;
    memcpy(out->text, tz->src + start, len);
    out->text[len] = '\0';
}

static void read_int(struct tokenizer *tz, struct token *out)
{
    size_t start = tz->pos;

    if (tz->src[tz->pos] == '-') {
        tz->pos++;
    }
    while (isdigit((unsigned char)tz->src[tz->pos])) {
        tz->pos++;
    }
    if (is_ident_char(tz->src[tz->pos])) {
        set_error(out, "invalid number");
        return;
    }
    size_t len = tz->pos - start;
    if (len >= TOKEN_TEXT_MAX) {
        set_error(out, "number out of range");
        return;
    }
    memcpy(out->text, tz->src + start, len);
    out->text[len] = '\0';

    errno = 0;
    out->int_value = strtoll(out->text, NULL, 10);
    if (errno == ERANGE) {
        set_error(out, "number out of range");
        return;
    }
    out->type = TOKEN_INT;
}

/* 'it''s' → it's. 닫는 따옴표가 없으면 오류. */
static void read_string(struct tokenizer *tz, struct token *out)
{
    size_t len = 0;

    tz->pos++; /* 여는 ' */
    for (;;) {
        char c = tz->src[tz->pos];
        if (c == '\0') {
            set_error(out, "unterminated string");
            return;
        }
        if (c == '\'') {
            if (tz->src[tz->pos + 1] != '\'') {
                tz->pos++; /* 닫는 ' */
                break;
            }
            tz->pos++; /* '' 의 첫 ' 를 건너뛰고 둘째 ' 를 글자로 담는다 */
        }
        if (len + 1 >= TOKEN_TEXT_MAX) {
            set_error(out, "string too long");
            return;
        }
        out->text[len++] = c;
        tz->pos++;
    }
    out->text[len] = '\0';
    out->type = TOKEN_STRING;
}

static bool read_symbol(struct tokenizer *tz, struct token *out)
{
    static const struct {
        char ch;
        enum token_type type;
    } symbols[] = {
        {'*', TOKEN_STAR},   {',', TOKEN_COMMA}, {'(', TOKEN_LPAREN},
        {')', TOKEN_RPAREN}, {'=', TOKEN_EQUAL}, {';', TOKEN_SEMICOLON},
    };
    char c = tz->src[tz->pos];

    for (size_t i = 0; i < sizeof(symbols) / sizeof(symbols[0]); i++) {
        if (symbols[i].ch == c) {
            out->type = symbols[i].type;
            out->text[0] = c;
            out->text[1] = '\0';
            tz->pos++;
            return true;
        }
    }
    return false;
}

void tokenizer_next(struct tokenizer *tz, struct token *out)
{
    skip_whitespace(tz);
    out->pos = tz->pos;
    out->text[0] = '\0';
    out->int_value = 0;

    char c = tz->src[tz->pos];
    if (c == '\0') {
        out->type = TOKEN_EOF;
        return;
    }
    if (is_ident_start(c)) {
        read_ident(tz, out);
        return;
    }
    if (isdigit((unsigned char)c) || (c == '-' && isdigit((unsigned char)tz->src[tz->pos + 1]))) {
        read_int(tz, out);
        return;
    }
    if (c == '\'') {
        read_string(tz, out);
        return;
    }
    if (read_symbol(tz, out)) {
        return;
    }
    set_error(out, "unexpected character");
    tz->pos++;
}

bool token_is_keyword(const struct token *tok, const char *kw)
{
    return tok->type == TOKEN_IDENT && strcasecmp(tok->text, kw) == 0;
}
