#ifndef PARSER_H
#define PARSER_H

/* 토큰 → Statement (재귀 하강). 문법만 본다: 테이블·컬럼이 실제로 있는지는 validator 가 검사한다.
 * 상태는 Parser 핸들 안에만 있어 스레드마다 따로 만들어 쓰면 된다. */

#include <stddef.h>

#include "error.h"
#include "statement.h"

typedef struct Parser Parser;

/* sql 의 len 바이트를 읽는 파서를 만든다. sql 은 파서를 다 쓸 때까지 살아 있어야 한다. 메모리 부족이면 NULL. */
Parser *parser_create(const char *sql, size_t len);
void parser_destroy(Parser *parser);

/* 다음 문장 하나(; 또는 입력 끝까지)를 out_stmt 에 파싱한다.
 * 실패하면 에러 코드를 돌려주고 parser_error_line / parser_error_message 로 위치와 설명을 알려준다.
 * 실패해도 그 문장의 ; 까지는 읽어 두므로 다시 부르면 다음 문장부터 읽는다.
 * 성공이든 실패든 out_stmt 에 statement_free 를 불러도 안전하다 (실패면 해제할 것이 없다). */
ErrorCode parser_parse_statement(Parser *parser, Statement *out_stmt);

/* 마지막 에러가 난 줄 (1 부터) */
int parser_error_line(const Parser *parser);
/* 마지막 에러 설명. 문제 토큰을 원문 그대로 담는다 (예: "near 'FORM': expected FROM") */
const char *parser_error_message(const Parser *parser);

#endif
