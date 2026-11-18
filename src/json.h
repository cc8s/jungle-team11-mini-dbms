#ifndef JSON_H
#define JSON_H

/* API 의 JSON: 요청 본문 파싱 (QueryRequest) 과 응답 만들기 (Result → JSON).
 * - 파서는 범용 트리를 만들지 않고 sql 과 params 만 뽑아 Value 로 바로 만든다. 나머지 값은 문법만 검사한다.
 * - 응답은 늘어나는 버퍼 (JsonBuf) 에 이어 붙인다.
 * 상태는 모두 호출 스택과 인자 안에 있어서 여러 워커가 동시에 불러도 안전하다 (엔진 락 밖에서 부른다). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"
#include "result.h"
#include "statement.h"

#define JSON_MAX_PARAMS 64 /* params 원소 수 한도 */
#define JSON_MAX_DEPTH 32  /* 객체·배열 중첩 한도: 재귀 깊이를 묶어 스택 오버플로를 막는다 */
#define JSON_DEFAULT_MAX_ROWS 1000 /* 응답에 담는 SELECT 행 수 기본 상한 (서버 옵션) */

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

/* ---- 응답 ---- */

/* 늘어나는 문자열 버퍼. 할당이 한 번 실패하면 is_out_of_memory 가 남고 이후 append 는 아무것도 하지 않는다.
 * 그래서 append 마다 검사하지 않고 다 만든 뒤 한 번만 확인한다. data 는 \0 으로 끝난다 (len 에는 미포함) */
typedef struct {
    char *data;
    size_t len;
    size_t capacity;
    bool is_out_of_memory;
} JsonBuf;

void json_buf_init(JsonBuf *buf);
void json_buf_free(JsonBuf *buf);
void json_buf_append(JsonBuf *buf, const char *text, size_t len);
/* \0 으로 끝나는 text 를 그대로 붙인다 */
void json_buf_append_text(JsonBuf *buf, const char *text);
/* text 의 len 바이트를 따옴표로 감싼 JSON 문자열로 붙인다. " \ 와 0x00~0x1F 제어 문자를 이스케이프하고
 * 나머지 바이트 (UTF-8 한글 등) 는 그대로 둔다 */
void json_buf_append_escaped(JsonBuf *buf, const char *text, size_t len);
void json_buf_append_int(JsonBuf *buf, int64_t number);

/* result 를 API 응답 JSON 으로 out_buf 에 쓴다 (out_buf 는 json_buf_init 한 것).
 *   SELECT {"ok":true,"columns":[...],"rows":[[...]],"row_count":N,"truncated":B,
 *           "used_index":B,"elapsed_ms":F}
 *   INSERT {"ok":true,"affected_rows":N,"last_insert_id":N,"elapsed_ms":F}
 *   에러   {"ok":false,"error":{"code":"UNKNOWN_COLUMN","message":"..."}}
 * SELECT 행은 max_rows 개까지만 담고, 넘으면 truncated 가 true 다 (row_count 는 전체 행 수).
 * column_count > 0 이면 SELECT 로 본다. 메모리가 모자라면 ERR_INTERNAL */
ErrorCode json_write_result(const Result *result, int64_t max_rows, JsonBuf *out_buf);

#endif
