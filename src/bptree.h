#ifndef MINI_DBMS_BPTREE_H
#define MINI_DBMS_BPTREE_H

/* 메모리 B+ 트리: id(키) → 테이블 파일 안의 행 오프셋(값) (D003).
 *
 * - 값은 리프에만 있고, 리프끼리 next 로 이어져 있다. BETWEEN 은 시작 리프를 찾은 뒤 옆으로 읽는다 (D004).
 * - order = 노드가 가질 수 있는 최대 자식 수. 키는 최대 order - 1 개.
 * - 넘친 노드는 반으로 나눈다. 단, 맨 오른쪽 노드의 끝에 붙이다 넘치면(자동 증가 id 의 늘 있는 경우)
 *   왼쪽을 꽉 채운 채로 나눈다(append split). 반씩 나누면 왼쪽은 다시 채워지지 않아 50% 만 찬다.
 * - 삭제는 없다 (INSERT / SELECT 만).
 * - 스레드 안전하지 않다. 호출하는 쪽이 테이블 rwlock 으로 지킨다 (D005). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BPTREE_DEFAULT_ORDER 128
#define BPTREE_MIN_ORDER     3

enum bptree_result {
    BPTREE_OK = 0,
    BPTREE_DUPLICATE = 1, /* 같은 키가 이미 있음. 트리는 그대로 */
    BPTREE_NO_MEMORY = -1
};

struct bptree_node;

struct bptree {
    struct bptree_node *root;
    int order;
    int max_keys;
    bool is_append_split; /* 끄면 늘 반씩 나눈다 (비교 측정용) */
    size_t count;
    int height; /* 리프만 있으면 1 */

    /* 분할에 쓸 예비 노드. 넣기 전에 높이 + 1 개를 확보해 두어 넣는 도중에는 실패하지 않는다 */
    struct bptree_node **spares;
    int spare_count;
    int spare_cap;
};

struct bptree_stats {
    size_t leaf_count;
    size_t internal_count;
    double leaf_fill; /* 리프 키 수 합 / (리프 수 × max_keys) */
};

/* order 가 BPTREE_MIN_ORDER 보다 작으면 -1 */
int bptree_init(struct bptree *t, int order);
void bptree_free(struct bptree *t);

enum bptree_result bptree_insert(struct bptree *t, long long key, uint64_t value);

/* 찾으면 true 와 *value */
bool bptree_find(const struct bptree *t, long long key, uint64_t *value);

/* low <= key <= high 인 항목을 키 순서로 fn 에 넘긴다. fn 이 0 이 아니면 멈춘다. 넘긴 개수를 돌려준다. */
typedef int (*bptree_range_fn)(long long key, uint64_t value, void *ctx);
size_t bptree_range(const struct bptree *t, long long low, long long high, bptree_range_fn fn,
                    void *ctx);

/* 가장 큰 키. 비었으면 false */
bool bptree_max_key(const struct bptree *t, long long *key);

void bptree_stats(const struct bptree *t, struct bptree_stats *out);

/* 불변식 검사 (테스트용): 노드 안 키 정렬, 부모 구간 안의 키, 노드별 키 수 범위
 * (맨 오른쪽 노드 제외 최소 개수), 모든 리프 깊이 같음, 리프 연결 순서·개수 = count.
 * 맞으면 0, 어긋나면 -1 과 msg. */
int bptree_check(const struct bptree *t, char *msg, size_t msg_size);

#endif
