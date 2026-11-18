#ifndef MINI_DBMS_TOKENIZER_H
#define MINI_DBMS_TOKENIZER_H

/* SQL 문자열을 앞에서부터 토큰 하나씩 잘라 낸다.
 * 키워드는 따로 구분하지 않고 TOKEN_IDENT 로 낸다. 키워드인지는 파서가 자리를 보고 판단한다. */

#include <stdbool.h>
#include <stddef.h>

#define TOKEN_TEXT_MAX 256 /* 식별자와 문자열 리터럴의 최대 바이트 수 (NUL 제외 255) */

enum token_type {
    TOKEN_EOF,
    TOKEN_IDENT,     /* users, name, SELECT ... */
    TOKEN_INT,       /* 42, -7 */
    TOKEN_STRING,    /* 'kim' (따옴표 제외, '' 는 ' 하나로) */
    TOKEN_STAR,      /* * */
    TOKEN_COMMA,     /* , */
    TOKEN_LPAREN,    /* ( */
    TOKEN_RPAREN,    /* ) */
    TOKEN_EQUAL,     /* = */
    TOKEN_SEMICOLON, /* ; */
    TOKEN_ERROR      /* text 에 오류 설명 */
};

struct token {
    enum token_type type;
    char text[TOKEN_TEXT_MAX];
    long long int_value; /* TOKEN_INT 일 때 */
    size_t pos;          /* 원본 문자열에서 토큰이 시작한 위치 */
};

struct tokenizer {
    const char *src;
    size_t pos;
};

void tokenizer_init(struct tokenizer *tz, const char *src);

/* 다음 토큰을 out 에 채운다. 끝이면 TOKEN_EOF, 잘못된 입력이면 TOKEN_ERROR. */
void tokenizer_next(struct tokenizer *tz, struct token *out);

/* 대소문자 무시 비교. 토큰이 키워드 kw 인지 확인할 때 쓴다. */
bool token_is_keyword(const struct token *tok, const char *kw);

#endif
