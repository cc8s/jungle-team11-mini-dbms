#ifndef ENGINE_H
#define ENGINE_H

/* 데이터 디렉터리 하나 = 데이터베이스 하나. SQL 텍스트를 받아 파싱 → 검증 → 실행하고 Result 를 돌려준다.
 * 출력하지 않는다 (CLI, API 서버가 Result 를 각자 형식으로 낸다). */

#include "common.h"
#include "error.h"
#include "result.h"
#include "statement.h"

typedef struct Engine Engine;

/* data_dir 의 *.tbl 을 모두 연다. 테이블 이름은 파일 이름에서 .tbl 을 뗀 소문자.
 * .tbl 이 아닌 파일은 무시한다. 하나라도 열지 못하면 전체 실패: NULL 을 돌려주고
 * *out_error (ERR_IO, ERR_CORRUPT, ERR_INTERNAL) 와 out_message (문제 파일 이름 포함) 를 채운다. */
Engine *engine_open(const char *data_dir, ErrorCode *out_error, char out_message[MAX_MESSAGE]);
void engine_close(Engine *engine);

/* sql 한 문장을 실행한다 (단일 문장 모드: 두 번째 문장은 ERR_UNSUPPORTED, 빈 입력은 ERR_SYNTAX).
 * 결과는 호출한 쪽이 result_free 한다. params 는 Phase 3 바인딩용 (지금은 param_count = 0 만). */
Result engine_execute(Engine *engine, const char *sql, const Value *params, int param_count);

#endif
