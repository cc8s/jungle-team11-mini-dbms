#include "btree.h"

#include <stdlib.h>
#include <string.h>

#include "btree_internal.h"

/* S22 에서 내부 노드 split 이 들어오기 전까지, 꽉 찬 내부 노드로 split 이 번지는 삽입은
 * 아무것도 바꾸기 전에 거부한다. 배열을 넘겨 쓰지 않기 위한 임시 방어선이다. */
#define BTREE_INTERNAL_FULL -3

/* 리프 split 때 왼쪽에 남기는 키 개수 ⌈(ORDER-1)/2⌉. 넘친 ORDER 개를 반으로 나눈다 */
#define LEAF_SPLIT_LEFT_COUNT ((BTREE_MAX_KEYS + 1) / 2)

/* 자식이 split 되었을 때 부모에게 넘기는 것: 구분 키와 새로 생긴 오른쪽 노드.
 * split 이 없었으면 right = NULL */
typedef struct {
    int32_t key;
    BNode *right;
} SplitResult;

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

/* 꽉 찬 leaf 에 (key, value) 를 넣으면 ORDER 개가 되므로 반으로 나눈다.
 * 오른쪽 리프의 첫 키를 부모로 "복사" 한다 — 리프는 실제 데이터를 가지므로 키가 리프에도 남아야 하고,
 * 부모 키는 길 안내용 사본일 뿐이다. 새 노드를 먼저 할당해서, 실패하면 leaf 는 그대로다. */
static int leaf_split(BNode *leaf, int position, int32_t key, uint64_t value,
                      SplitResult *out_split)
{
    BNode *right = node_create(true);
    if (right == NULL)
        return BTREE_NO_MEMORY;

    /* 넘친 ORDER 개를 임시 배열에서 정렬된 채로 만든 뒤 나눠 담는다 */
    int32_t keys[BTREE_ORDER];
    uint64_t values[BTREE_ORDER];
    memcpy(keys, leaf->keys, position * sizeof(keys[0]));
    memcpy(values, leaf->values, position * sizeof(values[0]));
    keys[position] = key;
    values[position] = value;
    int tail_count = BTREE_MAX_KEYS - position;
    memcpy(&keys[position + 1], &leaf->keys[position], tail_count * sizeof(keys[0]));
    memcpy(&values[position + 1], &leaf->values[position], tail_count * sizeof(values[0]));

    int right_count = BTREE_ORDER - LEAF_SPLIT_LEFT_COUNT;
    memcpy(leaf->keys, keys, LEAF_SPLIT_LEFT_COUNT * sizeof(keys[0]));
    memcpy(leaf->values, values, LEAF_SPLIT_LEFT_COUNT * sizeof(values[0]));
    memcpy(right->keys, &keys[LEAF_SPLIT_LEFT_COUNT], right_count * sizeof(keys[0]));
    memcpy(right->values, &values[LEAF_SPLIT_LEFT_COUNT], right_count * sizeof(values[0]));
    leaf->num_keys = LEAF_SPLIT_LEFT_COUNT;
    right->num_keys = right_count;

    /* 리프 연결: leaf → right → (원래 leaf 의 오른쪽) */
    right->next = leaf->next;
    leaf->next = right;
    out_split->key = right->keys[0];
    out_split->right = right;
    return BTREE_OK;
}

/* 내부 노드의 child_index 번 자식이 split 되었다: 구분 키는 keys[child_index] 에,
 * 새 오른쪽 자식은 children[child_index + 1] 에 끼운다. 자리가 있어야 한다 */
static void internal_insert_at(BNode *node, int child_index, const SplitResult *split)
{
    int move_count = node->num_keys - child_index;
    memmove(&node->keys[child_index + 1], &node->keys[child_index],
            move_count * sizeof(node->keys[0]));
    memmove(&node->children[child_index + 2], &node->children[child_index + 1],
            move_count * sizeof(node->children[0]));
    node->keys[child_index] = split->key;
    node->children[child_index + 1] = split->right;
    node->num_keys++;
}

/* node 아래에 (key, value) 를 넣는다. 재귀로 리프까지 내려갔다가 돌아오면서,
 * 자식이 split 되었으면 그 결과를 이 노드에 흡수한다. 이 노드도 split 되면 out_split 에 담아 위로 보낸다 */
static int node_insert(BNode *node, int32_t key, uint64_t value, SplitResult *out_split)
{
    out_split->right = NULL;
    if (node->is_leaf) {
        int position = node_lower_bound(node, key);
        if (position < node->num_keys && node->keys[position] == key)
            return BTREE_DUPLICATE;
        if (node->num_keys < BTREE_MAX_KEYS) {
            leaf_insert_at(node, position, key, value);
            return BTREE_OK;
        }
        return leaf_split(node, position, key, value, out_split);
    }

    int child_index = node_child_index(node, key);
    SplitResult child_split;
    int status = node_insert(node->children[child_index], key, value, &child_split);
    if (status != BTREE_OK || child_split.right == NULL)
        return status;
    if (node->num_keys == BTREE_MAX_KEYS)
        return BTREE_INTERNAL_FULL; /* 미리 거부하므로 오지 않는다 (S22 에서 내부 split) */
    internal_insert_at(node, child_index, &child_split);
    return BTREE_OK;
}

/* 임시 (S22 전까지): 이 삽입이 리프를 split 시키고 그 부모도 꽉 차 있는지.
 * 부모에 자리가 있으면 split 은 거기서 멈추므로 바로 위 부모만 보면 된다 */
static bool insert_reaches_full_internal(const BTree *tree, int32_t key)
{
    const BNode *parent = NULL;
    const BNode *node = tree->root;
    while (!node->is_leaf) {
        parent = node;
        node = node->children[node_child_index(node, key)];
    }
    return node->num_keys == BTREE_MAX_KEYS && parent != NULL &&
           parent->num_keys == BTREE_MAX_KEYS;
}

/* 루트가 split 되었다: 옛 루트와 새 오른쪽 노드를 자식으로 갖는 새 루트를 올린다 (높이 +1).
 * B+ 트리는 아래가 아니라 위로 자라므로 모든 리프의 깊이가 늘 같다 */
static void tree_grow_root(BTree *tree, BNode *new_root, const SplitResult *split)
{
    new_root->keys[0] = split->key;
    new_root->children[0] = tree->root;
    new_root->children[1] = split->right;
    new_root->num_keys = 1;
    tree->root = new_root;
    tree->height++;
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
    uint64_t existing;
    if (btree_search(tree, key, &existing))
        return BTREE_DUPLICATE;
    if (insert_reaches_full_internal(tree, key))
        return BTREE_INTERNAL_FULL;

    /* 루트가 꽉 찼으면 split 될 수 있으니 새 루트를 미리 할당해 둔다.
     * split 이 일어난 뒤에 할당이 실패하면 되돌리기 어렵기 때문이다 (안 쓰면 해제) */
    BNode *spare_root = NULL;
    if (tree->root->num_keys == BTREE_MAX_KEYS) {
        spare_root = node_create(false);
        if (spare_root == NULL)
            return BTREE_NO_MEMORY;
    }

    SplitResult split;
    int status = node_insert(tree->root, key, value, &split);
    if (status == BTREE_OK && split.right != NULL) {
        tree_grow_root(tree, spare_root, &split);
        spare_root = NULL;
    }
    free(spare_root);
    if (status == BTREE_OK)
        tree->size++;
    return status;
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
