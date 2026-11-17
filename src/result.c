#include "result.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void result_init(Result *result)
{
    memset(result, 0, sizeof(*result));
    result->error = ERR_NONE;
}

Result result_error(ErrorCode code, int line, const char *format, ...)
{
    Result result;
    result_init(&result);
    result.error = code;
    result.line = line;

    /* vsnprintf 는 버퍼 크기를 넘지 않고 항상 \0 으로 끝내므로 긴 메시지는 안전하게 잘린다 */
    va_list args;
    va_start(args, format);
    vsnprintf(result.message, sizeof(result.message), format, args);
    va_end(args);
    return result;
}

void result_free(Result *result)
{
    if (result == NULL)
        return;
    free(result->rows);
    result->rows = NULL;
    result->row_count = 0;
}

static const uint8_t *cell_at(const Result *result, int64_t row, int col)
{
    assert(result->rows != NULL);
    assert(row >= 0 && row < result->row_count);
    assert(col >= 0 && col < result->column_count);
    return result->rows + row * result->record_size + result->columns[col].offset;
}

int32_t result_get_int(const Result *result, int64_t row, int col)
{
    assert(result->columns[col].type == COL_INT);
    const uint8_t *cell = cell_at(result, row, col);

    /* 파일 형식이 리틀 엔디언으로 고정이라 호스트 바이트 순서와 정렬에 기대지 않고 바이트로 조립한다 */
    uint32_t bits = (uint32_t)cell[0] | (uint32_t)cell[1] << 8 | (uint32_t)cell[2] << 16 |
                    (uint32_t)cell[3] << 24;
    int32_t number;
    memcpy(&number, &bits, sizeof(number));
    return number;
}

const char *result_get_str(const Result *result, int64_t row, int col, size_t *out_len)
{
    assert(result->columns[col].type == COL_VARCHAR);
    const char *cell = (const char *)cell_at(result, row, col);

    /* VARCHAR(n) 이 꽉 차면 \0 이 없으므로 strlen 대신 칸 크기 안에서만 센다 */
    *out_len = strnlen(cell, (size_t)result->columns[col].size);
    return cell;
}
