#ifndef MINI_DBMS_THREADPOOL_H
#define MINI_DBMS_THREADPOOL_H

/* 고정 크기 스레드 풀 + 크기가 정해진 작업 큐 (D005, 학습 노트 2 Q10~Q12).
 *
 * - 워커 N개를 시작할 때 만들어 두고 계속 쓴다. 요청마다 스레드를 만들지 않는다 (Q10).
 * - 일이 없으면 워커는 condition variable 에서 잠든다. CPU 를 쓰지 않는다 (Q11).
 * - 큐가 가득 차면 submit 은 기다리지 않고 바로 실패한다. HTTP 서버는 이를 503 으로 바꾼다 (Q12).
 * - 종료하면 이미 큐에 들어온 일은 끝까지 하고 워커를 모두 join 한다. */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

typedef void (*threadpool_fn)(void *arg);

struct threadpool_job {
    threadpool_fn fn;
    void *arg;
};

struct threadpool {
    pthread_mutex_t lock;     /* 아래 필드 전부 보호 */
    pthread_cond_t not_empty; /* 큐에 일이 생겼거나 종료가 시작됨 */
    struct threadpool_job *queue;
    int queue_cap;
    int head;  /* 다음에 꺼낼 칸 */
    int count; /* 큐에 있는 일 수 */
    bool is_shutting_down;

    pthread_t *workers;
    int worker_count;

    /* 통계 (테스트·로그용) */
    size_t submitted;
    size_t rejected;
    size_t completed;
};

/* 성공 0, 인자 오류나 자원 부족 -1 */
int threadpool_init(struct threadpool *tp, int worker_count, int queue_cap);

/* 일을 큐에 넣는다. 성공 0, 큐가 가득 찼거나 종료 중이면 -1 (기다리지 않는다) */
int threadpool_submit(struct threadpool *tp, threadpool_fn fn, void *arg);

/* 큐에 남은 일을 다 끝낸 뒤 워커를 join 하고 자원을 놓는다. 이후 tp 는 다시 init 해야 쓴다 */
void threadpool_shutdown(struct threadpool *tp);

#endif
