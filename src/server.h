#ifndef SERVER_H
#define SERVER_H

/* HTTP API 서버: listen → accept 루프 → 스레드 풀 → 라우팅 → 응답.
 * accept 루프 (server_run) 는 호출한 스레드에서 돌고, 요청 처리는 풀의 워커가 한다.
 * 테스트는 server_run 을 스레드로 띄우고 server_stop 으로 멈춘다. 엔진은 호출한 쪽이 열고 닫는다. */

#include <stdint.h>

#include "common.h"
#include "engine.h"

#define SERVER_DEFAULT_QUEUE_PER_WORKER 16 /* queue_capacity 를 안 주면 workers × 16 */
#define SERVER_MAX_PORT 65535

typedef struct {
    int port;            /* 0 이면 OS 가 빈 포트를 고른다 (server_port 로 조회) */
    int worker_count;
    int queue_capacity;
    int64_t max_rows;    /* SELECT 응답에 담는 행 수 상한 */
    int read_timeout_ms; /* 요청 읽기 타임아웃 */
} ServerOptions;

typedef struct Server Server;

/* options 의 기본값 (port 0, 워커 4, 큐 64, 행 상한 1,000, 타임아웃 5 초) */
ServerOptions server_default_options(void);

/* 0.0.0.0:port 에 bind·listen 하고 워커를 띄운다. 이미 쓰는 포트, 잘못된 옵션, 자원 부족이면 NULL 이고
 * out_message 에 이유를 쓴다. engine 은 서버보다 오래 살아야 한다 */
Server *server_create(Engine *engine, const ServerOptions *options, char out_message[MAX_MESSAGE]);

/* 실제로 bind 된 포트 */
int server_port(const Server *server);

/* server_stop 이 불릴 때까지 연결을 받는다. 멈추면 listen 소켓을 닫아 새 연결을 거부하고,
 * 큐에 남은 요청까지 처리한 뒤 (워커 join) 돌아온다. */
void server_run(Server *server);

/* 다른 스레드에서 accept 루프를 깨워 멈추게 한다. 여러 번 불러도 안전하다 */
void server_stop(Server *server);

/* server_run 이 돌아온 뒤 (또는 돌리지 않았으면 바로) 자원을 해제한다. NULL 이면 아무 일도 없다 */
void server_destroy(Server *server);

/* 엔진 에러 코드 → HTTP 상태 코드 (계획서 5.3). 모든 ErrorCode 가 표에 있다. 표 밖 값은 500 */
int server_status_for_error(ErrorCode code);

#endif
