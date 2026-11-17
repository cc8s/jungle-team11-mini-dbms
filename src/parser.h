#ifndef PARSER_H
#define PARSER_H

/* 토큰 → Statement (재귀 하강). 문법만 본다: 테이블·컬럼이 실제로 있는지는 validator 가 검사한다.
 * 상태는 Parser 핸들 안에만 있어 스레드마다 따로 만들어 쓰면 된다. */

#include <stdbool.h>
#include <stddef.h>

#include "common.h"
#include "error.h"
#include "statement.h"

typedef struct Parser Parser;

/* sql 의 len 바이트를 읽는 파서를 만든다. sql 은 파서를 다 쓸 때까지 살아 있어야 한다. 메모리 부족이면 NULL. */
Parser *parser_create(const char *sql, size_t len);
void parser_destroy(Parser *parser);

/* 스크립트 모드: 다음 문장 하나를 out_stmt 에 파싱한다. 빈 문장(; 만, 주석만)은 건너뛴다.
 * 남은 문장이 없으면 false (이후에도 계속 false). 있으면 true 이고 결과는 *out_error 에 담는다.
 * 실패한 문장도 번호를 받고, 그 문장의 ; 까지 건너뛰므로 다시 부르면 다음 문장부터 읽는다.
 * 실패면 parser_error_line / parser_error_message 로 위치와 설명을 알 수 있다.
 * 성공이든 실패든 out_stmt 에 statement_free 를 불러도 안전하다 (실패면 해제할 것이 없다). */
bool parser_next_statement(Parser *parser, Statement *out_stmt, ErrorCode *out_error);

/* 단일 문장 모드에서 에러 위치와 설명을 돌려받는 곳. 성공이면 line = 0, message = "" */
typedef struct {
    int line;                  /* 에러가 난 줄 (1 부터) */
    char message[MAX_MESSAGE]; /* 예: "near 'FORM': expected FROM" */
} ParseError;

/* 단일 문장 모드 (API 용): sql 의 len 바이트 전체가 문장 하나여야 한다. 끝의 ; 와 공백·주석은 허용한다.
 * 문장이 없으면 ERR_SYNTAX, 두 번째 문장이 있으면 ERR_UNSUPPORTED, 메모리 부족이면 ERR_INTERNAL.
 * 실패하면 out_stmt 는 비어 있고 (statement_free 해도 안전) out_error 에 위치와 설명을 채운다.
 * 파서를 안에서 만들고 해제하므로 호출한 쪽은 Parser 를 다루지 않는다. */
ErrorCode parser_parse_single(const char *sql, size_t len, Statement *out_stmt,
                              ParseError *out_error);

/* 마지막으로 꺼낸 문장의 번호 (1 부터, 에러 문장 포함, 빈 문장 제외). 아직 없으면 0 */
int parser_statement_number(const Parser *parser);

/* 마지막 에러가 난 줄 (1 부터) */
int parser_error_line(const Parser *parser);
/* 마지막 에러 설명. 문제 토큰을 원문 그대로 담는다 (예: "near 'FORM': expected FROM") */
const char *parser_error_message(const Parser *parser);

#endif
