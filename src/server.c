#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "http.h"
#include "json.h"
#include "threadpool.h"

#define DEFAULT_WORKER_COUNT 4
#define LISTEN_BACKLOG 128
#define JSON_CONTENT_TYPE "application/json"
#define RETRY_AFTER_HEADER "Retry-After: 1\r\n"
#define ALLOW_HEADER_SIZE 64
#define HEALTH_BODY "{\"ok\":true}"
#define POLL_LISTEN 0
#define POLL_WAKE 1
#define POLL_FD_COUNT 2
#define WAKE_BYTE 'x'
#define MS_PER_SECOND 1000
#define DRAIN_TIMEOUT_MS 200                    /* 닫기 전에 남은 입력을 기다리는 최대 시간 */
#define DRAIN_LIMIT (2 * HTTP_REQUEST_BUFFER_SIZE) /* 닫기 전에 버리는 최대 바이트 */
#define DRAIN_FALLBACK_SIZE 512
#define NS_PER_MS 1000000

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
    struct timespec started_at;  /* 만든 뒤 바뀌지 않음 */
};

/* 응답 하나를 보낼 곳: 카운터를 올릴 서버와 연결 fd */
typedef struct {
    Server *server;
    int fd;
} Connection;

/* ---- 응답 도우미 ---- */

typedef struct {
    int status;
    const char *code;
} StatusCode;

/* HTTP 계층 에러의 응답 코드 이름. 엔진 에러는 S37 에서 error_name 을 쓴다 */
static const StatusCode HTTP_ERROR_CODES[] = {
    {HTTP_STATUS_BAD_REQUEST, "BAD_REQUEST"},
    {HTTP_STATUS_NOT_FOUND, "NOT_FOUND"},
    {HTTP_STATUS_METHOD_NOT_ALLOWED, "METHOD_NOT_ALLOWED"},
    {HTTP_STATUS_REQUEST_TIMEOUT, "TIMEOUT"},
    {HTTP_STATUS_LENGTH_REQUIRED, "LENGTH_REQUIRED"},
    {HTTP_STATUS_PAYLOAD_TOO_LARGE, "PAYLOAD_TOO_LARGE"},
    {HTTP_STATUS_URI_TOO_LONG, "URI_TOO_LONG"},
    {HTTP_STATUS_HEADERS_TOO_LARGE, "HEADERS_TOO_LARGE"},
    {HTTP_STATUS_INTERNAL_ERROR, "INTERNAL"},
    {HTTP_STATUS_NOT_IMPLEMENTED, "NOT_IMPLEMENTED"},
    {HTTP_STATUS_SERVICE_UNAVAILABLE, "SERVER_BUSY"},
    {HTTP_STATUS_VERSION_NOT_SUPPORTED, "VERSION_NOT_SUPPORTED"},
};

static const char *http_error_code(int status)
{
    for (size_t i = 0; i < sizeof(HTTP_ERROR_CODES) / sizeof(HTTP_ERROR_CODES[0]); i++) {
        if (HTTP_ERROR_CODES[i].status == status)
            return HTTP_ERROR_CODES[i].code;
    }
    return "INTERNAL";
}

/* 모든 응답이 여기를 지나므로 카운터도 여기서 한 번만 올린다 */
static void count_response(Server *server, int status)
{
    if (status == HTTP_STATUS_SERVICE_UNAVAILABLE) {
        atomic_fetch_add(&server->rejected_total, 1);
        return;
    }
    atomic_fetch_add(&server->requests_total, 1);
    if (status >= HTTP_STATUS_BAD_REQUEST)
        atomic_fetch_add(&server->errors_total, 1);
}

static void send_json(const Connection *conn, int status, const char *body, size_t body_len,
                      const char *extra_headers)
{
    count_response(conn->server, status);
    HttpResponse response = {status, JSON_CONTENT_TYPE, extra_headers, body, body_len};
    http_write_response(conn->fd, &response); /* 상대가 끊었으면 실패해도 할 일이 없다 */
}

/* {"ok":false,"error":{"code":...,"message":...}} 를 보낸다. 메모리가 모자라면 본문 없이 보낸다 */
static void send_http_error(const Connection *conn, int status, const char *message,
                            const char *extra_headers)
{
    JsonBuf buf;
    json_buf_init(&buf);
    const char *code = http_error_code(status);
    json_buf_append_text(&buf, "{\"ok\":false,\"error\":{\"code\":");
    json_buf_append_escaped(&buf, code, strlen(code));
    json_buf_append_text(&buf, ",\"message\":");
    json_buf_append_escaped(&buf, message, strlen(message));
    json_buf_append_text(&buf, "}}");
    if (buf.is_out_of_memory)
        send_json(conn, status, NULL, 0, extra_headers);
    else
        send_json(conn, status, buf.data, buf.len, extra_headers);
    json_buf_free(&buf);
}

/* ---- 라우팅 ---- */

typedef void (*RouteHandler)(const Connection *conn, const HttpRequest *request);

typedef struct {
    const char *method;
    const char *path;
    RouteHandler handler;
} Route;

static void handle_health(const Connection *conn, const HttpRequest *request)
{
    (void)request;
    send_json(conn, HTTP_STATUS_OK, HEALTH_BODY, strlen(HEALTH_BODY), NULL);
}

/* ---- /query ---- */

typedef struct {
    ErrorCode code;
    int status;
} ErrorStatus;

/* 엔진 에러 → HTTP 상태 (계획서 5.3). 요청이 잘못된 것은 400, 없는 테이블은 404, 너무 큰 것은 413,
 * 서버 쪽 문제 (파일, 손상, 메모리) 는 500. 빠진 코드가 없는지 E37-01 이 모든 코드를 순회해 확인한다 */
static const ErrorStatus ERROR_STATUSES[] = {
    {ERR_NONE, HTTP_STATUS_OK},
    {ERR_SYNTAX, HTTP_STATUS_BAD_REQUEST},
    {ERR_UNKNOWN_TABLE, HTTP_STATUS_NOT_FOUND},
    {ERR_UNKNOWN_COLUMN, HTTP_STATUS_BAD_REQUEST},
    {ERR_TYPE_MISMATCH, HTTP_STATUS_BAD_REQUEST},
    {ERR_VALUE_COUNT, HTTP_STATUS_BAD_REQUEST},
    {ERR_VALUE_TOO_LONG, HTTP_STATUS_BAD_REQUEST},
    {ERR_OUT_OF_RANGE, HTTP_STATUS_BAD_REQUEST},
    {ERR_AUTO_ID, HTTP_STATUS_BAD_REQUEST},
    {ERR_UNSUPPORTED, HTTP_STATUS_BAD_REQUEST},
    {ERR_PARAM, HTTP_STATUS_BAD_REQUEST},
    {ERR_TOO_LARGE, HTTP_STATUS_PAYLOAD_TOO_LARGE},
    {ERR_IO, HTTP_STATUS_INTERNAL_ERROR},
    {ERR_CORRUPT, HTTP_STATUS_INTERNAL_ERROR},
    {ERR_INTERNAL, HTTP_STATUS_INTERNAL_ERROR},
};

int server_status_for_error(ErrorCode code)
{
    for (size_t i = 0; i < sizeof(ERROR_STATUSES) / sizeof(ERROR_STATUSES[0]); i++) {
        if (ERROR_STATUSES[i].code == code)
            return ERROR_STATUSES[i].status;
    }
    return HTTP_STATUS_INTERNAL_ERROR;
}

/* JSON 요청 파싱 실패를 응답으로. params 오류는 엔진의 ERR_PARAM 과 같은 코드 이름 ("PARAM") 을 쓴다 */
static void send_json_error(const Connection *conn, JsonStatus status, const char *message)
{
    if (status == JSON_BAD_PARAM) {
        Result result = result_error(ERR_PARAM, 0, "%s", message);
        JsonBuf buf;
        json_buf_init(&buf);
        if (json_write_result(&result, 0, &buf) == ERR_NONE)
            send_json(conn, HTTP_STATUS_BAD_REQUEST, buf.data, buf.len, NULL);
        else
            send_json(conn, HTTP_STATUS_INTERNAL_ERROR, NULL, 0, NULL);
        json_buf_free(&buf);
        return;
    }
    int http_status =
        status == JSON_NO_MEMORY ? HTTP_STATUS_INTERNAL_ERROR : HTTP_STATUS_BAD_REQUEST;
    send_http_error(conn, http_status, message, NULL);
}

/* 본문 JSON → engine_execute (실행 단계만 엔진 락 안) → Result → JSON 응답.
 * JSON 파싱과 응답 만들기는 락 밖이라 다른 워커의 실행을 막지 않는다 */
static void handle_query(const Connection *conn, const HttpRequest *request)
{
    Server *server = conn->server;
    QueryRequest query;
    char message[MAX_MESSAGE];
    JsonStatus status = json_parse_query(request->body, request->body_len, &query, message);
    if (status != JSON_OK) {
        send_json_error(conn, status, message);
        return;
    }
    Result result = engine_execute(server->engine, query.sql, query.params, query.param_count);
    query_request_free(&query);
    JsonBuf buf;
    json_buf_init(&buf);
    if (json_write_result(&result, server->options.max_rows, &buf) == ERR_NONE)
        send_json(conn, server_status_for_error(result.error), buf.data, buf.len, NULL);
    else
        send_http_error(conn, HTTP_STATUS_INTERNAL_ERROR, "out of memory", NULL);
    json_buf_free(&buf);
    result_free(&result);
}

/* ---- /tables, /stats ---- */

static const char *column_type_name(ColumnType type)
{
    return type == COL_INT ? "INT" : "VARCHAR";
}

/* {"name":"users","columns":[{"name":"id","type":"INT","size":4},...],"row_count":3}
 * 스키마는 락 없이, 바뀌는 row_count 는 engine_record_count (엔진 락) 로 읽는다 */
static void append_table(JsonBuf *buf, Engine *engine, const Table *table)
{
    json_buf_append_text(buf, "{\"name\":");
    json_buf_append_escaped(buf, table_name(table), strlen(table_name(table)));
    json_buf_append_text(buf, ",\"columns\":[");
    for (int col = 0; col < table_column_count(table); col++) {
        const ColumnDef *column = table_column(table, col);
        json_buf_append_text(buf, col == 0 ? "{\"name\":" : ",{\"name\":");
        json_buf_append_escaped(buf, column->name, strlen(column->name));
        json_buf_append_text(buf, ",\"type\":\"");
        json_buf_append_text(buf, column_type_name(column->type));
        json_buf_append_text(buf, "\",\"size\":");
        json_buf_append_int(buf, column->size);
        json_buf_append_text(buf, "}");
    }
    json_buf_append_text(buf, "],\"row_count\":");
    json_buf_append_int(buf, engine_record_count(engine, table));
    json_buf_append_text(buf, "}");
}

static void send_buf(const Connection *conn, JsonBuf *buf)
{
    if (buf->is_out_of_memory)
        send_http_error(conn, HTTP_STATUS_INTERNAL_ERROR, "out of memory", NULL);
    else
        send_json(conn, HTTP_STATUS_OK, buf->data, buf->len, NULL);
}

static void handle_tables(const Connection *conn, const HttpRequest *request)
{
    (void)request;
    Engine *engine = conn->server->engine;
    JsonBuf buf;
    json_buf_init(&buf);
    json_buf_append_text(&buf, "{\"ok\":true,\"tables\":[");
    for (int i = 0; i < engine_table_count(engine); i++) {
        if (i > 0)
            json_buf_append_text(&buf, ",");
        append_table(&buf, engine, engine_table_at(engine, i));
    }
    json_buf_append_text(&buf, "]}");
    send_buf(conn, &buf);
    json_buf_free(&buf);
}

static int64_t uptime_ms(const Server *server)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)(now.tv_sec - server->started_at.tv_sec) * MS_PER_SECOND +
           (now.tv_nsec - server->started_at.tv_nsec) / NS_PER_MS;
}

static void append_field(JsonBuf *buf, const char *name, int64_t value)
{
    json_buf_append_text(buf, ",\"");
    json_buf_append_text(buf, name);
    json_buf_append_text(buf, "\":");
    json_buf_append_int(buf, value);
}

/* 카운터는 원자 변수, 큐 길이는 풀의 뮤텍스를 잡고 읽는다. 값들은 각각 그 순간의 값이다
 * (한 번에 찍은 스냅샷은 아니다: 모니터링 용도로는 충분하고, 전부 묶으려면 락이 더 필요하다) */
static void handle_stats(const Connection *conn, const HttpRequest *request)
{
    (void)request;
    Server *server = conn->server;
    JsonBuf buf;
    json_buf_init(&buf);
    json_buf_append_text(&buf, "{\"ok\":true");
    append_field(&buf, "requests_total", atomic_load(&server->requests_total));
    append_field(&buf, "errors_total", atomic_load(&server->errors_total));
    append_field(&buf, "rejected_total", atomic_load(&server->rejected_total));
    append_field(&buf, "queue_length", threadpool_queue_length(server->pool));
    append_field(&buf, "queue_capacity", server->options.queue_capacity);
    append_field(&buf, "workers", server->options.worker_count);
    json_buf_append_text(&buf, ",\"lock_mode\":");
    const char *lock_mode = engine_lock_mode_name();
    json_buf_append_escaped(&buf, lock_mode, strlen(lock_mode));
    append_field(&buf, "uptime_ms", uptime_ms(server));
    json_buf_append_text(&buf, "}");
    send_buf(conn, &buf);
    json_buf_free(&buf);
}

/* 라우팅 표. 경로·메서드 조합은 여기에만 둔다 */
static const Route ROUTES[] = {
    {"GET", "/health", handle_health},
    {"POST", "/query", handle_query},
    {"GET", "/tables", handle_tables},
    {"GET", "/stats", handle_stats},
};

#define ROUTE_COUNT (sizeof(ROUTES) / sizeof(ROUTES[0]))

/* 경로는 있는데 메서드가 다르면 405 + 그 경로가 받는 메서드 목록 (Allow), 경로가 없으면 404 */
static void route_request(const Connection *conn, const HttpRequest *request)
{
    char allow[ALLOW_HEADER_SIZE] = "Allow: ";
    bool is_path_known = false;
    for (size_t i = 0; i < ROUTE_COUNT; i++) {
        if (strcmp(ROUTES[i].path, request->path) != 0)
            continue;
        if (strcmp(ROUTES[i].method, request->method) == 0) {
            ROUTES[i].handler(conn, request);
            return;
        }
        if (is_path_known)
            strncat(allow, ", ", sizeof(allow) - strlen(allow) - 1);
        strncat(allow, ROUTES[i].method, sizeof(allow) - strlen(allow) - 1);
        is_path_known = true;
    }
    if (!is_path_known) {
        send_http_error(conn, HTTP_STATUS_NOT_FOUND, "no such endpoint", NULL);
        return;
    }
    strncat(allow, "\r\n", sizeof(allow) - strlen(allow) - 1);
    send_http_error(conn, HTTP_STATUS_METHOD_NOT_ALLOWED, "method not allowed for this endpoint",
                    allow);
}

/* ---- 워커: 연결 하나 처리 ---- */

/* 읽지 않은 요청 바이트가 남은 채로 close 하면 커널이 FIN 대신 RST 를 보내서, 상대가 아직 읽지 않은
 * 응답까지 버려질 수 있다 (413 처럼 본문을 받기 전에 거절한 경우). 그래서 쓰기 쪽을 먼저 닫아 (FIN)
 * 응답을 확실히 보낸 뒤, 상대가 보낸 나머지를 잠깐 읽어 버리고 닫는다 (lingering close).
 * 시간과 양을 묶어서 느린 클라이언트가 워커를 오래 붙잡지 못하게 한다 */
static void close_after_draining(int fd, char *scratch, size_t scratch_size)
{
    shutdown(fd, SHUT_WR);
    http_set_read_timeout(fd, DRAIN_TIMEOUT_MS);
    size_t drained = 0;
    while (drained < DRAIN_LIMIT) {
        ssize_t n = recv(fd, scratch, scratch_size, 0);
        if (n <= 0)
            break; /* 상대가 닫음, 타임아웃, 에러 */
        drained += (size_t)n;
    }
    close(fd);
}

/* 지금 소켓에 읽지 않은 바이트가 있는지 (기다리지 않고 들여다보기만 한다) */
static bool has_pending_input(int fd)
{
    char byte;
    return recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT) > 0;
}

/* 요청을 읽고 (타임아웃), 파싱 에러면 그 상태 코드로, 아니면 라우팅해서 응답한다.
 * 요청을 끝까지 읽었으면 true (닫을 때 남은 입력을 비울 필요가 없을 가능성이 크다) */
static bool serve_request(const Connection *conn, char *buf, HttpStatus *out_status)
{
    *out_status = HTTP_STATUS_INTERNAL_ERROR;
    if (buf == NULL) {
        send_http_error(conn, HTTP_STATUS_INTERNAL_ERROR, "out of memory", NULL);
        return false;
    }
    if (!http_set_read_timeout(conn->fd, conn->server->options.read_timeout_ms)) {
        send_http_error(conn, HTTP_STATUS_INTERNAL_ERROR, "cannot set socket timeout", NULL);
        return false;
    }
    HttpRequest request;
    *out_status = http_read_request(conn->fd, buf, HTTP_REQUEST_BUFFER_SIZE, &request);
    if (*out_status == HTTP_PARSE_OK) {
        route_request(conn, &request);
        return true;
    }
    if (*out_status != HTTP_READ_CLOSED) /* 상대가 이미 끊었으면 응답하지 않는다 */
        send_http_error(conn, (int)*out_status, "invalid HTTP request", NULL);
    return false;
}

static void handle_connection(int fd, void *ctx)
{
    Connection conn = {ctx, fd};
    char *buf = malloc(HTTP_REQUEST_BUFFER_SIZE); /* 72KB: 워커 스택 대신 힙 */
    HttpStatus status = HTTP_PARSE_OK;
    bool is_consumed = serve_request(&conn, buf, &status);
    bool needs_drain = status != HTTP_READ_CLOSED && (!is_consumed || has_pending_input(fd));
    char fallback[DRAIN_FALLBACK_SIZE];
    if (needs_drain && buf != NULL)
        close_after_draining(fd, buf, HTTP_REQUEST_BUFFER_SIZE);
    else if (needs_drain)
        close_after_draining(fd, fallback, sizeof(fallback));
    else
        close(fd);
    free(buf);
}

/* ---- 생성 ---- */

ServerOptions server_default_options(void)
{
    return (ServerOptions){
        .port = 0,
        .worker_count = DEFAULT_WORKER_COUNT,
        .queue_capacity = DEFAULT_WORKER_COUNT * SERVER_DEFAULT_QUEUE_PER_WORKER,
        .max_rows = JSON_DEFAULT_MAX_ROWS,
        .read_timeout_ms = HTTP_READ_TIMEOUT_MS,
    };
}

static bool is_valid_options(const ServerOptions *options)
{
    return options->port >= 0 && options->port <= SERVER_MAX_PORT &&
           options->worker_count > 0 && options->queue_capacity > 0 && options->max_rows >= 0 &&
           options->read_timeout_ms > 0;
}

/* SO_REUSEADDR: 재시작 직후 TIME_WAIT 로 남은 포트에도 다시 bind 할 수 있게 한다
 * (다른 프로세스가 listen 중인 포트는 여전히 실패한다). accept 루프가 poll 뒤 accept 에서 막히지 않게
 * listen 소켓은 non-blocking 으로 둔다 (그 사이 연결이 사라지면 EAGAIN 으로 돌아온다) */
static int open_listen_socket(int port, char out_message[MAX_MESSAGE])
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        snprintf(out_message, MAX_MESSAGE, "socket: %s", strerror(errno));
        return -1;
    }
    int on = 1;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                                  .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0 ||
        bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, LISTEN_BACKLOG) != 0) {
        snprintf(out_message, MAX_MESSAGE, "cannot listen on port %d: %s", port,
                 strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static bool create_resources(Server *server, char out_message[MAX_MESSAGE])
{
    server->listen_fd = open_listen_socket(server->options.port, out_message);
    if (server->listen_fd < 0)
        return false;
    if (pipe2(server->wake_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        snprintf(out_message, MAX_MESSAGE, "pipe: %s", strerror(errno));
        return false;
    }
    server->pool = threadpool_create(server->options.worker_count,
                                     server->options.queue_capacity, handle_connection, server);
    if (server->pool == NULL) {
        snprintf(out_message, MAX_MESSAGE, "cannot start %d worker threads",
                 server->options.worker_count);
        return false;
    }
    return true;
}

Server *server_create(Engine *engine, const ServerOptions *options, char out_message[MAX_MESSAGE])
{
    out_message[0] = '\0';
    if (!is_valid_options(options)) {
        snprintf(out_message, MAX_MESSAGE, "invalid server options");
        return NULL;
    }
    Server *server = calloc(1, sizeof(*server));
    if (server == NULL) {
        snprintf(out_message, MAX_MESSAGE, "out of memory");
        return NULL;
    }
    server->engine = engine;
    server->options = *options;
    server->listen_fd = -1;
    server->wake_pipe[0] = server->wake_pipe[1] = -1;
    atomic_init(&server->is_stopping, false);
    atomic_init(&server->requests_total, 0);
    atomic_init(&server->errors_total, 0);
    atomic_init(&server->rejected_total, 0);
    clock_gettime(CLOCK_MONOTONIC, &server->started_at);
    if (create_resources(server, out_message))
        return server;
    server_destroy(server);
    return NULL;
}

int server_port(const Server *server)
{
    struct sockaddr_in address;
    socklen_t len = sizeof(address);
    if (getsockname(server->listen_fd, (struct sockaddr *)&address, &len) != 0)
        return -1;
    return ntohs(address.sin_port);
}

/* ---- accept 루프 ---- */

/* 이미 도착해 있는 입력만 기다리지 않고 버린다 (503 을 RST 없이 보내기 위한 최선) */
static void discard_available_input(int fd)
{
    char scratch[DRAIN_FALLBACK_SIZE];
    size_t drained = 0;
    while (drained < DRAIN_LIMIT) {
        ssize_t n = recv(fd, scratch, sizeof(scratch), MSG_DONTWAIT);
        if (n <= 0)
            return;
        drained += (size_t)n;
    }
}

/* 연결 하나를 받아 풀에 넣는다. 큐가 가득 차면 accept 스레드가 바로 503 을 쓰고 닫는다 (백프레셔).
 * 일시적 실패 (EINTR, EAGAIN, 연결이 그새 끊김) 는 조용히, 자원 부족 (EMFILE 등) 은 로그 후 계속 */
static void accept_one(Server *server)
{
    int fd = accept4(server->listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK && errno != ECONNABORTED)
            fprintf(stderr, "minidb: accept failed: %s\n", strerror(errno));
        return;
    }
    if (!threadpool_submit(server->pool, fd)) {
        Connection rejected = {server, fd};
        send_http_error(&rejected, HTTP_STATUS_SERVICE_UNAVAILABLE, "server is busy, retry later",
                        RETRY_AFTER_HEADER);
        discard_available_input(fd); /* accept 루프는 기다릴 수 없어서 이미 온 것만 비운다 */
        shutdown(fd, SHUT_WR);
        close(fd);
    }
}

/* listen 소켓과 깨우기 파이프를 같이 기다린다. accept 에 그냥 막혀 있으면 다른 스레드가 멈출 방법이
 * 없어서 (close 로 깨우는 것은 경쟁 상태가 있다) self-pipe 를 쓴다 */
void server_run(Server *server)
{
    struct pollfd fds[POLL_FD_COUNT] = {
        [POLL_LISTEN] = {.fd = server->listen_fd, .events = POLLIN},
        [POLL_WAKE] = {.fd = server->wake_pipe[0], .events = POLLIN},
    };
    while (!atomic_load(&server->is_stopping)) {
        if (poll(fds, POLL_FD_COUNT, -1) < 0) {
            if (errno != EINTR)
                fprintf(stderr, "minidb: poll failed: %s\n", strerror(errno));
            continue;
        }
        if (fds[POLL_WAKE].revents != 0)
            break;
        if (fds[POLL_LISTEN].revents & POLLIN)
            accept_one(server);
    }
    /* 종료 순서: 새 연결 거부 (listen 닫기) → 큐에 남은 요청 처리 후 워커 join */
    close(server->listen_fd);
    server->listen_fd = -1;
    threadpool_shutdown(server->pool);
    server->pool = NULL;
}

void server_stop(Server *server)
{
    atomic_store(&server->is_stopping, true);
    char wake = WAKE_BYTE;
    ssize_t written = write(server->wake_pipe[1], &wake, 1); /* 파이프가 이미 차 있으면 이미 깨어 있다 */
    (void)written;
}

void server_destroy(Server *server)
{
    if (server == NULL)
        return;
    threadpool_shutdown(server->pool); /* server_run 을 돌리지 않았을 때 */
    if (server->listen_fd >= 0)
        close(server->listen_fd);
    for (int i = 0; i < 2; i++) {
        if (server->wake_pipe[i] >= 0)
            close(server->wake_pipe[i]);
    }
    free(server);
}
