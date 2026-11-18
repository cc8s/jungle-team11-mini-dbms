#include "http_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "api.h"
#include "executor.h"
#include "http.h"
#include "threadpool.h"

#define SERVER_DEFAULT_PORT    8080
#define SERVER_DEFAULT_WORKERS 8
#define SERVER_DEFAULT_QUEUE   64
#define SERVER_BACKLOG         128
#define SERVER_IO_TIMEOUT_SEC  5

static const char SERVER_BUSY_JSON[] = "{\"ok\":false,\"error\":\"server busy, try again\"}";

struct server {
    struct executor ex;
    struct threadpool pool;
    atomic_size_t served;
    atomic_size_t busy_rejected;
};

struct connection {
    struct server *server;
    int fd;
};

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void send_json(int fd, int status, const char *json)
{
    if (json) {
        http_write_response(fd, status, "application/json", json, strlen(json));
        return;
    }
    static const char oom[] = "{\"ok\":false,\"error\":\"out of memory\"}";
    http_write_response(fd, 500, "application/json", oom, sizeof(oom) - 1);
}

static void send_error(int fd, int status, const char *msg)
{
    char *json = api_error_json(msg);

    send_json(fd, status, json);
    free(json);
}

static void route(struct server *s, int fd, const struct http_request *req)
{
    if (strcmp(req->path, "/query") == 0) {
        if (strcmp(req->method, "POST") != 0) {
            send_error(fd, 405, "use POST /query");
            return;
        }
        int status;
        char *json = api_handle_query(&s->ex, req->body, req->body_len, &status);
        send_json(fd, status, json);
        free(json);
        return;
    }
    if (strcmp(req->path, "/health") == 0) {
        send_json(fd, 200, "{\"ok\":true}");
        return;
    }
    send_error(fd, 404, "not found (POST /query, GET /health)");
}

/* 요청을 다 읽기 전에 오류로 답했을 때(413 등): 바로 닫으면 아직 본문을 보내던 클라이언트는
 * 응답을 읽기도 전에 연결이 끊긴다(Broken pipe / RST). 쓰기 쪽만 닫고 남은 입력을 잠깐 읽어 버린 뒤 닫는다 */
static void drain_and_close_write(int fd)
{
    char sink[4096];
    struct timeval tv = {1, 0};
    size_t drained = 0;

    shutdown(fd, SHUT_WR);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (drained < 2 * HTTP_MAX_BODY) {
        ssize_t n = recv(fd, sink, sizeof(sink), 0);
        if (n <= 0) {
            break;
        }
        drained += (size_t)n;
    }
}

/* 워커 스레드에서 돈다: 요청 하나 읽기 → 처리 → 응답 → 닫기 */
static void handle_connection(void *arg)
{
    struct connection *c = arg;
    struct http_request req;
    char *buf;
    int status;

    if (http_read_request(c->fd, &buf, &req, &status) == 0) {
        route(c->server, c->fd, &req);
        free(buf);
    } else if (status != 0) {
        send_error(c->fd, status, http_status_text(status));
        drain_and_close_write(c->fd);
    }
    atomic_fetch_add(&c->server->served, 1);
    close(c->fd);
    free(c);
}

static void set_io_timeout(int fd)
{
    struct timeval tv = {SERVER_IO_TIMEOUT_SEC, 0};

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int open_listener(int port, int *bound_port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    if (fd < 0) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, SERVER_BACKLOG) < 0 ||
        getsockname(fd, (struct sockaddr *)&addr, &addr_len) < 0) {
        close(fd);
        return -1;
    }
    *bound_port = ntohs(addr.sin_port);
    return fd;
}

/* 메인 스레드: 연결을 받아 큐에 넣는다. 큐가 가득 차면 기다리지 않고 503 (Q12) */
static void accept_loop(struct server *s, int listen_fd)
{
    while (!g_stop) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno != EINTR) {
                perror("accept");
            }
            continue;
        }
        set_io_timeout(fd);
        struct connection *c = malloc(sizeof(*c));
        if (c) {
            c->server = s;
            c->fd = fd;
        }
        if (!c || threadpool_submit(&s->pool, handle_connection, c) != 0) {
            atomic_fetch_add(&s->busy_rejected, 1);
            http_write_response(fd, 503, "application/json", SERVER_BUSY_JSON,
                                sizeof(SERVER_BUSY_JSON) - 1);
            close(fd);
            free(c);
        }
    }
}

static void install_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal; /* SA_RESTART 없음: accept 가 EINTR 로 깨어나 g_stop 을 본다 */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN); /* 끊긴 소켓에 쓰면 죽지 말고 오류로 받는다 */
}

/* 워커가 시그널을 가로채면 메인의 accept 가 안 깬다. 풀을 만들기 전에 막고, 만든 뒤 메인만 푼다 */
static void block_stop_signals(int how)
{
    sigset_t set;

    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(how, &set, NULL);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s --server [-p port] [-w workers] [-q queue] [-s schema_dir] [-d data_dir] "
            "[-n]\n",
            prog);
}

int server_main(int argc, char **argv)
{
    int port = SERVER_DEFAULT_PORT;
    int workers = SERVER_DEFAULT_WORKERS;
    int queue = SERVER_DEFAULT_QUEUE;
    const char *schema_dir = "schema";
    const char *data_dir = "data";
    bool use_index = true;
    int opt;

    optind = 2; /* argv[1] 은 --server */
    while ((opt = getopt(argc, argv, "p:w:q:s:d:nh")) != -1) {
        if (opt == 'p') {
            port = atoi(optarg);
        } else if (opt == 'w') {
            workers = atoi(optarg);
        } else if (opt == 'q') {
            queue = atoi(optarg);
        } else if (opt == 's') {
            schema_dir = optarg;
        } else if (opt == 'd') {
            data_dir = optarg;
        } else if (opt == 'n') {
            use_index = false;
        } else {
            usage(argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }

    static struct server s;
    if (executor_init(&s.ex, schema_dir, data_dir) < 0) {
        fprintf(stderr, "path too long\n");
        return 2;
    }
    s.ex.use_index = use_index;

    int bound_port;
    int listen_fd = open_listener(port, &bound_port);
    if (listen_fd < 0) {
        perror("listen");
        executor_close(&s.ex);
        return 1;
    }

    block_stop_signals(SIG_BLOCK);
    if (threadpool_init(&s.pool, workers, queue) < 0) {
        fprintf(stderr, "bad -w / -q\n");
        close(listen_fd);
        executor_close(&s.ex);
        return 2;
    }
    install_signals();
    block_stop_signals(SIG_UNBLOCK);

    printf("listening on port %d (workers %d, queue %d, index %s)\n", bound_port, workers, queue,
           use_index ? "on" : "off");
    fflush(stdout);

    accept_loop(&s, listen_fd);

    close(listen_fd);
    threadpool_shutdown(&s.pool); /* 큐에 남은 요청을 끝내고 워커를 join */
    executor_close(&s.ex);        /* 테이블 flush */
    printf("stopped: served %zu, rejected busy %zu\n", atomic_load(&s.served),
           atomic_load(&s.busy_rejected));
    return 0;
}
