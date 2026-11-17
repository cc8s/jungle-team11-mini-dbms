#ifndef THREADPOOL_H
#define THREADPOOL_H

/* 고정 크기 스레드 풀: 공유 유한 큐 하나 + 워커 N 개 (워커는 S31).
 * 생산자 (accept 루프) 는 submit 으로 연결 fd 를 넣고, 워커가 꺼내서 handler 를 부른다.
 * 상태는 모두 ThreadPool 안에 있고 전역 변수가 없다. */

#include <stdbool.h>

typedef struct ThreadPool ThreadPool;

/* 워커가 연결 하나를 처리하는 함수. connfd 를 닫는 것도 handler 의 몫이다 */
typedef void (*ThreadPoolHandler)(int connfd, void *ctx);

/* connfd 를 큐에 넣는다. 기다리지 않는다: 큐가 가득 찼거나 종료 중이면 바로 false
 * (호출한 쪽이 503 을 보내고 연결을 닫는다 — 백프레셔). 여러 스레드에서 동시에 불러도 안전하다. */
bool threadpool_submit(ThreadPool *pool, int connfd);

/* 종료 표시 → 워커를 모두 깨움 → join → 해제. NULL 이면 아무 일도 하지 않는다.
 * 큐에 남은 fd 는 워커가 처리한다 (S31). */
void threadpool_shutdown(ThreadPool *pool);

#endif
