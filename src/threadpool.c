#include "threadpool.h"

#include <stdlib.h>

#include "threadpool_internal.h"

/* ---- 원형 큐 ---- */

JobQueue *job_queue_create(int capacity)
{
    if (capacity <= 0)
        return NULL;
    JobQueue *queue = calloc(1, sizeof(*queue));
    if (queue == NULL)
        return NULL;
    queue->items = malloc((size_t)capacity * sizeof(queue->items[0]));
    if (queue->items == NULL) {
        free(queue);
        return NULL;
    }
    queue->capacity = capacity;
    return queue;
}

void job_queue_destroy(JobQueue *queue)
{
    if (queue == NULL)
        return;
    free(queue->items);
    free(queue);
}

/* 끝 칸 다음은 0 번 칸: 나머지 연산 대신 비교 한 번으로 감는다 */
static int next_index(const JobQueue *queue, int index)
{
    return index + 1 == queue->capacity ? 0 : index + 1;
}

bool job_queue_push(JobQueue *queue, int value)
{
    if (queue->count == queue->capacity)
        return false;
    queue->items[queue->tail] = value;
    queue->tail = next_index(queue, queue->tail);
    queue->count++;
    return true;
}

bool job_queue_pop(JobQueue *queue, int *out_value)
{
    if (queue->count == 0)
        return false;
    *out_value = queue->items[queue->head];
    queue->head = next_index(queue, queue->head);
    queue->count--;
    return true;
}

int job_queue_count(const JobQueue *queue)
{
    return queue->count;
}

/* ---- 풀 ---- */

ThreadPool *threadpool_create_without_workers(int queue_capacity, ThreadPoolHandler handler,
                                              void *ctx)
{
    ThreadPool *pool = calloc(1, sizeof(*pool));
    if (pool == NULL)
        return NULL;
    pool->queue = job_queue_create(queue_capacity);
    if (pool->queue == NULL)
        goto cleanup_pool;
    if (pthread_mutex_init(&pool->mutex, NULL) != 0)
        goto cleanup_queue;
    if (pthread_cond_init(&pool->not_empty, NULL) != 0)
        goto cleanup_mutex;
    pool->handler = handler;
    pool->ctx = ctx;
    return pool;

cleanup_mutex:
    pthread_mutex_destroy(&pool->mutex);
cleanup_queue:
    job_queue_destroy(pool->queue);
cleanup_pool:
    free(pool);
    return NULL;
}

/* ---- 워커 ---- */

/* 꺼낼 작업이 생기거나 종료가 시작될 때까지 잠든다. 작업을 꺼냈으면 true.
 * - while 로 다시 확인한다: cond_wait 은 신호 없이도 깨어날 수 있고 (spurious wakeup),
 *   깨어나는 사이 다른 워커가 먼저 꺼내 갔을 수도 있다.
 * - 종료 중이어도 큐에 남은 것은 꺼낸다: 받아 둔 연결을 버리지 않고 다 처리한 뒤에 나간다.
 *   큐가 비고 종료 중일 때만 false (워커 종료). */
static bool wait_for_job(ThreadPool *pool, int *out_connfd)
{
    pthread_mutex_lock(&pool->mutex);
    while (job_queue_count(pool->queue) == 0 && !pool->is_shutting_down)
        pthread_cond_wait(&pool->not_empty, &pool->mutex);
    bool has_job = job_queue_pop(pool->queue, out_connfd);
    pthread_mutex_unlock(&pool->mutex);
    return has_job;
}

/* handler 는 락 밖에서 부른다: 요청 처리 (읽기, 엔진, 응답) 동안 다른 워커가 큐를 쓸 수 있어야
 * 병렬로 돈다. 락 안에서 부르면 워커가 N 개여도 한 번에 하나만 일한다. */
static void *worker_main(void *arg)
{
    ThreadPool *pool = arg;
    int connfd = -1;
    while (wait_for_job(pool, &connfd))
        pool->handler(connfd, pool->ctx);
    return NULL;
}

/* 워커를 worker_count 개 띄운다. 중간에 실패하면 이미 띄운 수만 worker_count 에 남기고 false
 * (threadpool_shutdown 이 그만큼만 join 한다) */
static bool start_workers(ThreadPool *pool, int worker_count)
{
    pool->workers = calloc((size_t)worker_count, sizeof(pool->workers[0]));
    if (pool->workers == NULL)
        return false;
    for (int i = 0; i < worker_count; i++) {
        if (pthread_create(&pool->workers[i], NULL, worker_main, pool) != 0)
            return false;
        pool->worker_count++;
    }
    return true;
}

ThreadPool *threadpool_create(int worker_count, int queue_capacity, ThreadPoolHandler handler,
                              void *ctx)
{
    if (worker_count <= 0 || handler == NULL)
        return NULL;
    ThreadPool *pool = threadpool_create_without_workers(queue_capacity, handler, ctx);
    if (pool == NULL)
        return NULL;
    if (!start_workers(pool, worker_count)) {
        threadpool_shutdown(pool); /* 이미 띄운 워커를 깨워 join 하고 모두 해제 */
        return NULL;
    }
    return pool;
}

/* 락 안에서는 큐에 넣고 신호만 보낸다: 기다리거나 I/O 를 하지 않으므로 accept 루프가 막히지 않는다.
 * signal (하나만 깨움) 로 충분하다 — 항목 하나는 워커 하나만 처리할 수 있다.
 * 종료 플래그도 같은 락 안에서 보므로, 종료가 시작된 뒤 들어온 fd 가 큐에 남아 버려지는 일이 없다. */
bool threadpool_submit(ThreadPool *pool, int connfd)
{
    pthread_mutex_lock(&pool->mutex);
    bool is_accepted = !pool->is_shutting_down && job_queue_push(pool->queue, connfd);
    if (is_accepted)
        pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->mutex);
    return is_accepted;
}

/* 종료 순서: 플래그 → broadcast → join → 해제.
 * broadcast 로 잠든 워커를 모두 깨우고, 워커는 큐를 비운 뒤 스스로 나온다 (wait_for_job).
 * join 이 끝나야 아무도 큐·뮤텍스를 쓰지 않으므로 그다음에만 해제한다. */
void threadpool_shutdown(ThreadPool *pool)
{
    if (pool == NULL)
        return;
    pthread_mutex_lock(&pool->mutex);
    pool->is_shutting_down = true;
    pthread_cond_broadcast(&pool->not_empty); /* 기다리는 워커를 모두 깨운다 */
    pthread_mutex_unlock(&pool->mutex);
    for (int i = 0; i < pool->worker_count; i++)
        pthread_join(pool->workers[i], NULL);
    free(pool->workers);
    pthread_cond_destroy(&pool->not_empty);
    pthread_mutex_destroy(&pool->mutex);
    job_queue_destroy(pool->queue);
    free(pool);
}
