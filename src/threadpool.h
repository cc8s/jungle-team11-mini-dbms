#ifndef THREADPOOL_H
#define THREADPOOL_H

/* 고정 크기 스레드 풀: 공유 유한 큐 하나 + 워커 N 개.
 * 생산자 (accept 루프) 는 submit 으로 연결 fd 를 넣고, 워커가 꺼내서 handler 를 부른다.
 * 상태는 모두 ThreadPool 안에 있고 전역 변수가 없다. */

#include <stdbool.h>

typedef struct ThreadPool ThreadPool;

/* 워커가 연결 하나를 처리하는 함수. connfd 를 닫는 것도 handler 의 몫이다 */
typedef void (*ThreadPoolHandler)(int connfd, void *ctx);

/* 워커 worker_count 개와 queue_capacity 칸짜리 큐를 가진 풀을 만든다. 워커는 큐에서 fd 를 꺼내
 * handler(connfd, ctx) 를 부른다 (락 밖에서, 여러 워커가 동시에). worker_count <= 0,
 * queue_capacity <= 0, handler 가 NULL, 메모리·스레드 생성 실패면 NULL (만든 워커는 정리한다). */
ThreadPool *threadpool_create(int worker_count, int queue_capacity, ThreadPoolHandler handler,
                              void *ctx);

/* connfd 를 큐에 넣는다. 기다리지 않는다: 큐가 가득 찼거나 종료 중이면 바로 false
 * (호출한 쪽이 503 을 보내고 연결을 닫는다 — 백프레셔). 여러 스레드에서 동시에 불러도 안전하다. */
bool threadpool_submit(ThreadPool *pool, int connfd);

/* 지금 큐에서 기다리는 연결 수 (/stats 용). 뮤텍스를 잡고 읽는다 */
int threadpool_queue_length(ThreadPool *pool);

/* 종료 표시 → 워커를 모두 깨움 → join → 해제. NULL 이면 아무 일도 하지 않는다.
 * 큐에 이미 들어온 fd 는 워커가 모두 처리한 뒤에 돌아온다. 종료가 시작된 뒤의 submit 은 false.
 * shutdown 이 진행 중일 때 (워커 join 을 기다리는 동안) 다른 스레드의 submit 은 false 를 받는다.
 * shutdown 이 돌아온 뒤에는 pool 이 해제되었으므로 쓰면 안 된다 (서버는 accept 를 먼저 멈춘다). */
void threadpool_shutdown(ThreadPool *pool);

#endif
