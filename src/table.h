#ifndef MINI_DBMS_TABLE_H
#define MINI_DBMS_TABLE_H

/* 테이블 하나 = 스키마 + 저장소 파일 + id 인덱스(B+ 트리). 실행기가 처음 쓸 때 연다.
 * id 발급(next_id)도 여기서 한다. 동시성은 #10 에서 테이블 rwlock 을 붙인다 (D005). */

#include <stdbool.h>
#include <stddef.h>

#include "bptree.h"
#include "schema.h"
#include "storage.h"

struct table {
    struct schema schema;
    struct storage storage;
    struct bptree index;  /* id → 행 오프셋. 메모리에만 있고 열 때 다시 만든다 (D003) */
    bool is_index_valid;  /* 거짓이면 실행기는 인덱스 대신 선형 탐색을 쓴다 */
};

/* schema_dir/<name>.schema 를 읽고 data_dir/<name>.tbl 을 연다(없으면 만든다).
 * 파일을 한 번 훑어 B+ 트리를 다시 만들고, next_id 를 max(헤더 값, 가장 큰 id + 1) 로 맞춘다 (D008).
 * 파일 안에 같은 id 가 두 번 있으면 손상으로 보고 열지 않는다. 성공 0, 실패 -1. */
int table_open(struct table *t, const char *schema_dir, const char *data_dir, const char *name,
               char *err, size_t err_size);

/* 행에 새 id 를 채워 붙이고 그 id 를 돌려준다. 실패하면 -1 (id 는 소모하지 않는다). */
long long table_insert(struct table *t, void *row);

void table_close(struct table *t);

#endif
