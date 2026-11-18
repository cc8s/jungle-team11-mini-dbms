#ifndef VALIDATOR_H
#define VALIDATOR_H

/* 의미 검사: 파서가 만든 Statement 를 열린 테이블의 스키마와 대조한다.
 * 통과하면 실행기가 다시 찾지 않도록 컬럼 인덱스·오프셋을 Plan 에 미리 계산해 둔다.
 * 파일을 읽거나 쓰지 않고 스키마(메모리)만 보므로 락 없이 불러도 된다 (스키마는 열린 뒤 바뀌지 않음). */

#include <stdbool.h>

#include "common.h"
#include "error.h"
#include "result.h"
#include "statement.h"
#include "table.h"

typedef struct {
    Table *table; /* 문장이 쓰는 테이블 */

    /* SELECT: 결과 컬럼 (SELECT * 면 테이블 컬럼 전부, 아니면 쓴 순서와 중복 그대로) */
    int column_count;
    ResultColumn columns[MAX_COLUMNS]; /* offset 은 원본 레코드 안의 위치 */
    int column_indexes[MAX_COLUMNS];   /* 테이블 컬럼 번호 */

    /* SELECT WHERE */
    bool has_where;
    int where_column;     /* 테이블 컬럼 번호 */
    int32_t where_offset; /* 레코드 안의 위치 */
} Plan;

/* tables 에서 name 과 정확히 같은 이름의 테이블을 찾는다 (이름은 모두 소문자). 없으면 NULL */
Table *validator_find_table(Table *const *tables, int table_count, const char *name);

/* stmt 를 검사하고 out_plan 을 채운다. 실패하면 에러 코드와 out_message 에 설명을 쓴다
 * (성공이면 out_message 는 ""). 검사 규칙은 계획서 3.4, 3.7:
 *   ERR_UNSUPPORTED     CREATE TABLE (init 명령에서만 허용)
 *   ERR_PARAM           바인딩되지 않은 ? 가 남아 있음 (CLI 경로. API 는 validator_bind_params 를 먼저 부른다)
 *   ERR_UNKNOWN_TABLE / ERR_UNKNOWN_COLUMN
 *   ERR_AUTO_ID         값 개수 = 컬럼 수 (id 까지 넣음), ERR_VALUE_COUNT 그 밖의 개수 불일치
 *   ERR_TYPE_MISMATCH   암묵 변환 없음, ERR_VALUE_TOO_LONG VARCHAR 바이트 수 초과 */
ErrorCode validator_check(const Statement *stmt, Table *const *tables, int table_count,
                          Plan *out_plan, char out_message[MAX_MESSAGE]);

/* Prepared Statement 바인딩 (API 경로). stmt 의 ? 자리 (값 자리만 있다: INSERT 값, WHERE 오른쪽) 를
 * params 의 값으로 바꾸고 param_count 를 0 으로 둔다. 그다음 validator_check 가 바뀐 값의 타입·길이를
 * 보통 값처럼 검사한다 (INT 자리에 문자열이면 ERR_TYPE_MISMATCH, 길이 초과면 ERR_VALUE_TOO_LONG).
 * 값은 토크나이저·파서를 거치지 않는다. 실패:
 *   ERR_PARAM    ? 개수와 param_count 가 다름 (? 없이 params 만 있는 경우 포함), 정수·문자열이 아닌 값,
 *                \0 이 든 문자열
 *   ERR_INTERNAL 문자열 복사 메모리 부족
 * 실패해도 이미 바꾼 값은 stmt 가 소유하므로 statement_free 로 해제된다. params 는 복사만 한다. */
ErrorCode validator_bind_params(Statement *stmt, const Value *params, int param_count,
                                char out_message[MAX_MESSAGE]);

#endif
