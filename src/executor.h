#ifndef EXECUTOR_H
#define EXECUTOR_H

/* 검증을 통과한 문장을 실행해 Result 를 만든다. 테이블 파일을 읽고 쓰는 유일한 단계라
 * Phase 3 에서 엔진이 이 호출만 테이블 락으로 감싼다. 출력은 하지 않는다. */

#include "result.h"
#include "statement.h"
#include "validator.h"

/* stmt 는 validator_check 를 통과한 것, plan 은 그때 채운 것이어야 한다 */
Result executor_run(const Statement *stmt, const Plan *plan);

#endif
