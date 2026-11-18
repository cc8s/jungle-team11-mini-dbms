#ifndef MINI_DBMS_EXECUTOR_H
#define MINI_DBMS_EXECUTOR_H

/* Statement 를 스키마와 대조하고 실행한다. 테이블은 처음 쓸 때 열어 끝까지 둔다 (D002).
 *
 * 탐색 방법 (D003, D004):
 *   WHERE id = n            → B+ 트리 검색      O(log N)
 *   WHERE id BETWEEN a AND b → 트리 + 리프 순회  O(log N + K)
 *   그 밖 (다른 컬럼, WHERE 없음) → 파일 선형 탐색 O(N)
 * use_index 를 끄면 모두 선형 탐색이다 (비교 측정과 결과 대조용). */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include "parser.h"
#include "result.h"
#include "table.h"

#define EXECUTOR_MAX_TABLES 16
#define EXECUTOR_PATH_MAX   256

struct executor {
    char schema_dir[EXECUTOR_PATH_MAX];
    char data_dir[EXECUTOR_PATH_MAX];
    bool use_index; /* 기본 참 */
    int table_count;
    struct table *tables[EXECUTOR_MAX_TABLES];
    pthread_mutex_t tables_lock; /* 테이블 목록 (열기) 보호. 행 접근은 테이블 락의 몫 */
};

int executor_init(struct executor *ex, const char *schema_dir, const char *data_dir);

/* 성공 0 (out 을 채움, 다 쓰면 result_free), 실패 -1 (err 에 이유).
 * 여러 스레드가 같은 실행기로 동시에 불러도 된다: 테이블별 rwlock 으로 INSERT 는 혼자, SELECT 는 함께 (D005). */
int executor_execute(struct executor *ex, const struct statement *stmt, struct result *out,
                     char *err, size_t err_size);

/* 모든 테이블을 flush 하고 닫는다 */
void executor_close(struct executor *ex);

#endif
