#include "test.h"

#include <stdint.h>

#include "btree.h"
#include "btree_internal.h"

#define TEST_RECORD_SIZE 40   /* users 테이블 레코드: 4 + 32 + 4 */
#define TEST_HEADER_SIZE 1024 /* .tbl 헤더 크기 */
#define SENTINEL_VALUE 0xDEADBEEFULL
#define SHUFFLED_COUNT 10

/* id 의 레코드 offset. 엔진 공식 (table.c) 과 같은 값을 테스트 안에서 직접 만든다 */
static uint64_t record_offset(int32_t id)
{
    return TEST_HEADER_SIZE + (uint64_t)(id - 1) * TEST_RECORD_SIZE;
}

static BTree *create_tree(void)
{
    BTree *tree = btree_create();
    CHECK(tree != NULL);
    return tree;
}

/* 모든 트리 테스트는 끝에서 불변식을 함께 검사한 뒤 해제한다 */
static void destroy_checked(BTree *tree)
{
    CHECK(btree_check(tree));
    btree_destroy(tree);
}

/* 루트 리프 안의 키가 엄격한 오름차순인지 */
static bool is_root_leaf_sorted(const BTree *tree)
{
    const BNode *root = tree->root;
    for (int i = 1; i < root->num_keys; i++) {
        if (root->keys[i - 1] >= root->keys[i])
            return false;
    }
    return true;
}

/* T20-01 */
static void test_create_empty_tree(void)
{
    BTree *tree = create_tree();
    CHECK(btree_size(tree) == 0);
    CHECK(btree_height(tree) == 1);
    CHECK(tree->root->is_leaf);
    CHECK(tree->root->num_keys == 0);
    destroy_checked(tree);
}

/* T20-02 */
static void test_insert_one_and_search(void)
{
    BTree *tree = create_tree();
    CHECK(btree_insert(tree, 7, 700) == BTREE_OK);
    uint64_t value = 0;
    CHECK(btree_search(tree, 7, &value));
    CHECK(value == 700);
    CHECK(btree_size(tree) == 1);
    destroy_checked(tree);
}

/* T20-03 */
static void test_insert_shuffled_keeps_leaf_sorted(void)
{
    const int32_t keys[SHUFFLED_COUNT] = {5, 1, 9, 3, 7, 10, 2, 8, 4, 6};
    BTree *tree = create_tree();
    for (int i = 0; i < SHUFFLED_COUNT; i++)
        CHECK(btree_insert(tree, keys[i], (uint64_t)keys[i] * 100) == BTREE_OK);

    for (int32_t key = 1; key <= SHUFFLED_COUNT; key++) {
        uint64_t value = 0;
        CHECK(btree_search(tree, key, &value));
        CHECK(value == (uint64_t)key * 100);
    }
    CHECK(btree_size(tree) == SHUFFLED_COUNT);
    CHECK(is_root_leaf_sorted(tree));
    destroy_checked(tree);
}

/* T20-04 */
static void test_value_is_record_offset(void)
{
    BTree *tree = create_tree();
    for (int32_t id = 1; id <= SHUFFLED_COUNT; id++)
        CHECK(btree_insert(tree, id, record_offset(id)) == BTREE_OK);
    for (int32_t id = 1; id <= SHUFFLED_COUNT; id++) {
        uint64_t value = 0;
        CHECK(btree_search(tree, id, &value));
        CHECK(value == record_offset(id));
    }
    destroy_checked(tree);
}

/* E20-01 */
static void test_search_empty_tree(void)
{
    BTree *tree = create_tree();
    uint64_t value = SENTINEL_VALUE;
    CHECK(!btree_search(tree, 1, &value));
    CHECK(value == SENTINEL_VALUE);
    destroy_checked(tree);
}

/* E20-02 */
static void test_search_missing_keys(void)
{
    BTree *tree = create_tree();
    CHECK(btree_insert(tree, 10, 1) == BTREE_OK);
    CHECK(btree_insert(tree, 20, 2) == BTREE_OK);
    CHECK(btree_insert(tree, 30, 3) == BTREE_OK);
    uint64_t value = SENTINEL_VALUE;
    CHECK(!btree_search(tree, 5, &value));  /* 가장 작은 키보다 작음 */
    CHECK(!btree_search(tree, 35, &value)); /* 가장 큰 키보다 큼 */
    CHECK(!btree_search(tree, 15, &value)); /* 사이 */
    CHECK(value == SENTINEL_VALUE);
    destroy_checked(tree);
}

/* E20-03 */
static void test_insert_duplicate_key(void)
{
    BTree *tree = create_tree();
    CHECK(btree_insert(tree, 3, 300) == BTREE_OK);
    CHECK(btree_insert(tree, 3, 999) == BTREE_DUPLICATE);
    CHECK(btree_size(tree) == 1);
    uint64_t value = 0;
    CHECK(btree_search(tree, 3, &value));
    CHECK(value == 300);
    destroy_checked(tree);
}

/* E20-04 */
static void test_extreme_keys(void)
{
    const int32_t keys[] = {INT32_MAX, 0, INT32_MIN, -1, -100, 1};
    const int key_count = (int)(sizeof(keys) / sizeof(keys[0]));
    BTree *tree = create_tree();
    for (int i = 0; i < key_count; i++)
        CHECK(btree_insert(tree, keys[i], (uint64_t)i) == BTREE_OK);
    for (int i = 0; i < key_count; i++) {
        uint64_t value = SENTINEL_VALUE;
        CHECK(btree_search(tree, keys[i], &value));
        CHECK(value == (uint64_t)i);
    }
    CHECK(is_root_leaf_sorted(tree));
    destroy_checked(tree);
}

/* E20-05 */
static void test_destroy_null(void)
{
    btree_destroy(NULL);
    CHECK(true);
}

/* E20-06 */
static void test_fill_leaf_without_split(void)
{
    BTree *tree = create_tree();
    /* 내림차순으로 넣어 매번 맨 앞 자리에 끼워 넣는 (가장 많이 미는) 경로를 밟는다 */
    for (int32_t key = BTREE_MAX_KEYS; key >= 1; key--)
        CHECK(btree_insert(tree, key, record_offset(key)) == BTREE_OK);
    CHECK(btree_size(tree) == BTREE_MAX_KEYS);
    CHECK(btree_height(tree) == 1);
    CHECK(tree->root->is_leaf);
    CHECK(is_root_leaf_sorted(tree));
    for (int32_t key = 1; key <= BTREE_MAX_KEYS; key++) {
        uint64_t value = 0;
        CHECK(btree_search(tree, key, &value));
        CHECK(value == record_offset(key));
    }
    destroy_checked(tree);
}

/* ---- S21 리프 split ---- */

/* 리프 split 때 왼쪽 리프에 남는 키 개수: ⌈(ORDER-1)/2⌉. 나머지 ORDER - 이 값은 오른쪽 */
#define SPLIT_LEFT_COUNT ((BTREE_MAX_KEYS + 1) / 2)
#define SPLIT_RIGHT_COUNT (BTREE_ORDER - SPLIT_LEFT_COUNT)
#define KEY_GAP 10 /* 키 사이에 빈자리를 두어 나중에 사이 키를 끼워 넣는다 */

static const BNode *leftmost_leaf(const BTree *tree)
{
    const BNode *node = tree->root;
    while (!node->is_leaf)
        node = node->children[0];
    return node;
}

static int count_leaves(const BTree *tree)
{
    int count = 0;
    for (const BNode *leaf = leftmost_leaf(tree); leaf != NULL; leaf = leaf->next)
        count++;
    return count;
}

/* 가장 왼쪽 리프부터 next 를 따라가며 키가 엄격한 오름차순이고 개수가 size 와 같은지 */
static bool is_leaf_chain_sorted(const BTree *tree)
{
    int64_t count = 0;
    bool has_previous = false;
    int32_t previous = 0;
    for (const BNode *leaf = leftmost_leaf(tree); leaf != NULL; leaf = leaf->next) {
        for (int i = 0; i < leaf->num_keys; i++) {
            if (has_previous && previous >= leaf->keys[i])
                return false;
            previous = leaf->keys[i];
            has_previous = true;
            count++;
        }
    }
    return count == btree_size(tree);
}

/* 첫 split 직후 모양: 루트는 키 1 개짜리 내부 노드, 루트 키 = 오른쪽 리프 첫 키 */
static void check_first_split_shape(const BTree *tree)
{
    const BNode *root = tree->root;
    CHECK(btree_height(tree) == 2);
    CHECK(!root->is_leaf);
    CHECK(root->num_keys == 1);
    CHECK(root->keys[0] == root->children[1]->keys[0]);
    CHECK(root->children[0]->next == root->children[1]);
    CHECK(root->children[1]->next == NULL);
    CHECK(is_leaf_chain_sorted(tree));
}

static void check_all_found(const BTree *tree, int32_t first, int32_t last, int32_t step)
{
    for (int32_t key = first; key <= last; key += step) {
        uint64_t value = SENTINEL_VALUE;
        CHECK(btree_search(tree, key, &value));
        CHECK(value == (uint64_t)key);
    }
}

/* 2, 4, ..., 2 × MAX_KEYS 로 리프를 꽉 채운다 (홀수 자리는 비어 있음) */
static BTree *create_full_even_leaf(void)
{
    BTree *tree = create_tree();
    for (int32_t i = 1; i <= BTREE_MAX_KEYS; i++)
        CHECK(btree_insert(tree, 2 * i, (uint64_t)(2 * i)) == BTREE_OK);
    CHECK(btree_height(tree) == 1);
    return tree;
}

/* T21-01 */
static void test_first_split_grows_height(void)
{
    BTree *tree = create_tree();
    for (int32_t key = 1; key <= BTREE_ORDER; key++)
        CHECK(btree_insert(tree, key, (uint64_t)key) == BTREE_OK);
    CHECK(btree_height(tree) == 2);
    CHECK(btree_size(tree) == BTREE_ORDER);
    check_all_found(tree, 1, BTREE_ORDER, 1);
    destroy_checked(tree);
}

/* T21-02 */
static void test_first_split_root_key(void)
{
    BTree *tree = create_tree();
    for (int32_t key = 1; key <= BTREE_ORDER; key++)
        CHECK(btree_insert(tree, key, (uint64_t)key) == BTREE_OK);
    check_first_split_shape(tree);
    CHECK(tree->root->keys[0] == SPLIT_LEFT_COUNT + 1);
    destroy_checked(tree);
}

/* T21-03 */
static void test_leaf_chain_is_sorted(void)
{
    BTree *tree = create_tree();
    /* 3 의 배수 보폭으로 뒤섞어 여러 리프에 흩뿌린다 (1 ~ 3×ORDER 를 한 번씩) */
    const int32_t key_count = 3 * BTREE_ORDER;
    for (int32_t i = 0; i < key_count; i++) {
        int32_t key = (int32_t)((i * 7) % key_count) + 1;
        CHECK(btree_insert(tree, key, (uint64_t)key) == BTREE_OK);
    }
    CHECK(count_leaves(tree) >= 2);
    CHECK(is_leaf_chain_sorted(tree));
    check_all_found(tree, 1, key_count, 1);
    destroy_checked(tree);
}

/* T21-04 */
static void test_split_key_counts(void)
{
    BTree *tree = create_tree();
    for (int32_t key = 1; key <= BTREE_ORDER; key++)
        CHECK(btree_insert(tree, key, (uint64_t)key) == BTREE_OK);
    CHECK(tree->root->children[0]->num_keys == SPLIT_LEFT_COUNT);
    CHECK(tree->root->children[1]->num_keys == SPLIT_RIGHT_COUNT);
    destroy_checked(tree);
}

/* E21-01 */
static void test_split_new_key_in_left_half(void)
{
    BTree *tree = create_full_even_leaf();
    CHECK(btree_insert(tree, 3, 3) == BTREE_OK); /* 2 와 4 사이: 왼쪽 절반 */
    check_first_split_shape(tree);
    /* ORDER = 3 이면 왼쪽에 키가 1 개만 남아 3 은 오른쪽 맨 앞으로 간다 (왼쪽 절반 삽입이 성립 안 함) */
    if (SPLIT_LEFT_COUNT >= 2)
        CHECK(tree->root->children[0]->keys[1] == 3);
    CHECK(tree->root->children[0]->num_keys == SPLIT_LEFT_COUNT);
    check_all_found(tree, 2, 2 * BTREE_MAX_KEYS, 2);
    destroy_checked(tree);
}

/* E21-02 */
static void test_split_new_key_at_right_front(void)
{
    BTree *tree = create_full_even_leaf();
    /* 합친 배열의 SPLIT_LEFT_COUNT 번째 자리 = 오른쪽 리프의 맨 앞 */
    const int32_t new_key = 2 * SPLIT_LEFT_COUNT + 1;
    CHECK(btree_insert(tree, new_key, (uint64_t)new_key) == BTREE_OK);
    check_first_split_shape(tree);
    CHECK(tree->root->keys[0] == new_key);
    CHECK(tree->root->children[1]->keys[0] == new_key);
    destroy_checked(tree);
}

/* E21-03 */
static void test_split_ascending_leaves_left_half_full(void)
{
    BTree *tree = create_tree();
    for (int32_t key = 1; key <= BTREE_ORDER; key++)
        CHECK(btree_insert(tree, key, (uint64_t)key) == BTREE_OK);
    check_first_split_shape(tree);
    const BNode *left = tree->root->children[0];
    const BNode *right = tree->root->children[1];
    CHECK(left->num_keys == SPLIT_LEFT_COUNT); /* 꽉 차지 않고 반만 참 */
    CHECK(left->keys[left->num_keys - 1] == SPLIT_LEFT_COUNT);
    CHECK(right->keys[right->num_keys - 1] == BTREE_ORDER);
    destroy_checked(tree);
}

/* E21-04 */
static void test_split_descending(void)
{
    BTree *tree = create_tree();
    for (int32_t key = BTREE_ORDER; key >= 1; key--)
        CHECK(btree_insert(tree, key, (uint64_t)key) == BTREE_OK);
    check_first_split_shape(tree);
    CHECK(leftmost_leaf(tree)->keys[0] == 1);
    check_all_found(tree, 1, BTREE_ORDER, 1);
    destroy_checked(tree);
}

/* E21-05 */
static void test_split_middle_leaf_keeps_chain(void)
{
    BTree *tree = create_tree();
    /* 오름차순으로 리프 3 개 (왼쪽, 가운데, 오른쪽) 가 될 때까지 넣는다 */
    int32_t last_key = 0;
    while (count_leaves(tree) < 3) {
        last_key += KEY_GAP;
        CHECK(btree_insert(tree, last_key, (uint64_t)last_key) == BTREE_OK);
    }
    const BNode *middle = leftmost_leaf(tree)->next;
    const BNode *right = middle->next;
    int32_t middle_first = middle->keys[0];

    /* 가운데 리프의 키 사이 빈자리를 채워 가운데 리프만 split 시킨다. 빈자리마다 두 개씩
     * (끝자리 1, 6) 넣어야 ORDER 가 홀수여서 split 에 키가 하나 더 필요할 때도 오른쪽 리프로 넘치지 않는다 */
    for (int32_t key = middle_first + 1; count_leaves(tree) < 4; key += KEY_GAP / 2)
        CHECK(btree_insert(tree, key, (uint64_t)key) == BTREE_OK);

    CHECK(btree_height(tree) == 2);
    CHECK(middle->next != right); /* 새 리프가 가운데와 오른쪽 사이에 들어감 */
    CHECK(middle->next->next == right);
    CHECK(right->next == NULL);
    CHECK(is_leaf_chain_sorted(tree));
    check_all_found(tree, KEY_GAP, last_key, KEY_GAP);
    destroy_checked(tree);
}

/* ---- S22 내부 노드 split, 루트 성장, 불변식 검사 ---- */

#define CASCADE_SPLIT_NODE_COUNT 3 /* 리프 + 내부 루트 split + 새 루트 */

/* 오름차순으로 target_height 가 될 때까지 넣고 마지막 키를 돌려준다.
 * 한 번에 높이가 1 넘게 늘면 실패 (E22-01) */
static int32_t insert_ascending_until_height(BTree *tree, int target_height)
{
    int32_t key = (int32_t)btree_size(tree);
    while (btree_height(tree) < target_height) {
        int height_before = btree_height(tree);
        key++;
        CHECK(btree_insert(tree, key, (uint64_t)key) == BTREE_OK);
        CHECK(btree_height(tree) - height_before <= 1);
    }
    return key;
}

static bool node_contains_key(const BNode *node, int32_t key)
{
    for (int i = 0; i < node->num_keys; i++) {
        if (node->keys[i] == key)
            return true;
    }
    return false;
}

/* T22-01 */
static void test_grow_to_height_three(void)
{
    BTree *tree = create_tree();
    insert_ascending_until_height(tree, 3);
    CHECK(btree_height(tree) == 3);
    CHECK(btree_check(tree));
    destroy_checked(tree);
}

/* T22-02 */
static void test_internal_split_moves_middle_key_up(void)
{
    BTree *tree = create_tree();
    insert_ascending_until_height(tree, 3);
    const BNode *root = tree->root;
    CHECK(root->num_keys == 1);
    int32_t up_key = root->keys[0];
    CHECK(!root->children[0]->is_leaf);
    CHECK(!node_contains_key(root->children[0], up_key)); /* 올라간 키는 양쪽에 없음 */
    CHECK(!node_contains_key(root->children[1], up_key));
    uint64_t value = 0;
    CHECK(btree_search(tree, up_key, &value)); /* 데이터는 리프에 그대로 */
    CHECK(value == (uint64_t)up_key);
    destroy_checked(tree);
}

/* T22-03 */
static void test_height_three_all_found(void)
{
    BTree *tree = create_tree();
    int32_t last_key = insert_ascending_until_height(tree, 3);
    check_all_found(tree, 1, last_key, 1);
    destroy_checked(tree);
}

/* T22-04 */
static void test_check_detects_broken_tree(void)
{
    BTree *tree = create_tree();
    for (int32_t key = 1; key <= SHUFFLED_COUNT; key++)
        CHECK(btree_insert(tree, key, (uint64_t)key) == BTREE_OK);
    CHECK(btree_check(tree));

    /* 테스트 전용 조작: 리프 안의 두 키 순서를 바꾼다 */
    BNode *leaf = tree->root;
    while (!leaf->is_leaf)
        leaf = leaf->children[0];
    int32_t saved = leaf->keys[0];
    leaf->keys[0] = leaf->keys[1];
    leaf->keys[1] = saved;
    CHECK(!btree_check(tree));

    leaf->keys[1] = leaf->keys[0]; /* 원래대로 돌린다 */
    leaf->keys[0] = saved;
    destroy_checked(tree);
}

/* E22-01 */
static void test_cascading_root_split(void)
{
    BTree *tree = create_tree();
    insert_ascending_until_height(tree, 2);
    /* 높이 2 → 3 이 되는 삽입은 리프 split 이 내부 루트 split 으로 번지는 연쇄 split 이다 */
    int leaves_before = 0;
    while (btree_height(tree) == 2) {
        leaves_before = count_leaves(tree);
        int32_t key = (int32_t)btree_size(tree) + 1;
        CHECK(btree_insert(tree, key, (uint64_t)key) == BTREE_OK);
    }
    CHECK(btree_height(tree) == 3);
    CHECK(count_leaves(tree) == leaves_before + 1);
    destroy_checked(tree);
}

/* E22-02 */
static void test_internal_split_on_leftmost_path(void)
{
    BTree *tree = create_tree();
    int32_t key = INT32_MAX; /* 내림차순: 늘 가장 왼쪽 경로에 삽입 */
    while (btree_height(tree) < 3) {
        CHECK(btree_insert(tree, key, (uint64_t)key) == BTREE_OK);
        key--;
    }
    CHECK(btree_check(tree));
    CHECK(leftmost_leaf(tree)->keys[0] == key + 1);
    /* INT32_MAX 까지 포함하려고 int64_t 로 돈다 (int32_t 면 ++ 에서 넘침) */
    for (int64_t found = (int64_t)key + 1; found <= INT32_MAX; found++) {
        uint64_t value = 0;
        CHECK(btree_search(tree, (int32_t)found, &value));
        CHECK(value == (uint64_t)found);
    }
    destroy_checked(tree);
}

/* E22-03 */
static void test_internal_split_on_rightmost_path(void)
{
    BTree *tree = create_tree();
    int32_t last_key = insert_ascending_until_height(tree, 3);
    const BNode *node = tree->root;
    while (!node->is_leaf)
        node = node->children[node->num_keys];
    CHECK(node->keys[node->num_keys - 1] == last_key); /* 가장 오른쪽 리프에 마지막 키 */
    CHECK(node->next == NULL);
    check_all_found(tree, 1, last_key, 1);
    destroy_checked(tree);
}

/* 다음 오름차순 삽입이 리프 → 내부 루트 → 새 루트 로 연쇄 split 되는 직전 상태를 만든다 */
static BTree *create_tree_before_cascade(void)
{
    BTree *tree = create_tree();
    insert_ascending_until_height(tree, 2);
    for (;;) {
        const BNode *root = tree->root;
        const BNode *rightmost = root->children[root->num_keys];
        if (root->num_keys == BTREE_MAX_KEYS && rightmost->num_keys == BTREE_MAX_KEYS)
            return tree;
        int32_t key = (int32_t)btree_size(tree) + 1;
        CHECK(btree_insert(tree, key, (uint64_t)key) == BTREE_OK);
        CHECK(btree_height(tree) == 2);
    }
}

/* E22-04 */
static void test_alloc_failure_keeps_tree(void)
{
    for (int budget = 0; budget < CASCADE_SPLIT_NODE_COUNT; budget++) {
        BTree *tree = create_tree_before_cascade();
        int64_t size_before = btree_size(tree);
        int32_t key = (int32_t)size_before + 1;

        tree->alloc_budget = budget; /* budget 번째 다음 할당부터 실패 */
        CHECK(btree_insert(tree, key, (uint64_t)key) == BTREE_NO_MEMORY);
        CHECK(btree_size(tree) == size_before);
        CHECK(btree_height(tree) == 2);
        uint64_t value = SENTINEL_VALUE;
        CHECK(!btree_search(tree, key, &value));
        CHECK(btree_check(tree));
        check_all_found(tree, 1, (int32_t)size_before, 1);

        tree->alloc_budget = BTREE_UNLIMITED_ALLOCS; /* 할당이 되면 같은 삽입이 성공 */
        CHECK(btree_insert(tree, key, (uint64_t)key) == BTREE_OK);
        CHECK(btree_height(tree) == 3);
        destroy_checked(tree);
    }
}

/* ---- S23 범위 순회 ---- */

#define RANGE_KEY_COUNT 1000
#define RANGE_TAKE_COUNT 10
#define BOUNDARY_BACKOFF 2 /* 리프 마지막 키보다 이만큼 앞에서 시작해 경계를 넘는다 */

/* first, first + step, ... 을 count 개 넣은 트리 (값 = 키) */
static BTree *create_range_tree(int32_t first, int32_t step, int count)
{
    BTree *tree = create_tree();
    for (int i = 0; i < count; i++) {
        int32_t key = first + i * step;
        CHECK(btree_insert(tree, key, (uint64_t)key) == BTREE_OK);
    }
    return tree;
}

/* 반복자에서 count 개를 꺼내 first, first + step, ... 인지 확인한다 (값도 키와 같아야 함) */
static void check_iter_sequence(BTreeIter *iter, int32_t first, int32_t step, int count)
{
    for (int i = 0; i < count; i++) {
        int32_t key = 0;
        uint64_t value = SENTINEL_VALUE;
        CHECK(btree_iter_next(iter, &key, &value));
        CHECK(key == first + i * step);
        CHECK(value == (uint64_t)key);
    }
}

static void check_iter_exhausted(BTreeIter *iter)
{
    int32_t key = 0;
    uint64_t value = 0;
    CHECK(!btree_iter_next(iter, &key, &value));
}

/* T23-01 */
static void test_seek_then_take_ten(void)
{
    BTree *tree = create_range_tree(1, 1, RANGE_KEY_COUNT);
    BTreeIter iter = btree_seek(tree, 500);
    check_iter_sequence(&iter, 500, 1, RANGE_TAKE_COUNT);
    destroy_checked(tree);
}

/* T23-02 */
static void test_seek_first_visits_all(void)
{
    BTree *tree = create_range_tree(1, 1, RANGE_KEY_COUNT);
    BTreeIter iter = btree_seek_first(tree);
    check_iter_sequence(&iter, 1, 1, RANGE_KEY_COUNT);
    check_iter_exhausted(&iter);
    destroy_checked(tree);
}

/* T23-03 */
static void test_iterate_across_leaf_boundary(void)
{
    BTree *tree = create_range_tree(1, 1, RANGE_KEY_COUNT);
    const BNode *first_leaf = leftmost_leaf(tree);
    CHECK(first_leaf->next != NULL); /* 리프가 여러 개여야 경계를 넘는다 */
    /* 첫 리프 끝 근처에서 시작한다. 작은 차수면 리프가 짧으니 첫 리프 안으로 제한한다 */
    int32_t start = first_leaf->keys[first_leaf->num_keys - 1] - BOUNDARY_BACKOFF;
    if (start < first_leaf->keys[0])
        start = first_leaf->keys[0];
    BTreeIter iter = btree_seek(tree, start);
    check_iter_sequence(&iter, start, 1, RANGE_TAKE_COUNT); /* 빠짐·중복이 있으면 순서가 어긋남 */
    destroy_checked(tree);
}

/* T23-04 */
static void test_seek_between_keys(void)
{
    BTree *tree = create_range_tree(2, 2, RANGE_KEY_COUNT / 2); /* 2, 4, ..., 1000 */
    BTreeIter iter = btree_seek(tree, 5);
    check_iter_sequence(&iter, 6, 2, RANGE_TAKE_COUNT);
    destroy_checked(tree);
}

/* E23-01 */
static void test_seek_empty_tree(void)
{
    BTree *tree = create_tree();
    BTreeIter iter = btree_seek(tree, 1);
    check_iter_exhausted(&iter);
    BTreeIter first = btree_seek_first(tree);
    check_iter_exhausted(&first);
    destroy_checked(tree);
}

/* E23-02 */
static void test_seek_past_last_key(void)
{
    BTree *tree = create_range_tree(1, 1, RANGE_KEY_COUNT);
    BTreeIter iter = btree_seek(tree, RANGE_KEY_COUNT + 1);
    check_iter_exhausted(&iter);
    destroy_checked(tree);
}

/* E23-03 */
static void test_seek_before_first_key(void)
{
    BTree *tree = create_range_tree(100, 1, RANGE_KEY_COUNT);
    BTreeIter iter = btree_seek(tree, -5);
    check_iter_sequence(&iter, 100, 1, RANGE_TAKE_COUNT);
    destroy_checked(tree);
}

/* E23-04 */
static void test_iter_next_after_end(void)
{
    BTree *tree = create_range_tree(1, 1, RANGE_KEY_COUNT);
    BTreeIter iter = btree_seek(tree, RANGE_KEY_COUNT);
    check_iter_sequence(&iter, RANGE_KEY_COUNT, 1, 1);
    for (int i = 0; i < RANGE_TAKE_COUNT; i++)
        check_iter_exhausted(&iter);
    destroy_checked(tree);
}

/* E23-05 */
static void test_seek_extreme_bounds(void)
{
    BTree *tree = create_range_tree(1, 1, RANGE_KEY_COUNT);
    BTreeIter from_min = btree_seek(tree, INT32_MIN);
    check_iter_sequence(&from_min, 1, 1, RANGE_TAKE_COUNT);
    BTreeIter absent_max = btree_seek(tree, INT32_MAX); /* INT32_MAX 가 없으면 빈 결과 */
    check_iter_exhausted(&absent_max);

    CHECK(btree_insert(tree, INT32_MAX, (uint64_t)INT32_MAX) == BTREE_OK);
    BTreeIter present_max = btree_seek(tree, INT32_MAX); /* 있으면 정확히 하나 */
    check_iter_sequence(&present_max, INT32_MAX, 1, 1);
    check_iter_exhausted(&present_max);
    destroy_checked(tree);
}

/* E23-06 */
static void test_seek_leaf_first_and_last_key(void)
{
    BTree *tree = create_range_tree(1, 1, RANGE_KEY_COUNT);
    const BNode *middle = leftmost_leaf(tree)->next;
    CHECK(middle != NULL && middle->next != NULL);
    int32_t first = middle->keys[0];
    int32_t last = middle->keys[middle->num_keys - 1];

    BTreeIter from_first = btree_seek(tree, first);
    check_iter_sequence(&from_first, first, 1, RANGE_TAKE_COUNT);
    BTreeIter from_last = btree_seek(tree, last); /* 마지막 키 다음은 오른쪽 리프의 첫 키 */
    check_iter_sequence(&from_last, last, 1, RANGE_TAKE_COUNT);
    destroy_checked(tree);
}

int main(void)
{
    printf("test_btree\n");
    RUN(test_create_empty_tree);
    RUN(test_insert_one_and_search);
    RUN(test_insert_shuffled_keeps_leaf_sorted);
    RUN(test_value_is_record_offset);
    RUN(test_search_empty_tree);
    RUN(test_search_missing_keys);
    RUN(test_insert_duplicate_key);
    RUN(test_extreme_keys);
    RUN(test_destroy_null);
    RUN(test_fill_leaf_without_split);
    RUN(test_first_split_grows_height);
    RUN(test_first_split_root_key);
    RUN(test_leaf_chain_is_sorted);
    RUN(test_split_key_counts);
    RUN(test_split_new_key_in_left_half);
    RUN(test_split_new_key_at_right_front);
    RUN(test_split_ascending_leaves_left_half_full);
    RUN(test_split_descending);
    RUN(test_split_middle_leaf_keeps_chain);
    RUN(test_grow_to_height_three);
    RUN(test_internal_split_moves_middle_key_up);
    RUN(test_height_three_all_found);
    RUN(test_check_detects_broken_tree);
    RUN(test_cascading_root_split);
    RUN(test_internal_split_on_leftmost_path);
    RUN(test_internal_split_on_rightmost_path);
    RUN(test_alloc_failure_keeps_tree);
    RUN(test_seek_then_take_ten);
    RUN(test_seek_first_visits_all);
    RUN(test_iterate_across_leaf_boundary);
    RUN(test_seek_between_keys);
    RUN(test_seek_empty_tree);
    RUN(test_seek_past_last_key);
    RUN(test_seek_before_first_key);
    RUN(test_iter_next_after_end);
    RUN(test_seek_extreme_bounds);
    RUN(test_seek_leaf_first_and_last_key);
    TEST_DONE();
    return 0;
}
