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

/* 등록된 키 개수 */
int64_t btree_size(const BTree *tree);

/* 루트에서 리프까지 노드 수. 리프 하나뿐인 트리 (빈 트리 포함) 는 1 */
int btree_height(const BTree *tree);

#endif
