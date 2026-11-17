#include "test.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
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

/* ---- S31 워커와 종료 ---- */

#define JOB_COUNT 1000
#define WORKER_COUNT 4
#define SMALL_QUEUE 16
#define RENDEZVOUS_JOBS 2 /* 처음 두 작업은 서로를 기다린다: 서로 다른 두 워커가 있어야 끝난다 */

/* 테스트가 열어 줄 때까지 handler 를 붙잡아 두는 문. 시간 (sleep) 에 기대지 않고 순서를 만든다 */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool is_open;
    int waiting; /* 문 앞에서 기다리는 handler 수 */
} Gate;

static void gate_init(Gate *gate)
{
    CHECK(pthread_mutex_init(&gate->mutex, NULL) == 0);
    CHECK(pthread_cond_init(&gate->changed, NULL) == 0);
    gate->is_open = false;
    gate->waiting = 0;
}

static void gate_destroy(Gate *gate)
{
    pthread_cond_destroy(&gate->changed);
    pthread_mutex_destroy(&gate->mutex);
}

static void gate_pass(Gate *gate)
{
    pthread_mutex_lock(&gate->mutex);
    gate->waiting++;
    pthread_cond_broadcast(&gate->changed);
    while (!gate->is_open)
        pthread_cond_wait(&gate->changed, &gate->mutex);
    gate->waiting--;
    pthread_mutex_unlock(&gate->mutex);
}

/* handler count 개가 문 앞에 도착할 때까지 기다린다 (= 워커가 작업을 꺼내 갔다) */
static void gate_wait_for(Gate *gate, int count)
{
    pthread_mutex_lock(&gate->mutex);
    while (gate->waiting < count)
        pthread_cond_wait(&gate->changed, &gate->mutex);
    pthread_mutex_unlock(&gate->mutex);
}

static void gate_open(Gate *gate)
{
    pthread_mutex_lock(&gate->mutex);
    gate->is_open = true;
    pthread_cond_broadcast(&gate->changed);
    pthread_mutex_unlock(&gate->mutex);
}

/* handler 가 공유하는 상태. 워커 여러 개가 동시에 쓰는 칸은 모두 원자 변수 */
typedef struct {
    atomic_int handled;
    atomic_int seen[JOB_COUNT];       /* connfd 별 처리 횟수 */
    pthread_t threads[JOB_COUNT];     /* connfd 를 처리한 스레드 (칸마다 한 워커만 쓴다) */
    int order[JOB_COUNT];             /* 워커 1 개 테스트에서만 쓴다 */
    Gate *gate;                       /* NULL 이면 막지 않는다 */
    pthread_barrier_t *rendezvous;    /* NULL 이면 쓰지 않는다 */
} Work;

static void record_job(int connfd, void *ctx)
{
    Work *work = ctx;
    if (work->rendezvous != NULL && connfd < RENDEZVOUS_JOBS)
        pthread_barrier_wait(work->rendezvous);
    if (work->gate != NULL)
        gate_pass(work->gate);
    int index = atomic_fetch_add(&work->handled, 1);
    work->order[index % JOB_COUNT] = connfd;
    work->threads[connfd] = pthread_self();
    atomic_fetch_add(&work->seen[connfd], 1);
}

static Work *create_work(void)
{
    Work *work = calloc(1, sizeof(*work));
    CHECK(work != NULL);
    return work;
}

static ThreadPool *create_pool(int workers, int capacity, Work *work)
{
    ThreadPool *pool = threadpool_create(workers, capacity, record_job, work);
    CHECK(pool != NULL);
    return pool;
}

/* 큐가 차 있으면 워커가 비울 때까지 다시 넣는다 */
static void submit_all(ThreadPool *pool, int count)
{
    for (int connfd = 0; connfd < count; connfd++) {
        while (!threadpool_submit(pool, connfd))
            sched_yield();
    }
}

/* T31-01, T31-03 (ASan 빌드에서 누수 없음) */
static void test_workers_handle_every_job_once(void)
{
    Work *work = create_work();
    ThreadPool *pool = create_pool(WORKER_COUNT, SMALL_QUEUE, work);
    submit_all(pool, JOB_COUNT);
    threadpool_shutdown(pool); /* 남은 작업을 다 처리하고 join 한 뒤 돌아온다 */
    CHECK(atomic_load(&work->handled) == JOB_COUNT);
    for (int i = 0; i < JOB_COUNT; i++)
        CHECK(atomic_load(&work->seen[i]) == 1);
    free(work);
}

/* T31-02 */
static void test_jobs_spread_over_workers(void)
{
    Work *work = create_work();
    pthread_barrier_t rendezvous;
    CHECK(pthread_barrier_init(&rendezvous, NULL, RENDEZVOUS_JOBS) == 0);
    work->rendezvous = &rendezvous;
    ThreadPool *pool = create_pool(WORKER_COUNT, SMALL_QUEUE, work);
    submit_all(pool, JOB_COUNT);
    threadpool_shutdown(pool);
    pthread_barrier_destroy(&rendezvous);
    /* 작업 0 과 1 은 barrier 에서 서로를 기다렸으므로 동시에 handler 안에 있었다 = 다른 워커 */
    CHECK(!pthread_equal(work->threads[0], work->threads[1]));
    CHECK(atomic_load(&work->handled) == JOB_COUNT);
    free(work);
}

typedef struct {
    ThreadPool *pool;
} ShutdownCall;

static void *call_shutdown(void *arg)
{
    threadpool_shutdown(((ShutdownCall *)arg)->pool);
    return NULL;
}

static bool is_shutting_down(ThreadPool *pool)
{
    pthread_mutex_lock(&pool->mutex);
    bool flag = pool->is_shutting_down;
    pthread_mutex_unlock(&pool->mutex);
    return flag;
}

/* E31-01, E31-02 */
static void test_shutdown_drains_queue(void)
{
    Gate gate;
    gate_init(&gate);
    Work *work = create_work();
    work->gate = &gate;
    ThreadPool *pool = create_pool(WORKER_COUNT, SMALL_QUEUE, work);
    submit_all(pool, WORKER_COUNT);
    gate_wait_for(&gate, WORKER_COUNT); /* 워커가 모두 handler 안에 붙잡혔다 */
    for (int connfd = WORKER_COUNT; connfd < WORKER_COUNT + SMALL_QUEUE; connfd++)
        CHECK(threadpool_submit(pool, connfd)); /* 큐를 가득 채운다 */

    pthread_t closer;
    ShutdownCall call = {pool};
    CHECK(pthread_create(&closer, NULL, call_shutdown, &call) == 0);
    while (!is_shutting_down(pool))
        sched_yield();
    CHECK(!threadpool_submit(pool, JOB_COUNT - 1)); /* E31-02: 종료 중에는 받지 않는다 */
    gate_open(&gate);
    CHECK(pthread_join(closer, NULL) == 0);

    CHECK(atomic_load(&work->handled) == WORKER_COUNT + SMALL_QUEUE); /* 큐에 남은 것까지 */
    CHECK(atomic_load(&work->seen[JOB_COUNT - 1]) == 0);
    gate_destroy(&gate);
    free(work);
}

/* E31-03 */
static void test_shutdown_without_jobs(void)
{
    Work *work = create_work();
    ThreadPool *pool = create_pool(WORKER_COUNT, SMALL_QUEUE, work);
    threadpool_shutdown(pool); /* 기다리던 워커가 broadcast 로 깨어나 나간다 (멈추면 테스트가 끝나지 않음) */
    CHECK(atomic_load(&work->handled) == 0);
    free(work);
}

/* E31-04 */
static void test_single_worker_keeps_order(void)
{
    Work *work = create_work();
    ThreadPool *pool = create_pool(1, SMALL_QUEUE, work);
    submit_all(pool, JOB_COUNT);
    threadpool_shutdown(pool);
    CHECK(atomic_load(&work->handled) == JOB_COUNT);
    for (int i = 0; i < JOB_COUNT; i++)
        CHECK(work->order[i] == i); /* 워커가 하나면 FIFO 순서 그대로 */
    free(work);
}

/* E31-05 */
static void test_invalid_worker_count(void)
{
    Work *work = create_work();
    CHECK(threadpool_create(0, SMALL_QUEUE, record_job, work) == NULL);
    CHECK(threadpool_create(-1, SMALL_QUEUE, record_job, work) == NULL);
    CHECK(threadpool_create(WORKER_COUNT, 0, record_job, work) == NULL);
    CHECK(threadpool_create(WORKER_COUNT, SMALL_QUEUE, NULL, work) == NULL);
    free(work);
}

/* E31-06 */
static void test_busy_workers_cause_backpressure(void)
{
    Gate gate;
    gate_init(&gate);
    Work *work = create_work();
    work->gate = &gate;
    ThreadPool *pool = create_pool(1, 2, work);
    CHECK(threadpool_submit(pool, 0));
    gate_wait_for(&gate, 1); /* 워커가 작업 0 을 붙잡고 있다 (느린 handler) */
    CHECK(threadpool_submit(pool, 1));
    CHECK(threadpool_submit(pool, 2));
    CHECK(!threadpool_submit(pool, 3)); /* 큐 2 칸이 다 찼다: 기다리지 않고 false */
    gate_open(&gate);
    threadpool_shutdown(pool);
    CHECK(atomic_load(&work->handled) == 3);
    gate_destroy(&gate);
    free(work);
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
    RUN(test_workers_handle_every_job_once);
    RUN(test_jobs_spread_over_workers);
    RUN(test_shutdown_drains_queue);
    RUN(test_shutdown_without_jobs);
    RUN(test_single_worker_keeps_order);
    RUN(test_invalid_worker_count);
    RUN(test_busy_workers_cause_backpressure);
    TEST_DONE();
    return 0;
}
