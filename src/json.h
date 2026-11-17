#ifndef JSON_H
#define JSON_H

/* API 요청 본문 JSON 을 QueryRequest 로 바꾸는 최소 파서.
 * 범용 트리를 만들지 않고 sql 과 params 만 뽑아 Value 로 바로 만든다. 나머지 값은 문법만 검사하고 건너뛴다.
 * 상태는 모두 호출 스택과 인자 안에 있어서 여러 워커가 동시에 불러도 안전하다. */

#include <stdbool.h>
#include <stddef.h>

#include "common.h"
#include "statement.h"

#define JSON_MAX_PARAMS 64 /* params 원소 수 한도 */
#define JSON_MAX_DEPTH 32  /* 객체·배열 중첩 한도: 재귀 깊이를 묶어 스택 오버플로를 막는다 */

typedef enum {
    JSON_OK,
    JSON_BAD_REQUEST, /* JSON 문법 오류, 최상위가 객체 아님, sql 없음·문자열 아님, params 가 배열 아님 */
    JSON_BAD_PARAM,   /* JSON 은 맞지만 params 원소가 정수 (int32)·문자열이 아님, 65 개 이상, \u0000 */
    JSON_NO_MEMORY,
} JsonStatus;

typedef struct {
    char *sql;      /* malloc, \0 으로 끝남 */
    size_t sql_len;
    Value params[JSON_MAX_PARAMS]; /* VAL_INT 또는 VAL_STRING (문자열은 malloc 복사본) */
    int param_count;
} QueryRequest;

/* text[0..len) 을 {"sql": "...", "params": [...]} 로 읽는다. 키 순서는 상관없고 모르는 키는 무시한다.
 * 같은 키가 두 번 (최상위) 이면 JSON_BAD_REQUEST. 문법 오류와 params 오류가 같이 있으면 문법 오류가 우선.
 * 성공하면 out_request 를 채우고 (query_request_free 로 해제), 실패하면 out_request 는 비어 있고
 * out_message 에 이유를 쓴다 (성공이면 ""). */
JsonStatus json_parse_query(const char *text, size_t len, QueryRequest *out_request,
                            char out_message[MAX_MESSAGE]);

/* sql 과 문자열 params 를 해제한다. 두 번 불러도 안전하다 */
void query_request_free(QueryRequest *request);

#endif
