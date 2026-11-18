#ifndef ENGINE_INTERNAL_H
#define ENGINE_INTERNAL_H

/* 엔진의 락 계측. engine.c 와 테스트만 include 한다 (서버·CLI 는 engine.h 만 쓴다).
 * "검증 실패는 락을 잡지 않는다", "에러 경로에서도 락이 풀린다" 를 시간에 기대지 않고 확인하는 데 쓴다. */

#include <stdint.h>

#include "engine.h"

/* 엔진이 연 뒤로 실행 락 (GLOBAL 뮤텍스 또는 테이블 RW 락) 을 잡은 횟수 */
int64_t engine_lock_acquire_count(const Engine *engine);

/* 지금 잡혀 있는 실행 락 수 (아무 요청도 실행 중이 아니면 0) */
int engine_locks_held(const Engine *engine);

#endif
