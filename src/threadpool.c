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
