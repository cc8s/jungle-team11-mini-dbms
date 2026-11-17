#include "btree.h"

#include <stdlib.h>
#include <string.h>

#include "btree_internal.h"

/* 리프 split 때 왼쪽에 남기는 키 개수 ⌈(ORDER-1)/2⌉. 넘친 ORDER 개를 반으로 나눈다 */
#define LEAF_SPLIT_LEFT_COUNT ((BTREE_MAX_KEYS + 1) / 2)
/* 내부 split 때 부모로 올라가는 가운데 키의 위치. 왼쪽은 이 개수만큼 키를 갖는다 */
#define INTERNAL_SPLIT_MIDDLE (BTREE_ORDER / 2)
/* 루트가 아닌 노드의 최소 키 개수 ⌈ORDER/2⌉ - 1. 두 split 모두 양쪽이 이 이상이 되게 나눈다 */
#define MIN_KEYS_NON_ROOT ((BTREE_ORDER + 1) / 2 - 1)

/* 자식이 split 되었을 때 부모에게 넘기는 것: 구분 키와 새로 생긴 오른쪽 노드.
 * split 이 없었으면 right = NULL */
typedef struct {
    int32_t key;
    BNode *right;
} SplitResult;

/* 삽입 한 번에 필요한 새 노드를 미리 할당해 둔 목록. 노드의 next 칸으로 엮는다.
 * split 을 시작한 뒤에는 할당이 실패할 일이 없게 해서, 실패하면 트리가 삽입 전 그대로 남는다. */
typedef struct {
    BNode *head;
} NodePool;

/* ---- 노드 할당과 해제 ---- */

/* 테스트가 alloc_budget 을 0 이상으로 두면 그만큼만 할당하고 그다음부터 실패한다 (할당 실패 주입) */
static BNode *tree_allocate_node(BTree *tree)
{
    if (tree->alloc_budget == 0)
        return NULL;
    if (tree->alloc_budget > 0)
        tree->alloc_budget--;
    return calloc(1, sizeof(BNode));
}

static bool pool_fill(BTree *tree, NodePool *pool, int count)
{
    for (int i = 0; i < count; i++) {
        BNode *node = tree_allocate_node(tree);
        if (node == NULL)
            return false;
        node->next = pool->head;
        pool->head = node;
    }
    return true;
}

/* 미리 할당한 노드를 하나 꺼낸다. pool_fill 이 필요한 만큼 채웠으므로 비어 있지 않다 */
static BNode *pool_take(NodePool *pool, bool is_leaf)
{
    BNode *node = pool->head;
    pool->head = node->next;
    node->next = NULL;
    node->is_leaf = is_leaf;
    return node;
}

static void pool_release(NodePool *pool)
{
    while (pool->head != NULL) {
        BNode *next = pool->head->next;
        free(pool->head);
        pool->head = next;
    }
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

/* ---- 노드 안 탐색 ---- */

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

/* 이 삽입에 필요한 새 노드 수. split 은 리프에서 시작해 꽉 찬 조상으로만 번지고, 자리가 있는
 * 첫 조상에서 멈춘다. 그래서 경로 끝에서부터 이어지는 꽉 찬 노드 수가 split 횟수이고,
 * 루트까지 전부 꽉 찼으면 새 루트가 하나 더 필요하다. */
static int tree_count_new_nodes(const BTree *tree, int32_t key)
{
    int full_run = 0;
    const BNode *node = tree->root;
    for (;;) {
        full_run = node->num_keys == BTREE_MAX_KEYS ? full_run + 1 : 0;
        if (node->is_leaf)
            break;
        node = node->children[node_child_index(node, key)];
    }
    return full_run == tree->height ? full_run + 1 : full_run;
}

/* ---- 리프 삽입과 split ---- */

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
 * 부모 키는 길 안내용 사본일 뿐이다. */
static void leaf_split(BNode *leaf, int position, int32_t key, uint64_t value, NodePool *pool,
                       SplitResult *out_split)
{
    BNode *right = pool_take(pool, true);

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
}

/* ---- 내부 노드 삽입과 split ---- */

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

/* 꽉 찬 node 에 자식 split 결과를 더한 상태 (키 ORDER 개, 자식 ORDER + 1 개) 를 임시 배열에 만든다 */
static void internal_build_overflow(const BNode *node, int child_index, const SplitResult *split,
                                    int32_t *out_keys, BNode **out_children)
{
    int tail_count = BTREE_MAX_KEYS - child_index;
    memcpy(out_keys, node->keys, child_index * sizeof(out_keys[0]));
    out_keys[child_index] = split->key;
    memcpy(&out_keys[child_index + 1], &node->keys[child_index], tail_count * sizeof(out_keys[0]));

    memcpy(out_children, node->children, (child_index + 1) * sizeof(out_children[0]));
    out_children[child_index + 1] = split->right;
    memcpy(&out_children[child_index + 2], &node->children[child_index + 1],
           tail_count * sizeof(out_children[0]));
}

/* 꽉 찬 내부 노드를 나눈다. 가운데 키는 부모로 "올리고" 양쪽 어디에도 남기지 않는다.
 * 내부 키는 데이터가 아니라 길 안내라서, 자식 ORDER + 1 개를 두 노드로 나누면 키는
 * (왼쪽 자식 수 - 1) + (오른쪽 자식 수 - 1) = ORDER - 1 개만 필요하고 정확히 하나가 남는다.
 * 그 하나가 두 노드를 가르는 구분 키로 부모에 올라간다. */
static void internal_split(BNode *node, int child_index, const SplitResult *child_split,
                           NodePool *pool, SplitResult *out_split)
{
    int32_t keys[BTREE_ORDER];
    BNode *children[BTREE_ORDER + 1];
    internal_build_overflow(node, child_index, child_split, keys, children);

    BNode *right = pool_take(pool, false);
    int right_key_count = BTREE_ORDER - INTERNAL_SPLIT_MIDDLE - 1;
    memcpy(node->keys, keys, INTERNAL_SPLIT_MIDDLE * sizeof(keys[0]));
    memcpy(node->children, children, (INTERNAL_SPLIT_MIDDLE + 1) * sizeof(children[0]));
    node->num_keys = INTERNAL_SPLIT_MIDDLE;
    memcpy(right->keys, &keys[INTERNAL_SPLIT_MIDDLE + 1], right_key_count * sizeof(keys[0]));
    memcpy(right->children, &children[INTERNAL_SPLIT_MIDDLE + 1],
           (right_key_count + 1) * sizeof(children[0]));
    right->num_keys = right_key_count;

    out_split->key = keys[INTERNAL_SPLIT_MIDDLE];
    out_split->right = right;
}

/* ---- 삽입 ---- */

/* node 아래에 (key, value) 를 넣는다. 재귀로 리프까지 내려갔다가 돌아오면서, 자식이 split 되었으면
 * 그 결과를 이 노드에 흡수하고, 이 노드도 꽉 찼으면 split 해서 out_split 으로 위에 넘긴다.
 * key 는 트리에 없어야 하고 pool 에는 필요한 노드가 다 있어야 한다 (btree_insert 가 보장). */
static void node_insert(BNode *node, int32_t key, uint64_t value, NodePool *pool,
                        SplitResult *out_split)
{
    out_split->right = NULL;
    if (node->is_leaf) {
        int position = node_lower_bound(node, key);
        if (node->num_keys < BTREE_MAX_KEYS)
            leaf_insert_at(node, position, key, value);
        else
            leaf_split(node, position, key, value, pool, out_split);
        return;
    }

    int child_index = node_child_index(node, key);
    SplitResult child_split;
    node_insert(node->children[child_index], key, value, pool, &child_split);
    if (child_split.right == NULL)
        return;
    if (node->num_keys < BTREE_MAX_KEYS)
        internal_insert_at(node, child_index, &child_split);
    else
        internal_split(node, child_index, &child_split, pool, out_split);
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
    tree->alloc_budget = BTREE_UNLIMITED_ALLOCS;
    tree->root = tree_allocate_node(tree);
    if (tree->root == NULL) {
        free(tree);
        return NULL;
    }
    tree->root->is_leaf = true;
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

    /* 필요한 노드를 전부 먼저 할당한다. 하나라도 실패하면 트리를 건드리기 전에 돌아간다 */
    NodePool pool = {NULL};
    if (!pool_fill(tree, &pool, tree_count_new_nodes(tree, key))) {
        pool_release(&pool);
        return BTREE_NO_MEMORY;
    }

    SplitResult split;
    node_insert(tree->root, key, value, &pool, &split);
    if (split.right != NULL)
        tree_grow_root(tree, pool_take(&pool, false), &split);
    pool_release(&pool); /* 계산이 맞으면 비어 있다 */
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

/* ---- 불변식 검사 ---- */

/* 한 서브트리의 키가 있어야 하는 구간 [low, high). has_* 가 false 면 그쪽은 열려 있다 */
typedef struct {
    bool has_low;
    int32_t low;
    bool has_high;
    int32_t high;
} KeyRange;

/* 트리를 왼쪽부터 깊이 우선으로 돌며 모으는 상태 */
typedef struct {
    int leaf_depth;              /* 처음 만난 리프의 깊이, 0 이면 아직 없음 */
    const BNode *previous_leaf;  /* 직전에 방문한 리프: 그 next 가 지금 리프여야 한다 */
    int64_t key_count;           /* 리프 키 개수 합 = size 여야 한다 */
} CheckState;

static bool node_has_valid_key_count(const BNode *node, bool is_root)
{
    if (node->num_keys > BTREE_MAX_KEYS)
        return false;
    if (!is_root)
        return node->num_keys >= MIN_KEYS_NON_ROOT;
    return node->is_leaf || node->num_keys >= 1; /* 내부 루트는 자식이 최소 2 개 */
}

/* 노드 안 키가 엄격한 오름차순이고 모두 range 안에 있는지 */
static bool node_keys_in_range(const BNode *node, KeyRange range)
{
    for (int i = 0; i < node->num_keys; i++) {
        int32_t key = node->keys[i];
        if (i > 0 && node->keys[i - 1] >= key)
            return false;
        if ((range.has_low && key < range.low) || (range.has_high && key >= range.high))
            return false;
    }
    return true;
}

/* index 번 자식의 키 구간: 왼쪽 구분 키 이상, 오른쪽 구분 키 미만 (양 끝은 부모 구간을 물려받음) */
static KeyRange child_key_range(const BNode *node, int index, KeyRange parent)
{
    KeyRange range = parent;
    if (index > 0) {
        range.has_low = true;
        range.low = node->keys[index - 1];
    }
    if (index < node->num_keys) {
        range.has_high = true;
        range.high = node->keys[index];
    }
    return range;
}

/* 리프는 모두 같은 깊이이고, 왼쪽부터 방문한 순서가 next 연결 순서와 같아야 한다 */
static bool check_leaf(const BNode *leaf, int depth, CheckState *state)
{
    if (state->leaf_depth == 0)
        state->leaf_depth = depth;
    if (depth != state->leaf_depth)
        return false;
    if (state->previous_leaf != NULL && state->previous_leaf->next != leaf)
        return false;
    state->previous_leaf = leaf;
    state->key_count += leaf->num_keys;
    return true;
}

static bool check_node(const BNode *node, int depth, KeyRange range, bool is_root,
                       CheckState *state)
{
    if (node == NULL || !node_has_valid_key_count(node, is_root) ||
        !node_keys_in_range(node, range))
        return false;
    if (node->is_leaf)
        return check_leaf(node, depth, state);
    /* 자식 수 = 키 수 + 1 은 children[0..num_keys] 를 모두 검사하는 것으로 확인된다 (NULL 이면 실패) */
    for (int i = 0; i <= node->num_keys; i++) {
        if (!check_node(node->children[i], depth + 1, child_key_range(node, i, range), false,
                        state))
            return false;
    }
    return true;
}

bool btree_check(const BTree *tree)
{
    KeyRange whole = {false, 0, false, 0};
    CheckState state = {0, NULL, 0};
    if (!check_node(tree->root, 1, whole, true, &state))
        return false;
    return state.previous_leaf->next == NULL && state.key_count == tree->size &&
           state.leaf_depth == tree->height;
}
