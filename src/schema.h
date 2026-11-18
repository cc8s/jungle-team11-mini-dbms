#ifndef MINI_DBMS_SCHEMA_H
#define MINI_DBMS_SCHEMA_H

/* schema/<table>.schema 를 읽는다. CREATE TABLE 은 없고 이 파일이 테이블 정의다 (D001).
 *
 *   # 주석과 빈 줄은 무시
 *   id INT              ← 첫 컬럼은 반드시 id INT (자동 증가, B+ 트리 키)
 *   name VARCHAR(32)    ← 최대 바이트 수 필수, 1~255
 *   age INT
 *
 * 행 하나는 컬럼을 순서대로 붙인 고정 길이 바이트열이다.
 *   INT        : 8바이트 (long long, 호스트 바이트 순서)
 *   VARCHAR(n) : n바이트, 남는 자리는 0 으로 채움. 정확히 n바이트면 끝에 0 이 없다 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCHEMA_MAX_COLUMNS  16
#define SCHEMA_NAME_MAX     64
#define SCHEMA_VARCHAR_MAX  255
#define SCHEMA_INT_SIZE     8

enum column_type {
    COLUMN_INT,
    COLUMN_VARCHAR
};

struct column {
    char name[SCHEMA_NAME_MAX];
    enum column_type type;
    uint32_t size;   /* 행 안에서 차지하는 바이트 수 */
    uint32_t offset; /* 행 시작에서부터의 위치 */
};

struct schema {
    char table[SCHEMA_NAME_MAX];
    int column_count;
    struct column columns[SCHEMA_MAX_COLUMNS];
    uint32_t row_size;
};

/* dir/<table>.schema 를 읽어 out 에 채운다. 성공 0, 실패 -1 (err 에 이유). */
int schema_load(const char *dir, const char *table, struct schema *out, char *err, size_t err_size);

/* 문자열 하나를 해석한다. schema_load 가 파일 내용을 넘겨 부른다. 테스트에서 직접 쓴다. */
int schema_parse(const char *text, const char *table, struct schema *out, char *err,
                 size_t err_size);

/* 컬럼 위치(0부터). 없으면 -1. 대소문자를 가리지 않는다. */
int schema_find_column(const struct schema *s, const char *name);

#endif
