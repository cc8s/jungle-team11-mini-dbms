#ifndef MINI_DBMS_API_H
#define MINI_DBMS_API_H

/* HTTP 본문(JSON) ↔ 실행기. 엔진은 HTTP 를 모르고, 이 모듈이 둘을 잇는다 (D006).
 *
 *   요청: {"sql": "SELECT * FROM users WHERE id = 3"}      (문장 하나)
 *   SELECT 200: {"ok":true,"columns":["id","name"],"rows":[[3,"kim"]],"row_count":1,
 *                "truncated":false,"plan":"index lookup","rows_examined":1,"elapsed_ms":0.01}
 *   INSERT 200: {"ok":true,"inserted_id":7,"elapsed_ms":0.02}
 *   오류   400: {"ok":false,"error":"syntax error near 'FORM': expected FROM"}
 *
 * 응답 rows 는 API_MAX_ROWS 개까지만 담는다. 넘으면 truncated 가 true 이고 row_count 는 전체 개수다.
 * (100만 행 SELECT * 가 JSON 100MB 가 되어 서버 메모리와 연결을 잡아먹지 않게) */

#include <stddef.h>

#include "executor.h"

#define API_MAX_ROWS 10000

/* 응답 JSON 을 malloc 해서 돌려주고 *status 에 HTTP 상태를 쓴다. 메모리 부족이면 NULL */
char *api_handle_query(struct executor *ex, const char *body, size_t body_len, int *status);

/* {"ok":false,"error":msg} — 메모리 부족이면 NULL */
char *api_error_json(const char *msg);

#endif
