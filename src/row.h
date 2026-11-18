#ifndef MINI_DBMS_ROW_H
#define MINI_DBMS_ROW_H

/* 고정 길이 행 바이트열에 컬럼 값을 쓰고 읽는다. 배치는 schema.h 참고. */

#include <stddef.h>

#include "schema.h"

void row_set_int(const struct schema *s, int col, void *row, long long value);
long long row_get_int(const struct schema *s, int col, const void *row);

/* 길이가 컬럼 크기를 넘으면 잘라내지 않고 -1 (D001). 성공 0. */
int row_set_string(const struct schema *s, int col, void *row, const char *value);

/* dst 에 NUL 로 끝나는 문자열을 쓴다. dst_size 는 컬럼 크기 + 1 이상이어야 다 담긴다. */
void row_get_string(const struct schema *s, int col, const void *row, char *dst, size_t dst_size);

#endif
