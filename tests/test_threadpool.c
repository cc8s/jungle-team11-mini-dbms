#include <errno.h>
#include <sched.h>
#include <time.h>

#include "test.h"
#include "threadpool.h"

/* ---- 도우미: 열고 닫는 문(gate)과 시간 제한 대기 ------------------------------------------ */

struct gate {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    bool is_open;
    int waiting; /* 문 앞에서 기다리는 일 수 */
    int peak;    /* 동시에 기다린 최대 수 */
};

static void gate_init(struct gate *g)
{
    pthread_mutex_init(&g->lock, NULL);
    pthread_cond_init(&g->changed, NULL);
    g->is_open = false;
    g->waiting = 0;
    g->peak = 0;
}

static void gate_destroy(struct gate *g)
{
    pthread_cond_destroy(&g->changed);
    pthread_mutex_destroy(&g->lock);
}

static void deadline_after_ms(struct timespec *ts, long ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

/* 일: 문이 열릴 때까지 기다린다 (최대 5초. 테스트가 멈추지 않게) */
static void wait_at_gate(void *arg)
{
    struct gate *g = arg;
    struct timespec deadline;

    deadline_after_ms(&deadline, 5000);
    pthread_mutex_lock(&g->lock);
    g->waiting++;
    if (g->waiting > g->peak) {
        g->peak = g->waiting;
    }
    pthread_cond_broadcast(&g->changed);
    while (!g->is_open) {
        if (pthread_cond_timedwait(&g->changed, &g->lock, &deadline) == ETIMEDOUT) {
            break;
        }
    }
    g->waiting--;
    pthread_mutex_unlock(&g->lock);
}

/* 문 앞에 n 개가 모일 때까지 기다린다. 제한 시간 안에 모이면 true */
static bool gate_wait_for(struct gate *g, int n, long ms)
{
    struct timespec deadline;
    bool ok = true;

    deadline_after_ms(&deadline, ms);
    pthread_mutex_lock(&g->lock);
    while (g->waiting < n) {
        if (pthread_cond_timedwait(&g->changed, &g->lock, &deadline) == ETIMEDOUT) {
            ok = g->waiting >= n;
            break;
        }
    }
    pthread_mutex_unlock(&g->lock);
    return ok;
}

static void gate_open(struct gate *g)
{
    pthread_mutex_lock(&g->lock);
    g->is_open = true;
    pthread_cond_broadcast(&g->changed);
    pthread_mutex_unlock(&g->lock);
}

/* ---- 일: 칸 하나를 1 올린다 ------------------------------------------------------------------ */

enum { MANY = 20000 };
static int hits[MANY];

static void hit_slot(void *arg)
{
    int *slot = arg;
    __atomic_add_fetch(slot, 1, __ATOMIC_RELAXED);
}

/* TC-TP-001 EP: 일 2만 개가 빠짐없이, 한 번씩만 실행된다 (큐가 차면 잠깐 쉬고 다시 넣는다) */
static void test_every_job_runs_once(void)
{
    struct threadpool tp;
    int retries = 0;

    memset(hits, 0, sizeof(hits));
    CHECK(threadpool_init(&tp, 4, 64) == 0);
    for (int i = 0; i < MANY; i++) {
        while (threadpool_submit(&tp, hit_slot, &hits[i]) != 0) {
            retries++;
            sched_yield();
        }
    }
    threadpool_shutdown(&tp);
    for (int i = 0; i < MANY; i++) {
        CHECK(hits[i] == 1);
    }
    CHECK(tp.submitted == MANY && tp.completed == MANY);
    CHECK((int)tp.rejected == retries);
}

/* TC-TP-002 EP: 워커 4개면 일 4개가 실제로 동시에 돈다 (Q10: 요청마다 줄 서지 않는다) */
static void test_runs_in_parallel(void)
{
    struct threadpool tp;
    struct gate g;

    gate_init(&g);
    CHECK(threadpool_init(&tp, 4, 16) == 0);
    for (int i = 0; i < 4; i++) {
        CHECK(threadpool_submit(&tp, wait_at_gate, &g) == 0);
    }
    CHECK(gate_wait_for(&g, 4, 2000)); /* 넷이 동시에 문 앞에 있어야 한다 */
    gate_open(&g);
    threadpool_shutdown(&tp);
    CHECK(g.peak == 4);
    gate_destroy(&g);
}

/* TC-TP-003 BVA: 큐가 가득 차면 submit 은 기다리지 않고 바로 실패한다 (Q12: 503 의 근거) */
static void test_full_queue_rejects(void)
{
    struct threadpool tp;
    struct gate g;

    gate_init(&g);
    CHECK(threadpool_init(&tp, 1, 2) == 0);
    CHECK(threadpool_submit(&tp, wait_at_gate, &g) == 0);
    CHECK(gate_wait_for(&g, 1, 2000)); /* 워커 하나가 문 앞에 묶였다 → 큐는 비었다 */
    CHECK(threadpool_submit(&tp, wait_at_gate, &g) == 0); /* 큐 1/2 */
    CHECK(threadpool_submit(&tp, wait_at_gate, &g) == 0); /* 큐 2/2 */
    CHECK(threadpool_submit(&tp, wait_at_gate, &g) != 0); /* 넘침 */
    CHECK(tp.rejected == 1);
    gate_open(&g);
    threadpool_shutdown(&tp);
    CHECK(tp.completed == 3);
    gate_destroy(&g);
}

/* TC-TP-004 ST: 종료는 큐에 남은 일을 다 끝낸 뒤 돌아온다 */
static void test_shutdown_drains_queue(void)
{
    struct threadpool tp;
    struct gate g;
    int slots[5] = {0};

    gate_init(&g);
    CHECK(threadpool_init(&tp, 1, 8) == 0);
    CHECK(threadpool_submit(&tp, wait_at_gate, &g) == 0);
    CHECK(gate_wait_for(&g, 1, 2000));
    for (int i = 0; i < 5; i++) {
        CHECK(threadpool_submit(&tp, hit_slot, &slots[i]) == 0);
    }
    gate_open(&g);
    threadpool_shutdown(&tp);
    for (int i = 0; i < 5; i++) {
        CHECK(slots[i] == 1);
    }
    CHECK(tp.completed == 6);
    gate_destroy(&g);
}

static double process_cpu_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* TC-TP-005 EG: 일이 없으면 워커는 잠든다 — 워커 8개가 300ms 노는 동안 CPU 를 거의 안 쓴다 (Q11).
 * 바쁜 대기였다면 8개 × 300ms 에 가까운 CPU 시간이 쌓인다 */
static void test_idle_workers_sleep(void)
{
    struct threadpool tp;
    struct timespec nap = {0, 300 * 1000000L};

    CHECK(threadpool_init(&tp, 8, 16) == 0);
    double before = process_cpu_ms();
    nanosleep(&nap, NULL);
    double used = process_cpu_ms() - before;
    threadpool_shutdown(&tp);
    printf("    idle 300ms with 8 workers: %.2f ms CPU\n", used);
    CHECK(used < 30.0);
}

/* TC-TP-006 BVA: 워커 0개, 큐 0칸은 거절 */
static void test_bad_arguments(void)
{
    struct threadpool tp;

    CHECK(threadpool_init(&tp, 0, 4) < 0);
    CHECK(threadpool_init(&tp, 4, 0) < 0);
    CHECK(threadpool_init(&tp, 1, 1) == 0);
    threadpool_shutdown(&tp);
}

int main(void)
{
    RUN(test_every_job_runs_once);
    RUN(test_runs_in_parallel);
    RUN(test_full_queue_rejects);
    RUN(test_shutdown_drains_queue);
    RUN(test_idle_workers_sleep);
    RUN(test_bad_arguments);
    TEST_DONE();
    return 0;
}
