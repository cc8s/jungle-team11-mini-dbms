#include <limits.h>

#include "bptree.h"
#include "test.h"

static char msg[256];

/* 불변식이 깨지면 이유를 찍고 실패한다 */
#define CHECK_TREE(t)                                                                          \
    do {                                                                                       \
        if (bptree_check((t), msg, sizeof(msg)) != 0) {                                        \
            fprintf(stderr, "  invariant: %s\n", msg);                                         \
        }                                                                                      \
        CHECK(bptree_check((t), msg, sizeof(msg)) == 0);                                       \
    } while (0)

/* 값은 키에서 계산할 수 있게 둔다: 오프셋처럼 보이게 32 + key * 112 */
static uint64_t value_of(long long key)
{
    return 32 + (uint64_t)key * 112;
}

static void check_all_found(const struct bptree *t, long long first, long long last)
{
    uint64_t v;

    for (long long k = first; k <= last; k++) {
        CHECK(bptree_find(t, k, &v) && v == value_of(k));
    }
}

/* 재현 가능한 순열 (Fisher–Yates + 고정 시드 LCG) */
static void shuffle(long long *a, int n, unsigned seed)
{
    for (int i = n - 1; i > 0; i--) {
        seed = seed * 1103515245u + 12345u;
        int j = (int)((seed >> 8) % (unsigned)(i + 1));
        long long tmp = a[i];
        a[i] = a[j];
        a[j] = tmp;
    }
}

/* TC-BPT-001 EP: 빈 트리 */
static void test_empty(void)
{
    struct bptree t;
    uint64_t v;
    long long k;

    CHECK(bptree_init(&t, 4) == 0);
    CHECK(!bptree_find(&t, 1, &v));
    CHECK(!bptree_max_key(&t, &k));
    CHECK(t.height == 1 && t.count == 0);
    CHECK_TREE(&t);
    bptree_free(&t);

    CHECK(bptree_init(&t, BPTREE_MIN_ORDER - 1) < 0);
}

/* TC-BPT-002 BVA: 루트 리프 분할 경계. order 4 = 키 최대 3개. 3개까지 높이 1, 4번째에 2 */
static void test_root_split_boundary(void)
{
    struct bptree t;

    CHECK(bptree_init(&t, 4) == 0);
    for (long long k = 1; k <= 3; k++) {
        CHECK(bptree_insert(&t, k, value_of(k)) == BPTREE_OK);
    }
    CHECK(t.height == 1);
    CHECK(bptree_insert(&t, 4, value_of(4)) == BPTREE_OK);
    CHECK(t.height == 2);
    CHECK_TREE(&t);
    check_all_found(&t, 1, 4);
    bptree_free(&t);
}

/* TC-BPT-003 EP: 순차 삽입 (자동 증가 id 의 실제 모양). 차수 3·4·5·128, 넣을 때마다 불변식 검사 */
static void test_sequential(void)
{
    static const int orders[] = {3, 4, 5, 128};
    uint64_t v;

    for (size_t o = 0; o < sizeof(orders) / sizeof(orders[0]); o++) {
        struct bptree t;
        CHECK(bptree_init(&t, orders[o]) == 0);
        for (long long k = 1; k <= 2000; k++) {
            CHECK(bptree_insert(&t, k, value_of(k)) == BPTREE_OK);
            CHECK_TREE(&t);
        }
        CHECK(t.count == 2000);
        check_all_found(&t, 1, 2000);
        CHECK(!bptree_find(&t, 0, &v) && !bptree_find(&t, 2001, &v));
        bptree_free(&t);
    }
}

/* TC-BPT-004 EP: 역순 삽입 — 늘 맨 왼쪽 리프가 나뉜다 */
static void test_reverse(void)
{
    struct bptree t;

    CHECK(bptree_init(&t, 4) == 0);
    for (long long k = 2000; k >= 1; k--) {
        CHECK(bptree_insert(&t, k, value_of(k)) == BPTREE_OK);
        CHECK_TREE(&t);
    }
    check_all_found(&t, 1, 2000);
    bptree_free(&t);
}

/* TC-BPT-005 EP: 무작위 순서 — 가운데 노드 분할, 내부 노드 분할이 섞인다 */
static void test_random(void)
{
    static const int orders[] = {3, 4, 7, 128};
    enum { N = 20000 };
    static long long keys[N];

    for (size_t o = 0; o < sizeof(orders) / sizeof(orders[0]); o++) {
        struct bptree t;
        for (int i = 0; i < N; i++) {
            keys[i] = i + 1;
        }
        shuffle(keys, N, 42u + (unsigned)o);
        CHECK(bptree_init(&t, orders[o]) == 0);
        for (int i = 0; i < N; i++) {
            CHECK(bptree_insert(&t, keys[i], value_of(keys[i])) == BPTREE_OK);
            if (i % 997 == 0) {
                CHECK_TREE(&t);
            }
        }
        CHECK_TREE(&t);
        check_all_found(&t, 1, N);
        bptree_free(&t);
    }
}

/* TC-BPT-006 EG: 같은 키를 다시 넣으면 DUPLICATE, 개수와 기존 값은 그대로 (학습 노트 2 Q13) */
static void test_duplicate(void)
{
    struct bptree t;
    uint64_t v;

    CHECK(bptree_init(&t, 4) == 0);
    for (long long k = 1; k <= 50; k++) {
        CHECK(bptree_insert(&t, k, value_of(k)) == BPTREE_OK);
    }
    CHECK(bptree_insert(&t, 1, 999) == BPTREE_DUPLICATE);  /* 맨 왼쪽 리프 */
    CHECK(bptree_insert(&t, 25, 999) == BPTREE_DUPLICATE); /* 가운데, 안내 키일 수 있음 */
    CHECK(bptree_insert(&t, 50, 999) == BPTREE_DUPLICATE); /* 맨 오른쪽 */
    CHECK(t.count == 50);
    CHECK(bptree_find(&t, 25, &v) && v == value_of(25));
    CHECK_TREE(&t);
    bptree_free(&t);
}

struct collect {
    long long keys[64];
    int n;
    int stop_after;
};

static int collect_fn(long long key, uint64_t value, void *ctx)
{
    struct collect *c = ctx;

    if (value != value_of(key) || c->n == 64) {
        return 1;
    }
    c->keys[c->n++] = key;
    return c->stop_after && c->n == c->stop_after;
}

static int range(const struct bptree *t, long long lo, long long hi, struct collect *c)
{
    c->n = 0;
    bptree_range(t, lo, hi, collect_fn, c);
    return c->n;
}

/* TC-BPT-007 BVA: BETWEEN (D004) — 양 끝 포함, 리프 경계 넘기, 빈 구간, 범위 밖, 틈 있는 키 */
static void test_range(void)
{
    struct bptree t;
    struct collect c = {.stop_after = 0};

    CHECK(bptree_init(&t, 4) == 0); /* 리프에 키 3개 → 구간이 여러 리프를 넘는다 */
    for (long long k = 2; k <= 100; k += 2) {
        CHECK(bptree_insert(&t, k, value_of(k)) == BPTREE_OK);
    }
    CHECK(range(&t, 10, 20, &c) == 6 && c.keys[0] == 10 && c.keys[5] == 20);
    CHECK(range(&t, 11, 19, &c) == 4 && c.keys[0] == 12 && c.keys[3] == 18); /* 끝이 없는 키 */
    CHECK(range(&t, 30, 30, &c) == 1 && c.keys[0] == 30);
    CHECK(range(&t, 31, 31, &c) == 0);
    CHECK(range(&t, 20, 10, &c) == 0);                                      /* low > high */
    CHECK(range(&t, -100, 4, &c) == 2 && c.keys[0] == 2);                   /* 왼쪽 밖 */
    CHECK(range(&t, 98, 1000, &c) == 2 && c.keys[1] == 100);                /* 오른쪽 밖 */
    CHECK(range(&t, 101, 200, &c) == 0);
    for (int i = 1; i < range(&t, 2, 100, &c); i++) {
        CHECK(c.keys[i - 1] < c.keys[i]);
    }
    CHECK(c.n == 50);
    bptree_free(&t);
}

/* TC-BPT-008 EP: 콜백이 멈추라고 하면 거기서 멈춘다 */
static void test_range_stop(void)
{
    struct bptree t;
    struct collect c = {.stop_after = 3};

    CHECK(bptree_init(&t, 4) == 0);
    for (long long k = 1; k <= 30; k++) {
        CHECK(bptree_insert(&t, k, value_of(k)) == BPTREE_OK);
    }
    CHECK(range(&t, 5, 30, &c) == 3 && c.keys[2] == 7);
    bptree_free(&t);
}

static int count_fn(long long key, uint64_t value, void *ctx)
{
    (void)key;
    (void)value;
    (void)ctx;
    return 0;
}

/* TC-BPT-009 BVA: long long 끝값과 음수 키 */
static void test_extreme_keys(void)
{
    struct bptree t;
    uint64_t v;
    long long max;

    CHECK(bptree_init(&t, 3) == 0);
    long long keys[] = {0, -1, LLONG_MAX, LLONG_MIN, 5, -5, LLONG_MAX - 1, LLONG_MIN + 1};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        CHECK(bptree_insert(&t, keys[i], 7) == BPTREE_OK);
        CHECK_TREE(&t);
    }
    CHECK(bptree_find(&t, LLONG_MIN, &v) && bptree_find(&t, LLONG_MAX, &v));
    CHECK(bptree_max_key(&t, &max) && max == LLONG_MAX);
    CHECK(bptree_range(&t, LLONG_MIN, LLONG_MAX, count_fn, NULL) == 8);
    CHECK(bptree_range(&t, -5, 0, count_fn, NULL) == 3);
    bptree_free(&t);
}

/* TC-BPT-010 EP: 가장 큰 키 (재시작 때 next_id 계산에 쓴다) */
static void test_max_key(void)
{
    struct bptree t;
    long long max;

    CHECK(bptree_init(&t, 4) == 0);
    long long keys[] = {5, 90, 3, 41, 90, 7};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        bptree_insert(&t, keys[i], value_of(keys[i]));
    }
    CHECK(bptree_max_key(&t, &max) && max == 90);
    bptree_free(&t);
}

/* TC-BPT-011 EP: append split 효과 측정 — 순차 10만 개, 차수 128.
 * 반씩 나누면 왼쪽 리프가 다시 채워지지 않아 약 50%, append split 이면 거의 100% (학습 노트 1 Q6) */
static void test_append_split_fill(void)
{
    struct bptree half;
    struct bptree append;
    struct bptree_stats hs;
    struct bptree_stats as;

    CHECK(bptree_init(&half, 128) == 0);
    CHECK(bptree_init(&append, 128) == 0);
    half.is_append_split = false;
    for (long long k = 1; k <= 100000; k++) {
        CHECK(bptree_insert(&half, k, value_of(k)) == BPTREE_OK);
        CHECK(bptree_insert(&append, k, value_of(k)) == BPTREE_OK);
    }
    CHECK_TREE(&half);
    CHECK_TREE(&append);
    bptree_stats(&half, &hs);
    bptree_stats(&append, &as);
    printf("    half split : leaves %zu, internal %zu, fill %.3f, height %d\n", hs.leaf_count,
           hs.internal_count, hs.leaf_fill, half.height);
    printf("    append split: leaves %zu, internal %zu, fill %.3f, height %d\n", as.leaf_count,
           as.internal_count, as.leaf_fill, append.height);
    CHECK(hs.leaf_fill > 0.49 && hs.leaf_fill < 0.52);
    CHECK(as.leaf_fill > 0.99);
    CHECK(as.leaf_count == (100000 + 126) / 127); /* 꽉 찬 리프 + 마지막 하나 = ⌈N / 127⌉ */
    /* 반씩 나누면 왼쪽에 127 개 중 64 개(50.4%)가 남아 정확히 2배는 아니다 */
    CHECK(hs.leaf_count * 10 >= as.leaf_count * 19);
    bptree_free(&half);
    bptree_free(&append);
}

/* TC-BPT-012 BVA: 100만 개 순차 — 높이가 log 로만 자란다 (차수 128 이면 3) */
static void test_million(void)
{
    struct bptree t;
    uint64_t v;

    CHECK(bptree_init(&t, BPTREE_DEFAULT_ORDER) == 0);
    for (long long k = 1; k <= 1000000; k++) {
        CHECK(bptree_insert(&t, k, value_of(k)) == BPTREE_OK);
    }
    CHECK_TREE(&t);
    CHECK(t.height == 3);
    CHECK(bptree_find(&t, 1, &v) && v == value_of(1));
    CHECK(bptree_find(&t, 777777, &v) && v == value_of(777777));
    CHECK(bptree_find(&t, 1000000, &v) && v == value_of(1000000));
    CHECK(!bptree_find(&t, 1000001, &v));
    bptree_free(&t);
}

/* TC-BPT-013 EG: 검사기가 헛돌지 않는다 — 개수·높이를 일부러 틀리게 하면 잡는다 */
static void test_checker_catches(void)
{
    struct bptree t;

    CHECK(bptree_init(&t, 4) == 0);
    for (long long k = 1; k <= 20; k++) {
        bptree_insert(&t, k, value_of(k));
    }
    t.count++;
    CHECK(bptree_check(&t, msg, sizeof(msg)) < 0 && strstr(msg, "count") != NULL);
    t.count--;
    t.height++;
    CHECK(bptree_check(&t, msg, sizeof(msg)) < 0 && strstr(msg, "depth") != NULL);
    t.height--;
    t.max_keys = 1; /* 노드가 넘친 것처럼 보이게 */
    CHECK(bptree_check(&t, msg, sizeof(msg)) < 0 && strstr(msg, "max") != NULL);
    t.max_keys = 3;
    CHECK_TREE(&t);
    bptree_free(&t);
}

int main(void)
{
    RUN(test_empty);
    RUN(test_root_split_boundary);
    RUN(test_sequential);
    RUN(test_reverse);
    RUN(test_random);
    RUN(test_duplicate);
    RUN(test_range);
    RUN(test_range_stop);
    RUN(test_extreme_keys);
    RUN(test_max_key);
    RUN(test_append_split_fill);
    RUN(test_million);
    RUN(test_checker_catches);
    TEST_DONE();
    return 0;
}
