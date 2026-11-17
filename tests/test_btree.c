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
    btree_destroy(tree);
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
    btree_destroy(tree);
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
    btree_destroy(tree);
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
    btree_destroy(tree);
}

/* E20-01 */
static void test_search_empty_tree(void)
{
    BTree *tree = create_tree();
    uint64_t value = SENTINEL_VALUE;
    CHECK(!btree_search(tree, 1, &value));
    CHECK(value == SENTINEL_VALUE);
    btree_destroy(tree);
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
    btree_destroy(tree);
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
    btree_destroy(tree);
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
    btree_destroy(tree);
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
    btree_destroy(tree);
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
    TEST_DONE();
    return 0;
}
