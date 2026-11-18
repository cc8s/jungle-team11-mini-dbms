#include "test.h"
#include "test_util.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cli.h"
#include "server.h"
#include "server_internal.h"
#include "table_internal.h"
#include "threadpool.h"

#define LOCALHOST "127.0.0.1"
#define RESPONSE_CAPACITY 65536
#define SEQUENTIAL_CLIENTS 10
#define FLOOD_CLIENTS 10
#define QUICK_CLOSE_CLIENTS 100
#define HELD_TIMEOUT_MS 1000 /* 503 테스트: 붙잡힌 연결이 타임아웃되기 전에 나머지 연결이 끝나면 된다 */
#define HEALTH_REQUEST "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"
#define DEFAULT_TEST_WORKERS 4 /* server_default_options 의 값 */
#define DEFAULT_TEST_QUEUE 64

/* 테스트 하나가 띄운 서버: 엔진 + 서버 + accept 루프 스레드 */
typedef struct {
    TestDb db;
    Server *server;
    pthread_t thread;
    int port;
} TestServer;

static void *run_server(void *arg)
{
    server_run(((TestServer *)arg)->server);
    return NULL;
}

#define USERS_SQL "CREATE TABLE users (id INT, name VARCHAR(32), age INT);"

/* create_sql 의 테이블들을 만든 데이터 디렉터리로 서버를 띄운다 (NULL 이면 테이블 없음) */
static void start_server_on(TestServer *ts, const ServerOptions *options, const char *create_sql)
{
    ts->db = test_db_create(create_sql);
    CHECK(ts->db.engine != NULL);
    char message[MAX_MESSAGE];
    ts->server = server_create(ts->db.engine, options, message);
    CHECK(ts->server != NULL);
    ts->port = server_port(ts->server);
    CHECK(pthread_create(&ts->thread, NULL, run_server, ts) == 0);
}

static void start_server_with(TestServer *ts, const ServerOptions *options)
{
    start_server_on(ts, options, NULL);
}

static void start_server(TestServer *ts)
{
    ServerOptions options = server_default_options();
    start_server_with(ts, &options);
}

static void stop_server(TestServer *ts)
{
    server_stop(ts->server);
    CHECK(pthread_join(ts->thread, NULL) == 0);
    server_destroy(ts->server);
    test_db_destroy(&ts->db);
}

static int connect_to(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    CHECK(inet_pton(AF_INET, LOCALHOST, &address.sin_addr) == 1);
    CHECK(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    return fd;
}

/* 받은 응답 (서버가 닫을 때까지) */
typedef struct {
    int status;
    char text[RESPONSE_CAPACITY];
    const char *body;
} Response;

static void read_response(int fd, Response *out)
{
    size_t len = 0;
    for (;;) {
        ssize_t n = read(fd, out->text + len, sizeof(out->text) - 1 - len);
        CHECK(n >= 0);
        if (n == 0)
            break;
        len += (size_t)n;
    }
    out->text[len] = '\0';
    CHECK(sscanf(out->text, "HTTP/1.1 %d", &out->status) == 1);
    const char *separator = strstr(out->text, "\r\n\r\n");
    CHECK(separator != NULL);
    out->body = separator + 4;
}

static void send_request(int port, const char *request, Response *out)
{
    int fd = connect_to(port);
    CHECK(write(fd, request, strlen(request)) == (ssize_t)strlen(request));
    read_response(fd, out);
    close(fd);
}

static int count_open_fds(void)
{
    DIR *dir = opendir("/proc/self/fd");
    CHECK(dir != NULL);
    int count = 0;
    while (readdir(dir) != NULL)
        count++;
    closedir(dir);
    return count;
}

/* T36-01 */
static void test_port_zero_picks_port(void)
{
    TestServer ts;
    start_server(&ts);
    CHECK(ts.port > 0 && ts.port <= SERVER_MAX_PORT);
    stop_server(&ts);
}

/* T36-02 */
static void test_health(void)
{
    TestServer ts;
    start_server(&ts);
    Response response;
    send_request(ts.port, HEALTH_REQUEST, &response);
    CHECK(response.status == 200);
    CHECK_STR_EQ(response.body, "{\"ok\":true}");
    CHECK(strstr(response.text, "Content-Type: application/json\r\n") != NULL);
    CHECK(strstr(response.text, "Connection: close\r\n") != NULL);
    stop_server(&ts);
}

/* T36-03 */
static void test_sequential_clients(void)
{
    TestServer ts;
    start_server(&ts);
    for (int i = 0; i < SEQUENTIAL_CLIENTS; i++) {
        Response response;
        send_request(ts.port, HEALTH_REQUEST, &response);
        CHECK(response.status == 200);
    }
    stop_server(&ts);
}

/* E36-01 */
static void test_unknown_path(void)
{
    TestServer ts;
    start_server(&ts);
    Response response;
    send_request(ts.port, "GET /nope HTTP/1.1\r\n\r\n", &response);
    CHECK(response.status == 404);
    const char *expected = "{\"ok\":false,\"error\":{\"code\":\"NOT_FOUND\"";
    CHECK(strncmp(response.body, expected, strlen(expected)) == 0);
    stop_server(&ts);
}

/* E36-02 */
static void test_wrong_method(void)
{
    TestServer ts;
    start_server(&ts);
    Response response;
    send_request(ts.port, "DELETE /health HTTP/1.1\r\n\r\n", &response);
    CHECK(response.status == 405);
    CHECK(strstr(response.text, "Allow: GET\r\n") != NULL);
    CHECK(strstr(response.body, "\"code\":\"METHOD_NOT_ALLOWED\"") != NULL);
    send_request(ts.port, "GET /query HTTP/1.1\r\n\r\n", &response);
    CHECK(response.status == 405);
    CHECK(strstr(response.text, "Allow: POST\r\n") != NULL);
    stop_server(&ts);
}

/* HTTP 파싱 에러도 JSON 에러 본문과 상태 코드로 나간다 */
static void test_bad_http_request(void)
{
    TestServer ts;
    start_server(&ts);
    Response response;
    send_request(ts.port, "GET /health HTTP/2.0\r\n\r\n", &response);
    CHECK(response.status == 505);
    send_request(ts.port, "POST /query HTTP/1.1\r\n\r\n", &response);
    CHECK(response.status == 411);
    CHECK(strstr(response.body, "\"code\":\"LENGTH_REQUIRED\"") != NULL);
    send_request(ts.port, "garbage\r\n\r\n", &response);
    CHECK(response.status == 400);
    CHECK(strstr(response.body, "\"code\":\"BAD_REQUEST\"") != NULL);
    stop_server(&ts);
}

/* E36-03: 워커 1, 큐 1. 아무것도 보내지 않는 연결로 둘 다 막으면 나머지는 503.
 * 붙잡힌 연결은 타임아웃 뒤 408 을 받는다 */
static void test_overload_returns_503(void)
{
    ServerOptions options = server_default_options();
    options.worker_count = 1;
    options.queue_capacity = 1;
    options.read_timeout_ms = HELD_TIMEOUT_MS;
    TestServer ts;
    start_server_with(&ts, &options);
    int fds[FLOOD_CLIENTS];
    for (int i = 0; i < FLOOD_CLIENTS; i++)
        fds[i] = connect_to(ts.port);
    int busy = 0;
    int timed_out = 0;
    for (int i = 0; i < FLOOD_CLIENTS; i++) {
        Response response;
        read_response(fds[i], &response);
        close(fds[i]);
        if (response.status == 503) {
            busy++;
            CHECK(strstr(response.text, "Retry-After: 1\r\n") != NULL);
            CHECK(strstr(response.body, "\"code\":\"SERVER_BUSY\"") != NULL);
        }
        timed_out += response.status == 408;
    }
    /* 동시에 붙잡을 수 있는 건 많아야 워커 1 + 큐 1. 워커가 첫 연결을 꺼내 가기 전에 accept 가 나머지를
     * 다 처리하면 큐 1 칸만 남아 9 개가 503 이 된다 (스케줄링에 따라 8 또는 9) */
    CHECK(busy >= FLOOD_CLIENTS - 2);
    CHECK(busy + timed_out == FLOOD_CLIENTS);
    stop_server(&ts);
}

/* E36-04 */
static void test_port_in_use(void)
{
    TestServer ts;
    start_server(&ts);
    ServerOptions options = server_default_options();
    options.port = ts.port;
    char message[MAX_MESSAGE];
    CHECK(server_create(ts.db.engine, &options, message) == NULL);
    CHECK(message[0] != '\0');
    Response response; /* 먼저 띄운 서버는 멀쩡하다 */
    send_request(ts.port, HEALTH_REQUEST, &response);
    CHECK(response.status == 200);
    stop_server(&ts);
}

/* E36-05: 워커 1 개라 마지막 /health 응답이 오면 앞의 연결은 모두 처리 (닫힘) 된 뒤다 */
static void test_quick_disconnects(void)
{
    ServerOptions options = server_default_options();
    options.worker_count = 1;
    options.queue_capacity = QUICK_CLOSE_CLIENTS * 2;
    TestServer ts;
    start_server_with(&ts, &options);
    Response response;
    send_request(ts.port, HEALTH_REQUEST, &response); /* 첫 연결로 지연 할당 등을 끝내 둔다 */
    int fds_before = count_open_fds();
    for (int i = 0; i < QUICK_CLOSE_CLIENTS; i++)
        close(connect_to(ts.port));
    send_request(ts.port, HEALTH_REQUEST, &response);
    CHECK(response.status == 200);
    CHECK(count_open_fds() == fds_before);
    stop_server(&ts);
}

/* 잘못된 옵션은 시작하지 않는다 */
static void test_invalid_options(void)
{
    TestDb db = test_db_create(NULL);
    CHECK(db.engine != NULL);
    ServerOptions options = server_default_options();
    char message[MAX_MESSAGE];
    options.worker_count = 0;
    CHECK(server_create(db.engine, &options, message) == NULL);
    options = server_default_options();
    options.port = SERVER_MAX_PORT + 1;
    CHECK(server_create(db.engine, &options, message) == NULL);
    server_destroy(NULL);
    test_db_destroy(&db);
}

/* E36-06 */
static void test_serve_usage_errors(void)
{
    char *cases[][5] = {
        {"minidb", "serve", "abc", "4", NULL},   {"minidb", "serve", "70000", "4", NULL},
        {"minidb", "serve", "8080", "0", NULL},  {"minidb", "serve", "8080", NULL, NULL},
        {"minidb", "serve", "8080", "4", "0"},   {"minidb", "serve", "-1", "4", NULL},
        {"minidb", "serve", "80x", "4", NULL},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int argc = 0;
        while (argc < 5 && cases[i][argc] != NULL)
            argc++;
        TestCapture out;
        TestCapture err;
        test_capture_open(&out);
        test_capture_open(&err);
        CHECK(cli_run(argc, cases[i], out.stream, err.stream) == CLI_EXIT_USAGE);
        char *out_text = test_capture_close(&out);
        char *err_text = test_capture_close(&err);
        CHECK(strstr(err_text, "usage") != NULL);
        free(out_text);
        free(err_text);
    }
}

/* ---- S37 /query ---- */

#define CONCURRENT_CLIENTS 8
#define INSERTS_PER_CLIENT 100
#define BIG_SQL_LEN 70000
#define REQUEST_HEAD_SIZE 256

static void start_users_server(TestServer *ts)
{
    ServerOptions options = server_default_options();
    start_server_on(ts, &options, USERS_SQL);
}

/* POST /query 에 body 를 보낸다 */
static void post_query(int port, const char *body, Response *out)
{
    size_t body_len = strlen(body);
    char head[REQUEST_HEAD_SIZE];
    int head_len = snprintf(head, sizeof(head),
                            "POST /query HTTP/1.1\r\nContent-Type: application/json\r\n"
                            "Content-Length: %zu\r\n\r\n",
                            body_len);
    int fd = connect_to(port);
    CHECK(write(fd, head, (size_t)head_len) == head_len);
    CHECK(write(fd, body, body_len) == (ssize_t)body_len);
    read_response(fd, out);
    close(fd);
}

static int32_t insert_id_of(const Response *response)
{
    const char *field = strstr(response->body, "\"last_insert_id\":");
    CHECK(field != NULL);
    int id = 0;
    CHECK(sscanf(field, "\"last_insert_id\":%d", &id) == 1);
    return id;
}

/* T37-01 */
static void test_query_insert(void)
{
    TestServer ts;
    start_users_server(&ts);
    Response response;
    post_query(ts.port, "{\"sql\":\"INSERT INTO users VALUES ('kim', 25)\"}", &response);
    CHECK(response.status == 200);
    CHECK(strstr(response.body, "{\"ok\":true,\"affected_rows\":1,\"last_insert_id\":1,") ==
          response.body);
    stop_server(&ts);
}

/* T37-02, T37-03 */
static void test_query_select_after_insert(void)
{
    TestServer ts;
    start_users_server(&ts);
    Response response;
    post_query(ts.port, "{\"sql\":\"INSERT INTO users VALUES ('kim', 25)\"}", &response);
    post_query(ts.port, "{\"sql\":\"INSERT INTO users VALUES ('lee', 30)\"}", &response);
    post_query(ts.port, "{\"sql\":\"SELECT * FROM users WHERE id = 1\"}", &response);
    CHECK(response.status == 200);
    CHECK(strstr(response.body, "\"rows\":[[1,\"kim\",25]],\"row_count\":1") != NULL);
    CHECK(strstr(response.body, "\"used_index\":true") != NULL);
    post_query(ts.port, "{\"sql\":\"SELECT name FROM users WHERE age > 20\"}", &response);
    CHECK(strstr(response.body, "\"rows\":[[\"kim\"],[\"lee\"]]") != NULL);
    CHECK(strstr(response.body, "\"used_index\":false") != NULL);
    stop_server(&ts);
}

/* T37-04, T37-05, E37-02 */
static void test_query_engine_errors(void)
{
    TestServer ts;
    start_users_server(&ts);
    Response response;
    post_query(ts.port, "{\"sql\":\"SELEC * FROM users\"}", &response);
    CHECK(response.status == 400);
    CHECK(strstr(response.body, "\"code\":\"SYNTAX\"") != NULL);
    post_query(ts.port, "{\"sql\":\"SELECT * FROM nope\"}", &response);
    CHECK(response.status == 404);
    CHECK(strstr(response.body, "\"code\":\"UNKNOWN_TABLE\"") != NULL);
    post_query(ts.port, "{\"sql\":\"\"}", &response); /* E37-02 */
    CHECK(response.status == 400);
    CHECK(strstr(response.body, "\"code\":\"SYNTAX\"") != NULL);
    post_query(ts.port, "{\"sql\":\"SELECT nmae FROM users\"}", &response);
    CHECK(response.status == 400);
    CHECK(strstr(response.body, "\"code\":\"UNKNOWN_COLUMN\"") != NULL);
    stop_server(&ts);
}

/* JSON 계층 에러: 깨진 JSON 은 BAD_REQUEST, 잘못된 params 는 PARAM (둘 다 400) */
static void test_query_json_errors(void)
{
    TestServer ts;
    start_users_server(&ts);
    Response response;
    post_query(ts.port, "{\"sql\":", &response);
    CHECK(response.status == 400);
    CHECK(strstr(response.body, "\"code\":\"BAD_REQUEST\"") != NULL);
    post_query(ts.port, "{\"sql\":\"SELECT * FROM users\",\"params\":[1.5]}", &response);
    CHECK(response.status == 400);
    CHECK(strstr(response.body, "\"code\":\"PARAM\"") != NULL);
    stop_server(&ts);
}

/* E37-01 */
static void test_error_status_table(void)
{
    const int expected[] = {
        [ERR_NONE] = 200,         [ERR_SYNTAX] = 400,         [ERR_UNKNOWN_TABLE] = 404,
        [ERR_UNKNOWN_COLUMN] = 400, [ERR_TYPE_MISMATCH] = 400, [ERR_VALUE_COUNT] = 400,
        [ERR_VALUE_TOO_LONG] = 400, [ERR_OUT_OF_RANGE] = 400,  [ERR_AUTO_ID] = 400,
        [ERR_UNSUPPORTED] = 400,  [ERR_PARAM] = 400,          [ERR_TOO_LARGE] = 413,
        [ERR_IO] = 500,           [ERR_CORRUPT] = 500,        [ERR_INTERNAL] = 500,
    };
    _Static_assert(sizeof(expected) / sizeof(expected[0]) == ERR_INTERNAL + 1,
                   "every ErrorCode needs an expected status");
    for (int code = ERR_NONE; code <= ERR_INTERNAL; code++)
        CHECK(server_status_for_error((ErrorCode)code) == expected[code]);
    CHECK(server_status_for_error((ErrorCode)(ERR_INTERNAL + 1)) == 500);
}

/* E37-03: SQL 이 64KB 를 넘으면 본문도 넘으므로 본문을 받기 전에 413 */
static void test_query_too_large(void)
{
    TestServer ts;
    start_users_server(&ts);
    char *body = malloc(BIG_SQL_LEN + 32);
    CHECK(body != NULL);
    int prefix = snprintf(body, 32, "{\"sql\":\"");
    memset(body + prefix, 'x', BIG_SQL_LEN);
    strcpy(body + prefix + BIG_SQL_LEN, "\"}");
    Response response;
    post_query(ts.port, body, &response);
    CHECK(response.status == 413);
    CHECK(strstr(response.body, "\"code\":\"PAYLOAD_TOO_LARGE\"") != NULL);
    free(body);
    stop_server(&ts);
}

/* E37-04: 테이블 fd 를 읽기 전용으로 바꿔 pwrite 실패 (ERR_IO) 를 만든다.
 * (파일 권한을 바꿔도 이미 열린 fd 는 계속 쓸 수 있어서 fd 를 바꾼다) */
static void test_query_io_error(void)
{
    TestServer ts;
    start_users_server(&ts);
    Table *table = (Table *)engine_find_table(ts.db.engine, "users");
    CHECK(table != NULL);
    char path[TEST_PATH_MAX];
    snprintf(path, sizeof(path), "%s/users.tbl", ts.db.dir);
    int writable_fd = table->fd;
    table->fd = open(path, O_RDONLY); /* 아직 요청이 없어 워커가 테이블을 쓰지 않는다 */
    CHECK(table->fd >= 0);

    Response response;
    post_query(ts.port, "{\"sql\":\"INSERT INTO users VALUES ('kim', 25)\"}", &response);
    CHECK(response.status == 500);
    CHECK(strstr(response.body, "\"code\":\"IO\"") != NULL);
    post_query(ts.port, "{\"sql\":\"SELECT * FROM users\"}", &response); /* 서버는 계속 동작 */
    CHECK(response.status == 200);
    CHECK(strstr(response.body, "\"row_count\":0") != NULL);

    server_stop(ts.server); /* 워커가 모두 끝난 뒤, 엔진을 닫기 전에 fd 를 되돌린다 */
    CHECK(pthread_join(ts.thread, NULL) == 0);
    server_destroy(ts.server);
    close(table->fd);
    table->fd = writable_fd;
    test_db_destroy(&ts.db);
}

typedef struct {
    int port;
    int32_t ids[INSERTS_PER_CLIENT];
} InsertClient;

static void *insert_many(void *arg)
{
    InsertClient *client = arg;
    for (int i = 0; i < INSERTS_PER_CLIENT; i++) {
        Response *response = malloc(sizeof(*response));
        CHECK(response != NULL);
        post_query(client->port, "{\"sql\":\"INSERT INTO users VALUES ('t', 1)\"}", response);
        CHECK(response->status == 200);
        client->ids[i] = insert_id_of(response);
        free(response);
    }
    return NULL;
}

/* E37-05 */
static void test_concurrent_inserts(void)
{
    ServerOptions options = server_default_options();
    options.worker_count = CONCURRENT_CLIENTS;
    TestServer ts;
    start_server_on(&ts, &options, USERS_SQL);
    pthread_t threads[CONCURRENT_CLIENTS];
    InsertClient *clients = calloc(CONCURRENT_CLIENTS, sizeof(*clients));
    CHECK(clients != NULL);
    for (int t = 0; t < CONCURRENT_CLIENTS; t++) {
        clients[t].port = ts.port;
        CHECK(pthread_create(&threads[t], NULL, insert_many, &clients[t]) == 0);
    }
    bool seen[CONCURRENT_CLIENTS * INSERTS_PER_CLIENT + 1] = {false};
    for (int t = 0; t < CONCURRENT_CLIENTS; t++) {
        CHECK(pthread_join(threads[t], NULL) == 0);
        for (int i = 0; i < INSERTS_PER_CLIENT; i++) {
            int32_t id = clients[t].ids[i];
            CHECK(id >= 1 && id <= CONCURRENT_CLIENTS * INSERTS_PER_CLIENT && !seen[id]);
            seen[id] = true; /* 800 개가 모두 다르고 범위 안 = 1..800 중복·누락 없음 */
        }
    }
    free(clients);
    Response response;
    post_query(ts.port, "{\"sql\":\"SELECT id FROM users WHERE id >= 800\"}", &response);
    CHECK(strstr(response.body, "\"rows\":[[800]],\"row_count\":1") != NULL);
    stop_server(&ts);
}

/* ---- S38 /tables, /stats ---- */

#define ORDERS_SQL "CREATE TABLE orders (id INT, item VARCHAR(8));"
#define LOAD_INSERTERS 4
#define LOAD_INSERTS 100
#define LOAD_POLLS 50

static int64_t json_number(const char *json, const char *field)
{
    char key[64];
    snprintf(key, sizeof(key), "\"%s\":", field);
    const char *at = strstr(json, key);
    CHECK(at != NULL);
    long long value = -1;
    CHECK(sscanf(at + strlen(key), "%lld", &value) == 1);
    return value;
}

/* T38-01 */
static void test_tables_lists_schema(void)
{
    ServerOptions options = server_default_options();
    TestServer ts;
    start_server_on(&ts, &options, USERS_SQL ORDERS_SQL);
    Response response;
    send_request(ts.port, "GET /tables HTTP/1.1\r\n\r\n", &response);
    CHECK(response.status == 200);
    CHECK(strncmp(response.body, "{\"ok\":true,\"tables\":[", 21) == 0);
    const char *users = "{\"name\":\"users\",\"columns\":["
                        "{\"name\":\"id\",\"type\":\"INT\",\"size\":4},"
                        "{\"name\":\"name\",\"type\":\"VARCHAR\",\"size\":32},"
                        "{\"name\":\"age\",\"type\":\"INT\",\"size\":4}],\"row_count\":0}";
    const char *orders = "{\"name\":\"orders\",\"columns\":["
                         "{\"name\":\"id\",\"type\":\"INT\",\"size\":4},"
                         "{\"name\":\"item\",\"type\":\"VARCHAR\",\"size\":8}],\"row_count\":0}";
    CHECK(strstr(response.body, users) != NULL); /* 테이블 순서는 디렉터리 순서라 정해져 있지 않다 */
    CHECK(strstr(response.body, orders) != NULL);
    stop_server(&ts);
}

/* T38-02 */
static void test_tables_row_count(void)
{
    TestServer ts;
    start_users_server(&ts);
    Response response;
    for (int i = 0; i < 3; i++)
        post_query(ts.port, "{\"sql\":\"INSERT INTO users VALUES ('kim', 25)\"}", &response);
    send_request(ts.port, "GET /tables HTTP/1.1\r\n\r\n", &response);
    CHECK(json_number(response.body, "row_count") == 3);
    stop_server(&ts);
}

/* E38-01 */
static void test_tables_empty(void)
{
    TestServer ts;
    start_server(&ts);
    Response response;
    send_request(ts.port, "GET /tables HTTP/1.1\r\n\r\n", &response);
    CHECK(response.status == 200);
    CHECK_STR_EQ(response.body, "{\"ok\":true,\"tables\":[]}");
    stop_server(&ts);
}

/* T38-03 */
static void test_stats_counts_requests(void)
{
    TestServer ts;
    start_server(&ts);
    Response response;
    for (int i = 0; i < 5; i++)
        send_request(ts.port, HEALTH_REQUEST, &response);
    send_request(ts.port, "GET /stats HTTP/1.1\r\n\r\n", &response);
    CHECK(response.status == 200);
    CHECK(json_number(response.body, "requests_total") >= 5);
    CHECK(json_number(response.body, "errors_total") == 0);
    CHECK(json_number(response.body, "rejected_total") == 0);
    CHECK(json_number(response.body, "queue_length") >= 0);
    CHECK(json_number(response.body, "queue_capacity") ==
          DEFAULT_TEST_QUEUE);
    CHECK(json_number(response.body, "workers") == DEFAULT_TEST_WORKERS);
    CHECK(json_number(response.body, "uptime_ms") >= 0);
    char lock_mode[MAX_MESSAGE];
    snprintf(lock_mode, sizeof(lock_mode), "\"lock_mode\":\"%s\"", engine_lock_mode_name());
    CHECK(strstr(response.body, lock_mode) != NULL);
    stop_server(&ts);
}

/* E38-02 */
static void test_stats_counts_errors_and_rejections(void)
{
    ServerOptions options = server_default_options();
    options.worker_count = 1;
    options.queue_capacity = 1;
    options.read_timeout_ms = HELD_TIMEOUT_MS;
    TestServer ts;
    start_server_with(&ts, &options);
    Response response;
    send_request(ts.port, "GET /nope HTTP/1.1\r\n\r\n", &response);  /* 404 */
    send_request(ts.port, "PUT /health HTTP/1.1\r\n\r\n", &response); /* 405 */
    int fds[FLOOD_CLIENTS];                                           /* 503 을 만든다 */
    for (int i = 0; i < FLOOD_CLIENTS; i++)
        fds[i] = connect_to(ts.port);
    for (int i = 0; i < FLOOD_CLIENTS; i++) {
        read_response(fds[i], &response);
        close(fds[i]);
    }
    send_request(ts.port, "GET /stats HTTP/1.1\r\n\r\n", &response);
    int64_t errors = json_number(response.body, "errors_total");
    int64_t rejected = json_number(response.body, "rejected_total");
    /* 404, 405 와 flood 연결 각각이 408 (에러) 또는 503 (거절) 중 하나로 세어진다 */
    CHECK(errors + rejected == 2 + FLOOD_CLIENTS);
    CHECK(rejected >= FLOOD_CLIENTS - 2);
    stop_server(&ts);
}

typedef struct {
    int port;
    bool is_poller; /* false 면 INSERT, true 면 /tables·/stats 를 번갈아 읽는다 */
} LoadClient;

static void *run_load_client(void *arg)
{
    LoadClient *client = arg;
    Response *response = malloc(sizeof(*response));
    CHECK(response != NULL);
    int64_t last_rows = 0;
    int rounds = client->is_poller ? LOAD_POLLS : LOAD_INSERTS;
    for (int i = 0; i < rounds; i++) {
        if (!client->is_poller) {
            post_query(client->port, "{\"sql\":\"INSERT INTO users VALUES ('t', 1)\"}", response);
            CHECK(response->status == 200);
            continue;
        }
        send_request(client->port, "GET /tables HTTP/1.1\r\n\r\n", response);
        int64_t rows = json_number(response->body, "row_count");
        CHECK(rows >= last_rows && rows <= LOAD_INSERTERS * LOAD_INSERTS); /* 줄지 않고 범위 안 */
        last_rows = rows;
        send_request(client->port, "GET /stats HTTP/1.1\r\n\r\n", response);
        CHECK(json_number(response->body, "requests_total") >= 0);
        CHECK(json_number(response->body, "queue_length") >= 0);
    }
    free(response);
    return NULL;
}

/* E38-03 */
static void test_stats_under_load(void)
{
    ServerOptions options = server_default_options();
    TestServer ts;
    start_server_on(&ts, &options, USERS_SQL);
    LoadClient clients[LOAD_INSERTERS + 2];
    pthread_t threads[LOAD_INSERTERS + 2];
    for (int i = 0; i < LOAD_INSERTERS + 2; i++) {
        clients[i] = (LoadClient){ts.port, i >= LOAD_INSERTERS};
        CHECK(pthread_create(&threads[i], NULL, run_load_client, &clients[i]) == 0);
    }
    for (int i = 0; i < LOAD_INSERTERS + 2; i++)
        CHECK(pthread_join(threads[i], NULL) == 0);
    Response response;
    send_request(ts.port, "GET /tables HTTP/1.1\r\n\r\n", &response);
    CHECK(json_number(response.body, "row_count") == LOAD_INSERTERS * LOAD_INSERTS);
    stop_server(&ts);
}

/* ---- S39 우아한 종료 ---- */

#define INSERT_BODY "{\"sql\":\"INSERT INTO users VALUES ('kim', 25)\"}"
#define INSERT_HEAD_FORMAT "POST /query HTTP/1.1\r\nContent-Length: %zu\r\n\r\n"
#define LINE_SIZE 256

/* accept 루프가 연결 accepted 개를 받았고 큐에 queued 개가 남았을 때까지 기다린다.
 * 시간에 기대지 않고 서버 상태를 직접 본다 (워커가 연결을 꺼내 갔는지 = 받은 수 - 큐 길이) */
static void wait_for_server_state(Server *server, int64_t accepted, int queued)
{
    while (atomic_load(&server->accepted_total) < accepted ||
           threadpool_queue_length(server->pool) != queued)
        sched_yield();
}

/* INSERT 요청의 헤더만 보내 둔 연결 (본문은 finish_insert 로). 워커는 본문을 기다리며 붙잡힌다 */
static int start_insert(int port)
{
    int fd = connect_to(port);
    char head[REQUEST_HEAD_SIZE];
    int len = snprintf(head, sizeof(head), INSERT_HEAD_FORMAT, strlen(INSERT_BODY));
    CHECK(write(fd, head, (size_t)len) == len);
    return fd;
}

static void finish_insert(int fd, Response *out)
{
    CHECK(write(fd, INSERT_BODY, strlen(INSERT_BODY)) == (ssize_t)strlen(INSERT_BODY));
    read_response(fd, out);
    close(fd);
}

static int count_users(Engine *engine)
{
    Result result = engine_execute(engine, "SELECT * FROM users", NULL, 0);
    CHECK(result.error == ERR_NONE);
    int count = (int)result.row_count;
    result_free(&result);
    return count;
}

/* 서버를 멈추게 하고 (server_stop), 같은 경로로 종료가 끝나는 것을 기다린다 */
static void stop_and_join(TestServer *ts)
{
    server_stop(ts->server);
    CHECK(pthread_join(ts->thread, NULL) == 0);
    server_destroy(ts->server);
}

/* T39-01 */
static void test_stop_idle_server(void)
{
    TestServer ts;
    start_users_server(&ts);
    stop_and_join(&ts); /* 멈추지 않으면 join 이 돌아오지 않는다. 누수는 ASan 빌드가 본다 */
    test_db_destroy(&ts.db);
}

/* T39-02, E39-02, E39-03 */
static void test_stop_while_insert_in_progress(void)
{
    TestServer ts;
    start_users_server(&ts);
    int fd = start_insert(ts.port);
    wait_for_server_state(ts.server, 1, 0); /* 워커가 이 연결을 잡고 본문을 기다린다 */
    server_stop(ts.server);

    /* E39-02: 종료가 시작되면 listen 소켓이 닫혀 새 연결은 거부된다 */
    for (;;) {
        int probe = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)ts.port)};
        inet_pton(AF_INET, LOCALHOST, &address.sin_addr);
        int connected = connect(probe, (struct sockaddr *)&address, sizeof(address));
        int saved_errno = errno;
        close(probe);
        if (connected < 0) {
            /* listen 이 닫힌 뒤면 REFUSED, 닫히는 순간 backlog 에 들어갔다 끊긴 연결이면 RESET.
             * 둘 다 새 연결을 받지 않았다는 뜻이다 */
            CHECK(saved_errno == ECONNREFUSED || saved_errno == ECONNRESET);
            break;
        }
        sched_yield(); /* 아직 accept 루프가 깨어나기 전 */
    }

    Response response;
    finish_insert(fd, &response); /* 처리 중이던 요청은 끝까지 응답받는다 */
    CHECK(response.status == 200);
    CHECK(pthread_join(ts.thread, NULL) == 0);
    server_destroy(ts.server);

    test_db_reopen(&ts.db); /* E39-03: 다시 열어도 헤더와 데이터가 맞다 (복구 경고 없음) */
    CHECK(ts.db.engine != NULL);
    CHECK(engine_warning_count(ts.db.engine) == 0);
    CHECK(count_users(ts.db.engine) == 1);
    CHECK(engine_record_count(ts.db.engine, 0) == 1); /* users 하나뿐 */
    test_db_destroy(&ts.db);
}

/* E39-01 */
static void test_stop_answers_queued_requests(void)
{
    ServerOptions options = server_default_options();
    options.worker_count = 1;
    TestServer ts;
    start_server_on(&ts, &options, USERS_SQL);
    int busy_fd = start_insert(ts.port);     /* 하나뿐인 워커를 붙잡는다 */
    wait_for_server_state(ts.server, 1, 0);
    int queued_fd = start_insert(ts.port);   /* 큐에서 기다린다 */
    CHECK(write(queued_fd, INSERT_BODY, strlen(INSERT_BODY)) == (ssize_t)strlen(INSERT_BODY));
    wait_for_server_state(ts.server, 2, 1);
    server_stop(ts.server);

    Response response;
    finish_insert(busy_fd, &response);
    CHECK(response.status == 200);
    read_response(queued_fd, &response); /* 큐에 있던 요청도 종료 전에 처리된다 */
    close(queued_fd);
    CHECK(response.status == 200);
    CHECK(pthread_join(ts.thread, NULL) == 0);
    server_destroy(ts.server);
    test_db_reopen(&ts.db);
    CHECK(count_users(ts.db.engine) == 2);
    test_db_destroy(&ts.db);
}

/* 자식 프로세스의 "minidb: listening on port N" 줄에서 포트를 읽는다 */
static int read_child_port(FILE *child_out)
{
    char line[LINE_SIZE];
    CHECK(fgets(line, sizeof(line), child_out) != NULL);
    int port = 0;
    CHECK(sscanf(line, "minidb: listening on port %d", &port) == 1);
    return port;
}

/* E39-04: 실제 serve 명령을 자식 프로세스로 띄우고 SIGINT 를 두 번 보낸다. 시그널 처리는 프로세스
 * 전체에 걸리므로 테스트 프로세스와 분리한다 (fork 시점에는 테스트 스레드가 하나뿐이다) */
static void test_sigint_twice_shuts_down_cleanly(void)
{
    TestDb db = test_db_create(USERS_SQL);
    CHECK(db.engine != NULL);
    engine_close(db.engine);
    db.engine = NULL;
    int pipe_fds[2];
    CHECK(pipe(pipe_fds) == 0);
    fflush(stdout);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(pipe_fds[0]);
        FILE *out = fdopen(pipe_fds[1], "w");
        char *argv[] = {"minidb", "-d", db.dir, "serve", "0", "2", NULL};
        _exit(cli_run(6, argv, out, stderr));
    }
    close(pipe_fds[1]);
    FILE *child_out = fdopen(pipe_fds[0], "r");
    CHECK(child_out != NULL);
    int port = read_child_port(child_out);
    Response response;
    post_query(port, INSERT_BODY, &response);
    CHECK(response.status == 200);

    CHECK(kill(child, SIGINT) == 0);
    CHECK(kill(child, SIGINT) == 0); /* 두 번째는 종료 중에 오거나 보류된다: 크래시 없음 */
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == CLI_EXIT_OK); /* 시그널로 죽지 않고 정상 종료 */
    fclose(child_out);

    test_db_reopen(&db);
    CHECK(engine_warning_count(db.engine) == 0);
    CHECK(count_users(db.engine) == 1);
    test_db_destroy(&db);
}

int main(void)
{
    printf("test_server\n");
    RUN(test_port_zero_picks_port);
    RUN(test_health);
    RUN(test_sequential_clients);
    RUN(test_unknown_path);
    RUN(test_wrong_method);
    RUN(test_bad_http_request);
    RUN(test_overload_returns_503);
    RUN(test_port_in_use);
    RUN(test_quick_disconnects);
    RUN(test_invalid_options);
    RUN(test_serve_usage_errors);
    RUN(test_query_insert);
    RUN(test_query_select_after_insert);
    RUN(test_query_engine_errors);
    RUN(test_query_json_errors);
    RUN(test_error_status_table);
    RUN(test_query_too_large);
    RUN(test_query_io_error);
    RUN(test_concurrent_inserts);
    RUN(test_tables_lists_schema);
    RUN(test_tables_row_count);
    RUN(test_tables_empty);
    RUN(test_stats_counts_requests);
    RUN(test_stats_counts_errors_and_rejections);
    RUN(test_stats_under_load);
    RUN(test_stop_idle_server);
    RUN(test_stop_while_insert_in_progress);
    RUN(test_stop_answers_queued_requests);
    RUN(test_sigint_twice_shuts_down_cleanly);
    TEST_DONE();
    return 0;
}
