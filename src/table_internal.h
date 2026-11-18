#ifndef TABLE_INTERNAL_H
#define TABLE_INTERNAL_H

/* Table 의 실제 구조. table.c 와 테스트만 include 한다 (다른 모듈은 table.h 의 불투명 Table 만 쓴다).
 * 테스트가 파일로는 만들 수 없는 상태 (next_id = INT32_MAX 인 일관된 헤더처럼 수십 GB 가 필요한 것) 를
 * 메모리에서 직접 만들 때 쓴다. */

#include <pthread.h>

#include "btree.h"
#include "table.h"

struct Table {
    int fd;
    char name[MAX_NAME];
    TableHeader header;
    int32_t column_offsets[MAX_COLUMNS]; /* 레코드 안에서 각 컬럼이 시작하는 바이트 */
    BTree *index;                        /* id → 레코드 offset. table_open 이 파일 스캔으로 재구축 */
    double index_build_ms;               /* 재구축 (복구 포함) 에 걸린 시간 */
    bool has_recovery;                   /* 열 때 헤더를 바로잡았는지 */
    TableRecovery recovery;
    /* 테이블 단위 RW 락 (쓰기 우선). 레코드 영역·헤더 카운터·인덱스를 함께 보호한다 */
    pthread_rwlock_t rwlock;
    bool has_rwlock;
};

#endif
