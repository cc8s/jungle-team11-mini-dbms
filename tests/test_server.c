#include "test.h"
#include "test_util.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cli.h"
#include "server.h"

#define LOCALHOST "127.0.0.1"
#define RESPONSE_CAPACITY 65536
#define SEQUENTIAL_CLIENTS 10
#define FLOOD_CLIENTS 10
#define QUICK_CLOSE_CLIENTS 100
#define HELD_TIMEOUT_MS 1000 /* 503 테스트: 붙잡힌 연결이 타임아웃되기 전에 나머지 연결이 끝나면 된다 */
#define HEALTH_REQUEST "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"

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

static void start_server_with(TestServer *ts, const ServerOptions *options)
{
    ts->db = test_db_create(NULL);
    CHECK(ts->db.engine != NULL);
    char message[MAX_MESSAGE];
    ts->server = server_create(ts->db.engine, options, message);
    CHECK(ts->server != NULL);
    ts->port = server_port(ts->server);
    CHECK(pthread_create(&ts->thread, NULL, run_server, ts) == 0);
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
    TEST_DONE();
    return 0;
}
