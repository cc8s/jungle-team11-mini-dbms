#include "test.h"

#include <pthread.h>
#include <stdint.h>

#include "threadpool.h"
#include "threadpool_internal.h"

#define SMALL_CAPACITY 4
#define PUSHER_COUNT 8
#define PUSHES_PER_THREAD 100
#define SHARED_CAPACITY 64
#define VALUE_STRIDE 1000 /* 스레드 t 의 i 번째 값 = t × STRIDE + i (서로 겹치지 않음) */
#define ROUND_COUNT 100

static JobQueue *create_queue(int capacity)
{
    JobQueue *queue = job_queue_create(capacity);
    CHECK(queue != NULL);
    return queue;
}

static int pop_one(JobQueue *queue)
{
    int value = -1;
    CHECK(job_queue_pop(queue, &value));
    return value;
}

static void push_range(JobQueue *queue, int first, int count)
{
    for (int i = 0; i < count; i++)
        CHECK(job_queue_push(queue, first + i));
}

/* T30-01 */
static void test_queue_is_fifo(void)
{
    JobQueue *queue = create_queue(SMALL_CAPACITY);
    push_range(queue, 10, SMALL_CAPACITY);
    for (int i = 0; i < SMALL_CAPACITY; i++)
        CHECK(pop_one(queue) == 10 + i);
    job_queue_destroy(queue);
}

/* T30-02 */
static void test_queue_wraps_around(void)
{
    JobQueue *queue = create_queue(SMALL_CAPACITY);
    push_range(queue, 1, 3);
    CHECK(pop_one(queue) == 1);
    CHECK(pop_one(queue) == 2);
    push_range(queue, 4, 3); /* tail 이 배열 끝을 넘어 앞으로 돈다 */
    for (int expected = 3; expected <= 6; expected++)
        CHECK(pop_one(queue) == expected);
    job_queue_destroy(queue);
}

/* T30-03 */
static void test_queue_tracks_count(void)
{
    JobQueue *queue = create_queue(SMALL_CAPACITY);
    CHECK(job_queue_count(queue) == 0);
    for (int i = 1; i <= SMALL_CAPACITY; i++) {
        CHECK(job_queue_push(queue, i));
        CHECK(job_queue_count(queue) == i);
    }
    for (int i = SMALL_CAPACITY - 1; i >= 0; i--) {
        pop_one(queue);
        CHECK(job_queue_count(queue) == i);
    }
    int untouched = 77;
    CHECK(!job_queue_pop(queue, &untouched)); /* 빈 큐 */
    CHECK(untouched == 77);
    job_queue_destroy(queue);
}

/* E30-01 */
static void test_push_to_full_queue(void)
{
    JobQueue *queue = create_queue(SMALL_CAPACITY);
    push_range(queue, 1, SMALL_CAPACITY);
    CHECK(!job_queue_push(queue, 99));
    CHECK(job_queue_count(queue) == SMALL_CAPACITY);
    for (int i = 1; i <= SMALL_CAPACITY; i++)
        CHECK(pop_one(queue) == i); /* 기존 항목 그대로, 99 는 없음 */
    job_queue_destroy(queue);
}

/* E30-02 */
static void test_capacity_one_queue(void)
{
    JobQueue *queue = create_queue(1);
    for (int i = 0; i < ROUND_COUNT; i++) {
        CHECK(job_queue_push(queue, i));
        CHECK(!job_queue_push(queue, -i));
        CHECK(pop_one(queue) == i);
    }
    CHECK(job_queue_count(queue) == 0);
    job_queue_destroy(queue);
}

/* E30-03 */
static void test_invalid_capacity(void)
{
    CHECK(job_queue_create(0) == NULL);
    CHECK(job_queue_create(-1) == NULL);
    CHECK(threadpool_create_without_workers(0, NULL, NULL) == NULL);
    CHECK(threadpool_create_without_workers(-5, NULL, NULL) == NULL);
    job_queue_destroy(NULL);
    threadpool_shutdown(NULL);
}

/* ---- E30-04 동시 submit ---- */

typedef struct {
    ThreadPool *pool;
    pthread_barrier_t *start; /* 모든 스레드가 같이 출발해 경쟁을 최대로 만든다 */
    int thread_index;
    int accepted; /* 이 스레드만 쓴다: 합계는 join 뒤에 계산하므로 공유 쓰기가 없다 */
} Pusher;

static void *push_many(void *arg)
{
    Pusher *pusher = arg;
    pthread_barrier_wait(pusher->start);
    for (int i = 0; i < PUSHES_PER_THREAD; i++) {
        int value = pusher->thread_index * VALUE_STRIDE + i;
        pusher->accepted += threadpool_submit(pusher->pool, value);
    }
    return NULL;
}

/* E30-04 */
static void test_concurrent_submit(void)
{
    ThreadPool *pool = threadpool_create_without_workers(SHARED_CAPACITY, NULL, NULL);
    CHECK(pool != NULL);
    pthread_barrier_t start;
    CHECK(pthread_barrier_init(&start, NULL, PUSHER_COUNT) == 0);
    pthread_t threads[PUSHER_COUNT];
    Pusher pushers[PUSHER_COUNT];
    for (int t = 0; t < PUSHER_COUNT; t++) {
        pushers[t] = (Pusher){.pool = pool, .start = &start, .thread_index = t};
        CHECK(pthread_create(&threads[t], NULL, push_many, &pushers[t]) == 0);
    }
    int accepted = 0;
    for (int t = 0; t < PUSHER_COUNT; t++) {
        CHECK(pthread_join(threads[t], NULL) == 0);
        accepted += pushers[t].accepted;
    }
    pthread_barrier_destroy(&start);

    CHECK(accepted == SHARED_CAPACITY); /* 나머지 736 번은 false */
    bool seen[PUSHER_COUNT * VALUE_STRIDE] = {false};
    int value = -1;
    for (int i = 0; i < SHARED_CAPACITY; i++) { /* 워커가 없으니 큐에 그대로 남아 있다 */
        CHECK(job_queue_pop(pool->queue, &value));
        CHECK(value >= 0 && value < PUSHER_COUNT * VALUE_STRIDE && !seen[value]);
        seen[value] = true; /* 같은 값이 두 번 들어가거나 깨진 값이 없음 */
    }
    CHECK(!job_queue_pop(pool->queue, &value));
    threadpool_shutdown(pool);
}

/* submit 은 종료가 시작된 뒤에는 받지 않는다 (S31 E31-02 의 큐 쪽 전제) */
static void test_submit_after_shutdown_flag(void)
{
    ThreadPool *pool = threadpool_create_without_workers(SMALL_CAPACITY, NULL, NULL);
    CHECK(pool != NULL);
    CHECK(threadpool_submit(pool, 1));
    pool->is_shutting_down = true; /* 단일 스레드라 락 없이 바꿔도 된다 */
    CHECK(!threadpool_submit(pool, 2));
    CHECK(job_queue_count(pool->queue) == 1);
    threadpool_shutdown(pool);
}

int main(void)
{
    printf("test_threadpool\n");
    RUN(test_queue_is_fifo);
    RUN(test_queue_wraps_around);
    RUN(test_queue_tracks_count);
    RUN(test_push_to_full_queue);
    RUN(test_capacity_one_queue);
    RUN(test_invalid_capacity);
    RUN(test_concurrent_submit);
    RUN(test_submit_after_shutdown_flag);
    TEST_DONE();
    return 0;
}
