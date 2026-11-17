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
 *   ERR_PARAM           바인딩되지 않은 ? 가 남아 있음 (CLI 경로)
 *   ERR_UNKNOWN_TABLE / ERR_UNKNOWN_COLUMN
 *   ERR_AUTO_ID         값 개수 = 컬럼 수 (id 까지 넣음), ERR_VALUE_COUNT 그 밖의 개수 불일치
 *   ERR_TYPE_MISMATCH   암묵 변환 없음, ERR_VALUE_TOO_LONG VARCHAR 바이트 수 초과 */
ErrorCode validator_check(const Statement *stmt, Table *const *tables, int table_count,
                          Plan *out_plan, char out_message[MAX_MESSAGE]);

#endif
