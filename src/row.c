#include "row.h"

#include <string.h>

/* 행 버퍼는 정렬이 보장되지 않으므로 long long 을 직접 역참조하지 않고 memcpy 로 옮긴다 */
void row_set_int(const struct schema *s, int col, void *row, long long value)
{
    memcpy((char *)row + s->columns[col].offset, &value, sizeof(value));
}

long long row_get_int(const struct schema *s, int col, const void *row)
{
    long long value;

    memcpy(&value, (const char *)row + s->columns[col].offset, sizeof(value));
    return value;
}

int row_set_string(const struct schema *s, int col, void *row, const char *value)
{
    const struct column *c = &s->columns[col];
    size_t len = strlen(value);

    if (len > c->size) {
        return -1;
    }
    char *dst = (char *)row + c->offset;
    memcpy(dst, value, len);
    memset(dst + len, 0, c->size - len);
    return 0;
}

void row_get_string(const struct schema *s, int col, const void *row, char *dst, size_t dst_size)
{
    const struct column *c = &s->columns[col];
    const char *src = (const char *)row + c->offset;
    size_t len = strnlen(src, c->size);

    if (dst_size == 0) {
        return;
    }
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}
