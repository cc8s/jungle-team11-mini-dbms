#ifndef MINI_DBMS_TABLE_H
#define MINI_DBMS_TABLE_H

/* 테이블 하나 = 스키마 + 저장소 파일. 실행기가 처음 쓸 때 연다.
 * id 발급(next_id)도 여기서 한다. 동시성은 #10 에서 테이블 rwlock 을 붙인다 (D005). */

#include <stddef.h>

#include "schema.h"
#include "storage.h"

struct table {
    struct schema schema;
    struct storage storage;
};

/* schema_dir/<name>.schema 를 읽고 data_dir/<name>.tbl 을 연다(없으면 만든다).
 * 파일을 한 번 훑어 next_id 를 max(헤더 값, 가장 큰 id + 1) 로 맞춘다 (D008).
 * 성공 0, 실패 -1. */
int table_open(struct table *t, const char *schema_dir, const char *data_dir, const char *name,
               char *err, size_t err_size);

/* 행에 새 id 를 채워 붙이고 그 id 를 돌려준다. 실패하면 -1 (id 는 소모하지 않는다). */
long long table_insert(struct table *t, void *row);

void table_close(struct table *t);

#endif
