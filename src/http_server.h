#ifndef MINI_DBMS_HTTP_SERVER_H
#define MINI_DBMS_HTTP_SERVER_H

/* Phase 3 API 서버 (D005, D006).
 *
 *   ./minidb --server [-p port] [-w workers] [-q queue] [-s schema_dir] [-d data_dir] [-n]
 *
 *   POST /query  {"sql": "..."}  → JSON (api.h)
 *   GET  /health                 → {"ok":true}
 *
 * accept 하는 메인 스레드가 연결을 스레드 풀 큐에 넣는다. 큐가 가득 차면 그 자리에서 503 을 보내고 닫는다.
 * SIGINT/SIGTERM 을 받으면 새 연결을 그만 받고, 큐에 남은 요청을 끝낸 뒤 테이블을 flush 하고 끝낸다.
 * -p 0 이면 남는 포트를 골라 첫 줄에 "listening on port N" 으로 알린다 (테스트용). */

int server_main(int argc, char **argv);

#endif
