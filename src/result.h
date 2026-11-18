#ifndef MINI_DBMS_RESULT_H
#define MINI_DBMS_RESULT_H

/* 실행 결과. SELECT 는 고른 행을 문자열로 바꾸지 않고 행 바이트열 그대로 모아 둔다.
 * 출력(CLI 표, 나중에 JSON)을 만들 때 schema 로 해석한다. 100만 행이어도 행 크기만큼만 쓴다. */

#include <stdio.h>

#include "schema.h"

enum result_type {
    RESULT_INSERT,
    RESULT_SELECT
};

struct result {
    enum result_type type;
    long long inserted_id; /* INSERT */

    /* SELECT */
    const struct schema *schema;
    int column_count;
    int columns[SCHEMA_MAX_COLUMNS]; /* 출력할 컬럼의 스키마 위치, 출력 순서대로 */
    char *rows;
    size_t row_count;
    size_t row_cap;
};

void result_init(struct result *r);

/* 행 하나(schema->row_size 바이트)를 복사해 붙인다. 성공 0, 메모리 부족 -1. */
int result_add_row(struct result *r, const void *row);

const void *result_row(const struct result *r, size_t index);

/* CLI 용 표:  id | name   (헤더) / 1 | kim  (행) / (1 row) */
void result_print(const struct result *r, FILE *out);

void result_free(struct result *r);

#endif
