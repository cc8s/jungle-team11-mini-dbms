#ifndef MINI_DBMS_STORAGE_H
#define MINI_DBMS_STORAGE_H

/* data/<table>.tbl 고정 길이 행 파일 (D001, D002).
 *
 *   [헤더 32바이트: "MDB1" | row_size u32 | next_id i64 | 예약]
 *   [행 0][행 1][행 2] ...      ← 행 i 의 오프셋 = 32 + i * row_size
 *
 * - 파일은 열어 둔 채로 쓴다. INSERT 는 메모리 쓰기 버퍼에 모았다가 한꺼번에 pwrite 한다.
 * - 읽기는 pread 라서 파일 위치를 공유하지 않는다. 그래서 여러 SELECT 가 동시에 읽어도 된다.
 * - 버퍼에 남은 행을 읽으려 하면 먼저 flush 한다. flush 는 내부 mutex 로 한 번에 하나만.
 * - 행 수는 헤더에 두지 않고 파일 크기에서 계산한다. 쓰다가 죽어 끝에 반쪽 행이 남아도 무시된다.
 *
 * 동시성 약속 (호출하는 쪽이 테이블 rwlock 으로 지킨다, D005):
 *   쓰기 락 필요: storage_append, storage_close, next_id 변경
 *   읽기 락으로 충분: storage_read, storage_scan, storage_flush, storage_row_count */

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define STORAGE_HEADER_SIZE  32
#define STORAGE_WRITE_BUFFER (64 * 1024)

struct storage {
    int fd;
    uint32_t row_size;
    uint64_t row_count;     /* 버퍼에 있는 행까지 포함 */
    uint64_t flushed_count; /* 파일에 실제로 쓰인 행 */
    long long next_id;      /* 다음 INSERT 가 받을 id. flush 때 헤더에 기록 */

    char *wbuf;
    size_t wbuf_len;
    size_t wbuf_cap;
    pthread_mutex_t flush_lock; /* wbuf, wbuf_len, flushed_count 보호 */
};

/* 파일을 열거나 새로 만든다. 기존 파일의 row_size 가 스키마와 다르면 실패.
 * 성공 0, 실패 -1 (err 에 이유). */
int storage_open(struct storage *st, const char *path, uint32_t row_size, char *err,
                 size_t err_size);

/* 행을 끝에 붙이고 그 오프셋을 돌려준다. 성공 0, 실패 -1. */
int storage_append(struct storage *st, const void *row, uint64_t *offset_out);

/* 오프셋의 행 하나를 row 에 읽는다. 성공 0, 범위 밖이거나 실패 -1. */
int storage_read(struct storage *st, uint64_t offset, void *row);

/* 모든 행을 앞에서부터 fn 에 넘긴다. fn 이 0 이 아닌 값을 돌려주면 멈춘다.
 * 성공 0 (중간에 멈춰도 0), 읽기 실패 -1. */
typedef int (*storage_scan_fn)(const void *row, uint64_t offset, void *ctx);
int storage_scan(struct storage *st, storage_scan_fn fn, void *ctx);

/* 버퍼의 행과 헤더(next_id)를 파일에 쓴다. 성공 0, 실패 -1. */
int storage_flush(struct storage *st);

uint64_t storage_row_count(const struct storage *st);

/* flush 하고 닫는다. 성공 0, flush 실패 -1 (그래도 닫기는 한다). */
int storage_close(struct storage *st);

#endif
