#ifndef BTREE_H
#define BTREE_H

/* 메모리 기반 B+ 트리. key 는 레코드 id (int32_t 전체 범위), value 는 레코드 offset.
 * 엔진과 독립된 모듈이라 단독으로 테스트할 수 있다. 스레드 안전하지 않다
 * (Phase 3 에서 테이블 RW 락이 트리와 파일을 함께 보호한다). */

#include <stdbool.h>
#include <stdint.h>

/* 노드당 최대 자식 수. 노드 하나는 최대 BTREE_ORDER - 1 개의 키를 담는다.
 * 컴파일 옵션 (-DBTREE_ORDER=4) 으로 바꿔 split 경로를 많이 밟게 하거나 높이를 비교한다. */
#ifndef BTREE_ORDER
#define BTREE_ORDER 128
#endif

_Static_assert(BTREE_ORDER >= 3, "BTREE_ORDER must be at least 3");

#define BTREE_OK 0         /* btree_insert 성공 */
#define BTREE_DUPLICATE -1 /* 이미 있는 키라서 삽입하지 않음 */
#define BTREE_NO_MEMORY -2 /* 노드 할당 실패, 트리는 삽입 전 그대로 */

typedef struct BTree BTree;

/* 빈 리프 하나를 루트로 하는 트리를 만든다 (height = 1). 메모리가 모자라면 NULL */
BTree *btree_create(void);

/* 모든 노드를 해제한다. NULL 이면 아무 일도 하지 않는다 */
void btree_destroy(BTree *tree);

/* key → value 를 등록한다. 성공하면 BTREE_OK, 이미 있는 키면 BTREE_DUPLICATE 이고
 * 트리는 바뀌지 않는다 (기존 값 유지). 메모리가 모자라면 BTREE_NO_MEMORY. */
int btree_insert(BTree *tree, int32_t key, uint64_t value);

/* key 를 찾으면 *out_value 에 값을 쓰고 true. 없으면 false 이고 *out_value 는 건드리지 않는다 */
bool btree_search(const BTree *tree, int32_t key, uint64_t *out_value);

/* ---- 범위 순회 ----
 * 반복자는 "다음에 돌려줄 리프와 그 안의 위치" 다. 값으로 주고받으며 해제할 것이 없다.
 * 필드는 btree 모듈 안에서만 쓴다 (호출하는 쪽은 seek 으로 만들고 iter_next 로만 움직인다).
 * 순회 중에 트리를 바꾸면 반복자는 무효가 된다 (Phase 3 에서는 읽기 락 안에서만 순회). */
typedef struct {
    const struct BNode *leaf; /* NULL 이면 끝 */
    int index;
} BTreeIter;

/* low 이상인 첫 키부터 순회하는 반복자. 그런 키가 없으면 첫 btree_iter_next 가 false */
BTreeIter btree_seek(const BTree *tree, int32_t low);

/* 가장 작은 키부터 순회하는 반복자 (가장 왼쪽 리프의 처음) */
BTreeIter btree_seek_first(const BTree *tree);

/* 다음 (key, value) 를 out 인자에 쓰고 한 칸 나아간다. 리프 끝이면 next 리프로 넘어간다.
 * 더 없으면 false 이고 out 인자는 건드리지 않는다. 끝난 뒤 다시 불러도 계속 false */
bool btree_iter_next(BTreeIter *iter, int32_t *out_key, uint64_t *out_value);

/* 등록된 키 개수 */
int64_t btree_size(const BTree *tree);

/* 루트에서 리프까지 노드 수. 리프 하나뿐인 트리 (빈 트리 포함) 는 1 */
int btree_height(const BTree *tree);

/* 테스트·디버깅용 불변식 검사. 모두 만족하면 true:
 * - 노드 안 키 오름차순, 키 개수 범위 (루트 제외 최소 ⌈ORDER/2⌉ - 1)
 * - 내부 노드: 자식 수 = 키 수 + 1, 자식의 키가 구분 키 구간 안에 있음
 * - 모든 리프의 깊이가 같고 = height
 * - 리프 연결 리스트가 왼쪽부터 오름차순이고 키 개수 합 = size */
bool btree_check(const BTree *tree);

#endif
