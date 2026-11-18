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

/* SELECT 가 행을 찾은 방법 */
enum result_plan {
    RESULT_PLAN_SCAN,         /* 파일 전체 선형 탐색 */
    RESULT_PLAN_INDEX_LOOKUP, /* B+ 트리 검색 (WHERE id = n) */
    RESULT_PLAN_INDEX_RANGE   /* B+ 트리 + 리프 순회 (WHERE id BETWEEN) */
};

struct result {
    enum result_type type;
    long long inserted_id; /* INSERT */
    double table_open_ms;  /* 이 문장이 테이블을 처음 열었으면 그 시간(인덱스 재구성 포함), 아니면 0 */

    /* SELECT */
    enum result_plan plan;
    size_t rows_examined; /* 조건을 확인한 행 수: 선형이면 전체, 인덱스면 찾은 것만 */
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

/* "index lookup" / "index range" / "full scan" */
const char *result_plan_name(enum result_plan plan);

void result_free(struct result *r);

#endif
