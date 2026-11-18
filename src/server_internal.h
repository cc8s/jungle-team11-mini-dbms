#ifndef SERVER_INTERNAL_H
#define SERVER_INTERNAL_H

/* Server 의 실제 구조. server.c 와 테스트만 include 한다 (다른 모듈은 server.h 의 불투명 Server 만).
 * 테스트가 "워커가 연결을 잡았다", "큐에 하나가 기다린다" 같은 상태를 시간에 기대지 않고 기다릴 때 쓴다. */

#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <time.h>

#include "server.h"
#include "threadpool.h"

struct Server {
    Engine *engine;
    ServerOptions options;
    int listen_fd;
    int wake_pipe[2];   /* server_stop 이 [1] 에 쓰면 poll 중인 accept 루프가 [0] 으로 깨어난다 */
    ThreadPool *pool;
    atomic_bool is_stopping;
    /* /stats 카운터. 워커 여러 개와 accept 스레드가 동시에 올리므로 원자 변수 */
    atomic_llong requests_total; /* 워커가 응답한 요청 (상대가 먼저 끊은 연결 제외) */
    atomic_llong errors_total;   /* 그중 상태 코드 400 이상 */
    atomic_llong rejected_total; /* 큐가 가득 차 503 으로 돌려보낸 연결 */
    atomic_llong accepted_total; /* accept 한 연결 (테스트가 서버 상태를 기다릴 때 쓴다) */
    struct timespec started_at;  /* 만든 뒤 바뀌지 않음 */
    /* 시그널로 멈추기 (server_watch_signals). accept 루프는 ppoll 로 기다리는 동안에만 wait_mask 를
     * 적용해 SIGINT/SIGTERM 을 받고, EINTR 로 깨어나면 stop_flag 를 본다 */
    bool has_wait_mask;
    sigset_t wait_mask;
    volatile sig_atomic_t *stop_flag;
};

#endif
