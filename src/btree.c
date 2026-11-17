#include "btree.h"

#include <stdlib.h>
#include <string.h>

#include "btree_internal.h"

/* S21 에서 리프 split 이 들어오기 전까지, 꽉 찬 리프에 넣으려는 삽입은 거부한다.
 * 배열을 넘겨 쓰지 않기 위한 임시 방어선이고 split 구현과 함께 사라진다. */
#define BTREE_LEAF_FULL -2

static BNode *node_create(bool is_leaf)
{
    BNode *node = calloc(1, sizeof(*node));
    if (node == NULL)
        return NULL;
    node->is_leaf = is_leaf;
    return node;
}

/* 내부 노드면 자식부터 재귀로 해제한다. 높이는 log_ORDER(N) 라 재귀 깊이는 작다 */
static void node_destroy(BNode *node)
{
    if (node == NULL)
        return;
    if (!node->is_leaf) {
        for (int i = 0; i <= node->num_keys; i++)
            node_destroy(node->children[i]);
    }
    free(node);
}

/* keys[0..num_keys) 에서 key 이상인 첫 위치 (lower bound). 모두 작으면 num_keys.
 * 리프에서는 "있으면 그 자리, 없으면 끼워 넣을 자리" 가 된다. */
static int node_lower_bound(const BNode *node, int32_t key)
{
    int low = 0;
    int high = node->num_keys;
    while (low < high) {
        /* low + (high - low) / 2: low + high 가 넘치지 않게 */
        int middle = low + (high - low) / 2;
        if (node->keys[middle] < key)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

/* 내부 노드에서 key 가 내려갈 자식 번호. keys[i] 이상은 children[i+1] 쪽이므로
 * key 와 같은 구분 키는 오른쪽으로 보낸다 (리프 split 이 오른쪽 첫 키를 복사해 올리기 때문). */
static int node_child_index(const BNode *node, int32_t key)
{
    int index = node_lower_bound(node, key);
    if (index < node->num_keys && node->keys[index] == key)
        index++;
    return index;
}

static const BNode *tree_find_leaf(const BTree *tree, int32_t key)
{
    const BNode *node = tree->root;
    while (!node->is_leaf)
        node = node->children[node_child_index(node, key)];
    return node;
}

/* position 부터 오른쪽으로 한 칸씩 밀고 그 자리에 (key, value) 를 넣는다. 자리가 있어야 한다 */
static void leaf_insert_at(BNode *leaf, int position, int32_t key, uint64_t value)
{
    int move_count = leaf->num_keys - position;
    memmove(&leaf->keys[position + 1], &leaf->keys[position], move_count * sizeof(leaf->keys[0]));
    memmove(&leaf->values[position + 1], &leaf->values[position],
            move_count * sizeof(leaf->values[0]));
    leaf->keys[position] = key;
    leaf->values[position] = value;
    leaf->num_keys++;
}

BTree *btree_create(void)
{
    BTree *tree = malloc(sizeof(*tree));
    if (tree == NULL)
        return NULL;
    tree->root = node_create(true);
    if (tree->root == NULL) {
        free(tree);
        return NULL;
    }
    tree->size = 0;
    tree->height = 1;
    return tree;
}

void btree_destroy(BTree *tree)
{
    if (tree == NULL)
        return;
    node_destroy(tree->root);
    free(tree);
}

int btree_insert(BTree *tree, int32_t key, uint64_t value)
{
    /* tree_find_leaf 는 읽기 전용 경로라 const 를 돌려준다. 이 트리는 호출한 쪽 소유라 풀어도 된다 */
    BNode *leaf = (BNode *)tree_find_leaf(tree, key);
    int position = node_lower_bound(leaf, key);
    if (position < leaf->num_keys && leaf->keys[position] == key)
        return BTREE_DUPLICATE;
    if (leaf->num_keys == BTREE_MAX_KEYS)
        return BTREE_LEAF_FULL;
    leaf_insert_at(leaf, position, key, value);
    tree->size++;
    return BTREE_OK;
}

bool btree_search(const BTree *tree, int32_t key, uint64_t *out_value)
{
    const BNode *leaf = tree_find_leaf(tree, key);
    int position = node_lower_bound(leaf, key);
    if (position == leaf->num_keys || leaf->keys[position] != key)
        return false;
    *out_value = leaf->values[position];
    return true;
}

int64_t btree_size(const BTree *tree)
{
    return tree->size;
}

int btree_height(const BTree *tree)
{
    return tree->height;
}
