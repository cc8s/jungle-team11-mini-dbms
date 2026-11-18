#include "threadpool.h"

#include <stdlib.h>
#include <string.h>

static void *worker_main(void *arg)
{
    struct threadpool *tp = arg;

    for (;;) {
        pthread_mutex_lock(&tp->lock);
        /* while 이지 if 가 아니다: 깨어나도 다른 워커가 먼저 가져갔거나 이유 없이 깼을 수 있다 (Q11) */
        while (tp->count == 0 && !tp->is_shutting_down) {
            pthread_cond_wait(&tp->not_empty, &tp->lock);
        }
        if (tp->count == 0) { /* 종료 중이고 남은 일도 없음 */
            pthread_mutex_unlock(&tp->lock);
            return NULL;
        }
        struct threadpool_job job = tp->queue[tp->head];
        tp->head = (tp->head + 1) % tp->queue_cap;
        tp->count--;
        pthread_mutex_unlock(&tp->lock);

        job.fn(job.arg); /* 락 밖에서 실행: 일이 오래 걸려도 다른 워커가 큐를 쓸 수 있다 */

        pthread_mutex_lock(&tp->lock);
        tp->completed++;
        pthread_mutex_unlock(&tp->lock);
    }
}

int threadpool_init(struct threadpool *tp, int worker_count, int queue_cap)
{
    memset(tp, 0, sizeof(*tp));
    if (worker_count < 1 || queue_cap < 1) {
        return -1;
    }
    tp->queue = calloc((size_t)queue_cap, sizeof(*tp->queue));
    tp->workers = calloc((size_t)worker_count, sizeof(*tp->workers));
    if (!tp->queue || !tp->workers) {
        free(tp->queue);
        free(tp->workers);
        return -1;
    }
    tp->queue_cap = queue_cap;
    pthread_mutex_init(&tp->lock, NULL);
    pthread_cond_init(&tp->not_empty, NULL);

    for (int i = 0; i < worker_count; i++) {
        if (pthread_create(&tp->workers[i], NULL, worker_main, tp) != 0) {
            tp->worker_count = i; /* 만든 것까지만 정리한다 */
            threadpool_shutdown(tp);
            return -1;
        }
    }
    tp->worker_count = worker_count;
    return 0;
}

int threadpool_submit(struct threadpool *tp, threadpool_fn fn, void *arg)
{
    int rc = 0;

    pthread_mutex_lock(&tp->lock);
    if (tp->is_shutting_down || tp->count == tp->queue_cap) {
        tp->rejected++;
        rc = -1;
    } else {
        int tail = (tp->head + tp->count) % tp->queue_cap;
        tp->queue[tail].fn = fn;
        tp->queue[tail].arg = arg;
        tp->count++;
        tp->submitted++;
        pthread_cond_signal(&tp->not_empty); /* 잠든 워커 하나만 깨운다 */
    }
    pthread_mutex_unlock(&tp->lock);
    return rc;
}

void threadpool_shutdown(struct threadpool *tp)
{
    pthread_mutex_lock(&tp->lock);
    tp->is_shutting_down = true;
    pthread_cond_broadcast(&tp->not_empty); /* 잠든 워커를 모두 깨워 종료를 알린다 */
    pthread_mutex_unlock(&tp->lock);

    for (int i = 0; i < tp->worker_count; i++) {
        pthread_join(tp->workers[i], NULL);
    }
    pthread_cond_destroy(&tp->not_empty);
    pthread_mutex_destroy(&tp->lock);
    free(tp->queue);
    free(tp->workers);
    tp->queue = NULL;
    tp->workers = NULL;
    tp->worker_count = 0;
}
