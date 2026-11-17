#ifndef TOKENIZER_H
#define TOKENIZER_H

/* SQL 텍스트 → 토큰. 호출할 때마다 토큰 하나를 읽는 스트리밍 방식이다.
 * 상태는 모두 호출한 쪽이 가진 Tokenizer 안에 있어 전역 없이 여러 스레드가 각자 쓸 수 있다. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"
#include "error.h"

typedef enum {
    TOK_KEYWORD,
    TOK_IDENT,
    TOK_INT,
    TOK_STRING,
    TOK_PARAM,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_COMMA,
    TOK_SEMI,
    TOK_STAR,
    TOK_EQ,
    TOK_NE,
    TOK_LT,
    TOK_GT,
    TOK_LE,
    TOK_GE,
    TOK_EOF,
} TokenType;

typedef enum {
    KW_SELECT,
    KW_FROM,
    KW_WHERE,
    KW_LIMIT,
    KW_INSERT,
    KW_INTO,
    KW_VALUES,
    KW_CREATE,
    KW_TABLE,
    KW_INT,
    KW_VARCHAR,
    KW_AND,
    KW_OR,
} Keyword;

typedef struct {
    TokenType type;
    int line;          /* 토큰이 시작한 줄, 1 부터 */
    const char *start; /* 원문 위치 (에러 메시지 "near 'FORM'" 용, 대소문자 그대로) */
    size_t len;
    Keyword keyword;      /* TOK_KEYWORD */
    char text[MAX_NAME];  /* TOK_IDENT: 소문자로 정규화한 이름 */
    int32_t int_value;    /* TOK_INT */
    size_t string_len;    /* TOK_STRING: '' 를 ' 로 푼 뒤의 바이트 수 (값은 tokenizer_copy_string) */
    int param_index;      /* TOK_PARAM: 입력 전체에서 ? 가 나온 순서, 0 부터 */
} Token;

/* 호출한 쪽이 스택 등에 두고 쓰는 커서. 필드는 tokenizer 모듈만 바꾼다. */
typedef struct {
    const char *sql;
    size_t len;
    size_t pos;
    int line;
    int param_count;
    bool is_in_statement;        /* ; 뒤 첫 토큰을 읽었는가 */
    size_t statement_start;      /* 현재 문장 첫 토큰의 위치 */
    bool is_too_large_reported;  /* 현재 문장의 ERR_TOO_LARGE 를 이미 알렸는가 */
    char message[MAX_MESSAGE];   /* 마지막 에러의 설명 */
} Tokenizer;

/* sql 의 len 바이트를 읽을 준비를 한다. sql 은 \0 으로 끝나지 않아도 되고, 토큰을 다 쓸 때까지 살아 있어야 한다. */
void tokenizer_init(Tokenizer *tokenizer, const char *sql, size_t len);

/* 다음 토큰을 out_token 에 채운다. 끝이면 TOK_EOF 를 계속 돌려준다.
 * 실패하면 에러 코드를 돌려주고 out_token->line 과 tokenizer->message 를 채운다.
 * 실패해도 문제 부분을 건너뛰어 두므로 다시 부르면 그 뒤부터 읽는다 (문장 단위 에러 복구용).
 * 문장 하나(첫 토큰 시작 ~ 마지막 토큰 끝, ; 제외)가 MAX_SQL_LEN 을 넘으면 그 문장에서 한 번 ERR_TOO_LARGE. */
ErrorCode tokenizer_next(Tokenizer *tokenizer, Token *out_token);

/* TOK_STRING 토큰의 값을 '' 이스케이프를 풀어 out_text 에 쓰고 \0 을 붙인다.
 * out_text 는 token->string_len + 1 바이트 이상이어야 한다. 원문(sql)이 살아 있어야 한다. */
void tokenizer_copy_string(const Token *token, char *out_text);

#endif
