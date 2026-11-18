#include "bptree.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 리프의 값과 내부 노드의 자식 포인터는 같은 자리를 쓴다 */
union bptree_slot {
    struct bptree_node *child;
    uint64_t value;
};

/* 키는 max_keys + 1 칸, 슬롯은 max_keys + 2 칸을 잡는다. 넣은 뒤 넘친 상태를 잠깐 담고 나누기 위해서다 */
struct bptree_node {
    bool is_leaf;
    int num_keys;
    struct bptree_node *next; /* 리프만: 오른쪽 이웃 */
    long long *keys;
    union bptree_slot *slots;
};

/* 노드 하나가 나뉘면 부모에 넣을 안내 키와 새 오른쪽 노드 */
struct split {
    long long key;
    struct bptree_node *right;
};

enum insert_status {
    INSERT_DONE,
    INSERT_SPLIT,
    INSERT_DUPLICATE
};

static struct bptree_node *alloc_node(int max_keys)
{
    size_t keys_size = (size_t)(max_keys + 1) * sizeof(long long);
    size_t slots_size = (size_t)(max_keys + 2) * sizeof(union bptree_slot);
    struct bptree_node *n = malloc(sizeof(*n) + keys_size + slots_size);

    if (!n) {
        return NULL;
    }
    n->keys = (long long *)(n + 1);
    n->slots = (union bptree_slot *)((char *)n->keys + keys_size);
    n->is_leaf = true;
    n->num_keys = 0;
    n->next = NULL;
    return n;
}

int bptree_init(struct bptree *t, int order)
{
    memset(t, 0, sizeof(*t));
    if (order < BPTREE_MIN_ORDER) {
        return -1;
    }
    t->order = order;
    t->max_keys = order - 1;
    t->is_append_split = true;
    t->height = 1;
    t->root = alloc_node(t->max_keys);
    return t->root ? 0 : -1;
}

/* ---- 예비 노드 ----------------------------------------------------------------------------- */

/* 한 번 넣을 때 최악은 모든 층이 나뉘고 새 루트까지 생기는 것: 높이 + 1 개.
 * 미리 확보해 두면 넣는 도중에 메모리 부족으로 실패해 트리가 반쯤 바뀐 채 남는 일이 없다 */
static int ensure_spares(struct bptree *t)
{
    int need = t->height + 1;

    if (t->spare_cap < need) {
        int cap = need * 2;
        struct bptree_node **grown = realloc(t->spares, (size_t)cap * sizeof(*grown));
        if (!grown) {
            return -1;
        }
        t->spares = grown;
        t->spare_cap = cap;
    }
    while (t->spare_count < need) {
        struct bptree_node *n = alloc_node(t->max_keys);
        if (!n) {
            return -1;
        }
        t->spares[t->spare_count++] = n;
    }
    return 0;
}

static struct bptree_node *take_spare(struct bptree *t)
{
    struct bptree_node *n = t->spares[--t->spare_count];

    n->num_keys = 0;
    n->next = NULL;
    return n;
}

/* ---- 검색 보조 ----------------------------------------------------------------------------- */

/* keys[i] >= key 인 첫 위치 */
static int lower_bound(const long long *keys, int n, long long key)
{
    int lo = 0;
    int hi = n;

    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (keys[mid] < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* keys[i] > key 인 첫 위치. 내부 노드에서 내려갈 자식 번호다.
 * 안내 키는 "오른쪽 서브트리의 가장 작은 키"라서 같으면 오른쪽으로 간다 */
static int upper_bound(const long long *keys, int n, long long key)
{
    int lo = 0;
    int hi = n;

    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (keys[mid] <= key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static const struct bptree_node *find_leaf(const struct bptree *t, long long key)
{
    const struct bptree_node *n = t->root;

    while (!n->is_leaf) {
        n = n->slots[upper_bound(n->keys, n->num_keys, key)].child;
    }
    return n;
}

/* ---- 삽입 ---------------------------------------------------------------------------------- */

static enum insert_status insert_into_leaf(struct bptree *t, struct bptree_node *leaf,
                                           long long key, uint64_t value, bool is_rightmost,
                                           struct split *out)
{
    int pos = lower_bound(leaf->keys, leaf->num_keys, key);

    if (pos < leaf->num_keys && leaf->keys[pos] == key) {
        return INSERT_DUPLICATE;
    }
    memmove(&leaf->keys[pos + 1], &leaf->keys[pos], (size_t)(leaf->num_keys - pos) * sizeof(long long));
    memmove(&leaf->slots[pos + 1], &leaf->slots[pos],
            (size_t)(leaf->num_keys - pos) * sizeof(union bptree_slot));
    leaf->keys[pos] = key;
    leaf->slots[pos].value = value;
    leaf->num_keys++;
    if (leaf->num_keys <= t->max_keys) {
        return INSERT_DONE;
    }

    /* 넘침. 맨 오른쪽 리프 끝에 붙인 경우면 왼쪽을 꽉 채우고 새 키 하나만 오른쪽으로 */
    int n = leaf->num_keys;
    bool is_append = t->is_append_split && is_rightmost && pos == n - 1;
    int left_n = is_append ? n - 1 : n / 2;
    struct bptree_node *right = take_spare(t);

    right->is_leaf = true;
    right->num_keys = n - left_n;
    memcpy(right->keys, &leaf->keys[left_n], (size_t)right->num_keys * sizeof(long long));
    memcpy(right->slots, &leaf->slots[left_n], (size_t)right->num_keys * sizeof(union bptree_slot));
    leaf->num_keys = left_n;
    right->next = leaf->next;
    leaf->next = right;

    out->key = right->keys[0]; /* 리프 분할은 키를 복사해 올린다 */
    out->right = right;
    return INSERT_SPLIT;
}

static enum insert_status insert_rec(struct bptree *t, struct bptree_node *node, long long key,
                                     uint64_t value, bool is_rightmost, struct split *out)
{
    if (node->is_leaf) {
        return insert_into_leaf(t, node, key, value, is_rightmost, out);
    }

    int idx = upper_bound(node->keys, node->num_keys, key);
    struct split child_split;
    enum insert_status st = insert_rec(t, node->slots[idx].child, key, value,
                                       is_rightmost && idx == node->num_keys, &child_split);
    if (st != INSERT_SPLIT) {
        return st;
    }

    /* 자식이 나뉘었으면 안내 키를 idx 에, 새 오른쪽 자식을 idx + 1 에 끼운다 */
    memmove(&node->keys[idx + 1], &node->keys[idx], (size_t)(node->num_keys - idx) * sizeof(long long));
    memmove(&node->slots[idx + 2], &node->slots[idx + 1],
            (size_t)(node->num_keys - idx) * sizeof(union bptree_slot));
    node->keys[idx] = child_split.key;
    node->slots[idx + 1].child = child_split.right;
    node->num_keys++;
    if (node->num_keys <= t->max_keys) {
        return INSERT_DONE;
    }

    /* 내부 노드 넘침: 가운데 키는 위로 "옮기고" 양쪽에 남기지 않는다 */
    int k = node->num_keys;
    bool is_append = t->is_append_split && is_rightmost && idx == k - 1;
    int mid = is_append ? k - 2 : k / 2;
    struct bptree_node *right = take_spare(t);

    right->is_leaf = false;
    right->num_keys = k - mid - 1;
    memcpy(right->keys, &node->keys[mid + 1], (size_t)right->num_keys * sizeof(long long));
    memcpy(right->slots, &node->slots[mid + 1], (size_t)(right->num_keys + 1) * sizeof(union bptree_slot));
    node->num_keys = mid;

    out->key = node->keys[mid];
    out->right = right;
    return INSERT_SPLIT;
}

enum bptree_result bptree_insert(struct bptree *t, long long key, uint64_t value)
{
    struct split sp;

    if (ensure_spares(t) < 0) {
        return BPTREE_NO_MEMORY;
    }
    enum insert_status st = insert_rec(t, t->root, key, value, true, &sp);
    if (st == INSERT_DUPLICATE) {
        return BPTREE_DUPLICATE;
    }
    if (st == INSERT_SPLIT) {
        /* 루트가 나뉘면 새 루트. 높이는 이때만 1 늘어난다 */
        struct bptree_node *root = take_spare(t);
        root->is_leaf = false;
        root->num_keys = 1;
        root->keys[0] = sp.key;
        root->slots[0].child = t->root;
        root->slots[1].child = sp.right;
        t->root = root;
        t->height++;
    }
    t->count++;
    return BPTREE_OK;
}

/* ---- 조회 ---------------------------------------------------------------------------------- */

bool bptree_find(const struct bptree *t, long long key, uint64_t *value)
{
    const struct bptree_node *leaf = find_leaf(t, key);
    int pos = lower_bound(leaf->keys, leaf->num_keys, key);

    if (pos < leaf->num_keys && leaf->keys[pos] == key) {
        *value = leaf->slots[pos].value;
        return true;
    }
    return false;
}

size_t bptree_range(const struct bptree *t, long long low, long long high, bptree_range_fn fn,
                    void *ctx)
{
    size_t visited = 0;

    if (low > high) {
        return 0;
    }
    /* ① low 가 있을 리프까지 내려간다 O(log N) → ② 리프 연결을 따라 옆으로 읽는다 O(K) */
    const struct bptree_node *leaf = find_leaf(t, low);
    int pos = lower_bound(leaf->keys, leaf->num_keys, low);
    while (leaf) {
        for (; pos < leaf->num_keys; pos++) {
            if (leaf->keys[pos] > high) {
                return visited;
            }
            visited++;
            if (fn(leaf->keys[pos], leaf->slots[pos].value, ctx) != 0) {
                return visited;
            }
        }
        leaf = leaf->next;
        pos = 0;
    }
    return visited;
}

bool bptree_max_key(const struct bptree *t, long long *key)
{
    const struct bptree_node *n = t->root;

    while (!n->is_leaf) {
        n = n->slots[n->num_keys].child;
    }
    if (n->num_keys == 0) {
        return false;
    }
    *key = n->keys[n->num_keys - 1];
    return true;
}

static void stats_rec(const struct bptree_node *n, struct bptree_stats *out, size_t *leaf_keys)
{
    if (n->is_leaf) {
        out->leaf_count++;
        *leaf_keys += (size_t)n->num_keys;
        return;
    }
    out->internal_count++;
    for (int i = 0; i <= n->num_keys; i++) {
        stats_rec(n->slots[i].child, out, leaf_keys);
    }
}

void bptree_stats(const struct bptree *t, struct bptree_stats *out)
{
    size_t leaf_keys = 0;

    memset(out, 0, sizeof(*out));
    stats_rec(t->root, out, &leaf_keys);
    out->leaf_fill = (double)leaf_keys / ((double)out->leaf_count * t->max_keys);
}

/* ---- 해제 ---------------------------------------------------------------------------------- */

static void free_rec(struct bptree_node *n)
{
    if (!n->is_leaf) {
        for (int i = 0; i <= n->num_keys; i++) {
            free_rec(n->slots[i].child);
        }
    }
    free(n);
}

void bptree_free(struct bptree *t)
{
    if (t->root) {
        free_rec(t->root);
    }
    for (int i = 0; i < t->spare_count; i++) {
        free(t->spares[i]);
    }
    free(t->spares);
    memset(t, 0, sizeof(*t));
}

/* ---- 불변식 검사 --------------------------------------------------------------------------- */

struct check_ctx {
    const struct bptree *t;
    const struct bptree_node *prev_leaf;
    size_t leaf_keys;
    char *msg;
    size_t msg_size;
};

static int check_fail(struct check_ctx *c, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(c->msg, c->msg_size, fmt, ap);
    va_end(ap);
    return -1;
}

/* 이 노드의 키는 모두 [lo, hi) 안에 있어야 한다 (has_lo / has_hi 가 거짓이면 그쪽은 제한 없음) */
static int check_rec(struct check_ctx *c, const struct bptree_node *n, int depth, bool has_lo,
                     long long lo, bool has_hi, long long hi, bool is_root, bool is_rightmost)
{
    const struct bptree *t = c->t;

    if (n->num_keys > t->max_keys) {
        return check_fail(c, "depth %d: %d keys > max %d", depth, n->num_keys, t->max_keys);
    }
    int min_keys = n->is_leaf ? (t->max_keys + 1) / 2 : t->max_keys / 2;
    if (is_root) {
        min_keys = n->is_leaf ? 0 : 1;
    } else if (is_rightmost) {
        min_keys = 1; /* append split 은 맨 오른쪽에 키 하나짜리 노드를 남길 수 있다 */
    }
    if (n->num_keys < min_keys) {
        return check_fail(c, "depth %d: %d keys < min %d", depth, n->num_keys, min_keys);
    }
    for (int i = 0; i < n->num_keys; i++) {
        long long k = n->keys[i];
        if (i > 0 && n->keys[i - 1] >= k) {
            return check_fail(c, "depth %d: keys not strictly increasing at %d", depth, i);
        }
        if ((has_lo && k < lo) || (has_hi && k >= hi)) {
            return check_fail(c, "depth %d: key %lld outside parent range", depth, k);
        }
    }

    if (n->is_leaf) {
        if (depth != t->height) {
            return check_fail(c, "leaf at depth %d, height is %d", depth, t->height);
        }
        if (c->prev_leaf && c->prev_leaf->next != n) {
            return check_fail(c, "leaf chain broken before key %lld",
                              n->num_keys ? n->keys[0] : 0);
        }
        c->prev_leaf = n;
        c->leaf_keys += (size_t)n->num_keys;
        return 0;
    }

    for (int i = 0; i <= n->num_keys; i++) {
        bool child_has_lo = i > 0 || has_lo;
        long long child_lo = i > 0 ? n->keys[i - 1] : lo;
        bool child_has_hi = i < n->num_keys || has_hi;
        long long child_hi = i < n->num_keys ? n->keys[i] : hi;
        if (check_rec(c, n->slots[i].child, depth + 1, child_has_lo, child_lo, child_has_hi,
                      child_hi, false, is_rightmost && i == n->num_keys) < 0) {
            return -1;
        }
    }
    return 0;
}

int bptree_check(const struct bptree *t, char *msg, size_t msg_size)
{
    struct check_ctx c = {.t = t, .msg = msg, .msg_size = msg_size};

    msg[0] = '\0';
    if (check_rec(&c, t->root, 1, false, 0, false, 0, true, true) < 0) {
        return -1;
    }
    if (c.prev_leaf && c.prev_leaf->next != NULL) {
        return check_fail(&c, "last leaf has a next pointer");
    }
    if (c.leaf_keys != t->count) {
        return check_fail(&c, "leaf keys %zu != count %zu", c.leaf_keys, t->count);
    }
    return 0;
}
