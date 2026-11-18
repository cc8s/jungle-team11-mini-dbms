#ifndef THREADPOOL_INTERNAL_H
#define THREADPOOL_INTERNAL_H

/* 스레드 풀의 실제 구조와 내부 큐. threadpool.c 와 테스트만 include 한다.
 * 큐를 워커 없이 따로 시험하고, 워커 없는 풀로 submit 의 동시성만 검사하기 위해 분리했다. */

#include <pthread.h>

#include "threadpool.h"

/* ---- 원형 큐 (스레드 안전하지 않음: ThreadPool.mutex 안에서만 쓴다) ----
 * head = 다음에 꺼낼 칸, tail = 다음에 넣을 칸, count = 들어 있는 개수.
 * head == tail 만으로는 "비었음" 과 "가득 참" 을 구분할 수 없어서 count 를 따로 둔다. */
typedef struct {
    int *items;
    int capacity;
    int head;
    int tail;
    int count;
} JobQueue;

/* capacity 칸짜리 빈 큐. capacity <= 0 이거나 메모리가 모자라면 NULL */
JobQueue *job_queue_create(int capacity);
void job_queue_destroy(JobQueue *queue);

/* 가득 차 있으면 false (큐 변경 없음) */
bool job_queue_push(JobQueue *queue, int value);
/* 비어 있으면 false (*out_value 건드리지 않음) */
bool job_queue_pop(JobQueue *queue, int *out_value);
int job_queue_count(const JobQueue *queue);

/* ---- 풀 ---- */

struct ThreadPool {
    JobQueue *queue;
    pthread_mutex_t mutex;    /* queue, is_shutting_down 을 보호 */
    pthread_cond_t not_empty; /* 큐에 항목이 생기거나 종료가 시작되면 신호 */
    bool is_shutting_down;
    pthread_t *workers;
    int worker_count;
    ThreadPoolHandler handler;
    void *ctx;
};

/* 워커를 만들지 않은 풀 (큐, 뮤텍스, 조건 변수만). threadpool_create 가 이 위에 워커를 붙이고,
 * 테스트는 이것만으로 submit 을 시험한다. queue_capacity <= 0 이면 NULL */
ThreadPool *threadpool_create_without_workers(int queue_capacity, ThreadPoolHandler handler,
                                              void *ctx);

#endif
