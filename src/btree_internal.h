#ifndef BTREE_INTERNAL_H
#define BTREE_INTERNAL_H

/* B+ 트리 노드 구조. btree.c 와 tests/test_btree.c 만 include 한다.
 * 다른 모듈은 btree.h 의 불투명 BTree 만 쓴다 (노드 배치를 바꿔도 영향이 없도록). */

#include "btree.h"

#define BTREE_MAX_KEYS (BTREE_ORDER - 1)

typedef struct BNode {
    bool is_leaf;
    int num_keys;
    int32_t keys[BTREE_MAX_KEYS]; /* 노드 안에서 오름차순 */
    union {
        struct BNode *children[BTREE_ORDER]; /* 내부 노드: keys[i] 이상은 children[i+1] 쪽 */
        uint64_t values[BTREE_MAX_KEYS];     /* 리프: keys[i] 의 레코드 offset */
    };
    struct BNode *next; /* 리프: 오른쪽 형제 (범위 순회용), 가장 오른쪽은 NULL */
} BNode;

#define BTREE_UNLIMITED_ALLOCS -1

struct BTree {
    BNode *root;
    int64_t size;
    int height;
    /* 테스트용 할당 실패 주입: 0 이상이면 노드를 그만큼만 더 할당하고 그다음부터 실패한다.
     * 전역 변수 대신 트리 안에 둬서 트리마다 독립적이다. 평소에는 BTREE_UNLIMITED_ALLOCS */
    int alloc_budget;
};

#endif
