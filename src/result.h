#ifndef RESULT_H
#define RESULT_H

/* 엔진이 돌려주는 실행 결과. 엔진은 출력하지 않고 이 구조체만 채운다 (출력은 format / server 가 한다).
 * SELECT 결과는 테이블 레코드 바이트를 그대로 rows 에 복사해 두고, 셀은 result_get_* 로 읽는다. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"
#include "error.h"

#define MAX_MESSAGE 256 /* 에러 메시지 버퍼 크기 (\0 포함), 넘으면 잘린다 */

typedef struct {
    char name[MAX_NAME];
    ColumnType type;
    int size;   /* 칸의 바이트 수 */
    int offset; /* 레코드 시작부터의 바이트 위치 */
} ResultColumn;

typedef struct {
    ErrorCode error;
    char message[MAX_MESSAGE];
    int line;

    int column_count;
    ResultColumn columns[MAX_COLUMNS];
    int record_size;
    int64_t row_count;
    uint8_t *rows; /* row_count * record_size 바이트, malloc */

    int64_t affected_rows;
    int32_t last_insert_id;
    bool used_index;
    double elapsed_ms;
} Result;

/* 빈 성공 결과로 초기화한다. */
void result_init(Result *result);
/* 에러 결과를 만든다. message 는 printf 형식이며 MAX_MESSAGE - 1 바이트에서 잘린다. */
Result result_error(ErrorCode code, int line, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
/* rows 를 해제한다. 구조체 자체는 호출한 쪽 소유. NULL 이나 두 번 호출해도 안전하다. */
void result_free(Result *result);

/* row 행 col 열의 INT 값. 범위 밖 인덱스는 프로그래머 실수라 assert 로 막는다. */
int32_t result_get_int(const Result *result, int64_t row, int col);
/* row 행 col 열의 VARCHAR 칸 시작 주소. \0 으로 끝난다는 보장이 없으므로 반드시 out_len 만큼만 읽는다. */
const char *result_get_str(const Result *result, int64_t row, int col, size_t *out_len);

#endif
