#include "cli.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "engine.h"
#include "format.h"
#include "parser.h"
#include "server.h"
#include "table.h"

#define DEFAULT_DATA_DIR "data"
#define INIT_COMMAND "init"
#define SERVE_COMMAND "serve"
#define SERVE_MIN_ARGS 2 /* <port> <threads> */
#define SERVE_MAX_ARGS 3 /* [queue_cap] */
#define SERVE_MAX_WORKERS 1024
#define SERVE_MAX_QUEUE 1048576
#define DECIMAL_BASE 10
#define INIT_STATEMENT_INDEX 1

static const char USAGE[] =
    "usage: minidb [-d <data_dir>] [-q] <file.sql>\n"
    "       minidb [-d <data_dir>] init <schema.sql>\n"
    "       minidb [-d <data_dir>] serve <port> <threads> [queue_cap]\n"
    "  -d <data_dir>  data directory (default: " DEFAULT_DATA_DIR ")\n"
    "  -q             print only errors and a summary\n";

typedef struct {
    const char *data_dir;
    const char *file;
    bool is_init;
    bool is_quiet;
    bool is_serve;
    const char *serve_args[SERVE_MAX_ARGS]; /* port, threads, queue_cap */
    int serve_arg_count;
} CliOptions;

/* ---- 옵션 ---- */

/* 위치 인자: 첫 번째가 "init" 이면 다음 인자가 스키마 파일, "serve" 면 다음 2~3 개가 서버 인자,
 * 아니면 그것이 SQL 파일 */
static bool take_positional(CliOptions *options, const char *arg)
{
    if (options->is_serve) {
        if (options->serve_arg_count == SERVE_MAX_ARGS)
            return false;
        options->serve_args[options->serve_arg_count++] = arg;
        return true;
    }
    if (options->file != NULL)
        return false;
    if (!options->is_init && strcmp(arg, INIT_COMMAND) == 0) {
        options->is_init = true;
        return true;
    }
    if (!options->is_init && strcmp(arg, SERVE_COMMAND) == 0) {
        options->is_serve = true;
        return true;
    }
    options->file = arg;
    return true;
}

/* getopt 은 전역 상태(optind)를 써서 쓰지 않는다 */
static bool parse_options(int argc, char **argv, CliOptions *out_options)
{
    *out_options = (CliOptions){ .data_dir = DEFAULT_DATA_DIR };
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        bool is_ok = true;
        if (strcmp(arg, "-d") == 0 && i + 1 < argc)
            out_options->data_dir = argv[++i];
        else if (strcmp(arg, "-q") == 0)
            out_options->is_quiet = true;
        else if (arg[0] == '-')
            is_ok = false;
        else
            is_ok = take_positional(out_options, arg);
        if (!is_ok)
            return false;
    }
    if (out_options->is_serve)
        return out_options->serve_arg_count >= SERVE_MIN_ARGS;
    return out_options->file != NULL;
}

/* ---- 파일 ---- */

static bool read_all(int fd, char *text, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(fd, text + done, len - done, (off_t)done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        done += (size_t)n;
    }
    return true;
}

/* 일반 파일 전체를 읽어 \0 을 붙여 돌려준다 (호출한 쪽이 free). 실패하면 err 에 쓰고 NULL */
static char *read_text_file(const char *path, size_t *out_len, FILE *err)
{
    char *text = NULL;
    struct stat info;
    char reason[MAX_MESSAGE];
    int fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        /* strerror 는 내부 버퍼를 공유해 재진입이 안 되므로 GNU strerror_r 을 쓴다 */
        fprintf(err, "minidb: cannot read '%s': %s\n", path,
                fd < 0 ? strerror_r(errno, reason, sizeof(reason)) : "not a regular file");
        goto cleanup;
    }
    text = malloc((size_t)info.st_size + 1);
    if (text == NULL || !read_all(fd, text, (size_t)info.st_size)) {
        fprintf(err, "minidb: cannot read '%s'\n", path);
        free(text);
        text = NULL;
        goto cleanup;
    }
    text[info.st_size] = '\0';
    *out_len = (size_t)info.st_size;

cleanup:
    if (fd >= 0)
        close(fd);
    return text;
}

/* 엔진은 \0 으로 끝나는 문자열을 받으므로, 중간의 \0 뒤를 조용히 버리지 않도록 미리 거절한다 */
static bool reject_nul_byte(const char *path, const char *text, size_t len, FILE *err)
{
    const char *nul = memchr(text, '\0', len);
    if (nul == NULL)
        return false;
    int line = 1;
    for (const char *c = text; c < nul; c++)
        line += *c == '\n';
    fprintf(err, "ERROR (line %d): '%s' contains a NUL byte; nothing was executed\n", line, path);
    return true;
}

static bool is_directory(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

/* ---- init ---- */

static int init_error(FILE *out, FILE *err, ErrorCode code, int line, const char *message)
{
    Result result = result_error(code, line, "%s", message);
    format_result(out, err, &result, INIT_STATEMENT_INDEX);
    return CLI_EXIT_SQL_ERROR;
}

/* 스키마는 CREATE TABLE 한 문장만. 테이블이 이미 있으면 덮어쓰지 않는다 */
static int create_table(const CliOptions *options, const Statement *stmt, FILE *out, FILE *err)
{
    if (stmt->type != STMT_CREATE)
        return init_error(out, err, ERR_UNSUPPORTED, stmt->line,
                          "a schema file must contain exactly one CREATE TABLE");
    char path[MAX_PATH];
    const char *table = stmt->create.table;
    if (!table_build_path(options->data_dir, table, path, sizeof(path)))
        return init_error(out, err, ERR_IO, stmt->line, "table path is too long");
    if (access(path, F_OK) == 0) {
        fprintf(err, "ERROR (line %d, statement %d): table '%s' already exists\n", stmt->line,
                INIT_STATEMENT_INDEX, table);
        return CLI_EXIT_SQL_ERROR;
    }
    if (table_create(path, &stmt->create) != ERR_NONE)
        return init_error(out, err, ERR_IO, stmt->line, "could not create the table file");
    fprintf(out, "Table '%s' created (%s)\n", table, path);
    return CLI_EXIT_OK;
}

static int run_init(const CliOptions *options, const char *text, size_t len, FILE *out,
                    FILE *err)
{
    Statement stmt;
    ParseError parse_error;
    ErrorCode error = parser_parse_single(text, len, &stmt, &parse_error);
    int code = error == ERR_NONE
                   ? create_table(options, &stmt, out, err)
                   : init_error(out, err, error, parse_error.line, parse_error.message);
    statement_free(&stmt);
    return code;
}

/* ---- SQL 실행 ---- */

typedef struct {
    FILE *out;
    FILE *err;
    bool is_quiet;
    int statement_count;
} ScriptOutput;

/* -q 면 에러만 출력한다. 마지막 요약에 쓰도록 문장 수를 센다 */
static void print_result(const Result *result, int stmt_index, void *ctx)
{
    ScriptOutput *output = ctx;
    output->statement_count++;
    if (result->error != ERR_NONE || !output->is_quiet)
        format_result(output->out, output->err, result, stmt_index);
}

static int run_script(const CliOptions *options, const char *text, FILE *out, FILE *err)
{
    ErrorCode error = ERR_NONE;
    char message[MAX_MESSAGE];
    Engine *engine = engine_open(options->data_dir, &error, message);
    if (engine == NULL) {
        fprintf(err, "minidb: %s\n", message);
        return CLI_EXIT_USAGE;
    }
    for (int i = 0; i < engine_warning_count(engine); i++)
        fprintf(err, "minidb: warning: %s\n", engine_warning(engine, i));
    ScriptOutput output = { .out = out, .err = err, .is_quiet = options->is_quiet };
    int error_count = engine_execute_script(engine, text, print_result, &output);
    engine_close(engine);
    if (options->is_quiet)
        fprintf(out, "%d statements, %d errors\n", output.statement_count, error_count);
    return error_count > 0 ? CLI_EXIT_SQL_ERROR : CLI_EXIT_OK;
}

static bool check_data_dir(const CliOptions *options, FILE *err)
{
    if (is_directory(options->data_dir))
        return true;
    fprintf(err, "minidb: data directory '%s' does not exist\n", options->data_dir);
    return false;
}

/* ---- 서버 ---- */

/* 부호 없는 10 진수만 받는다 ("80x", "-1", "" 는 실패). min..max 밖이어도 실패 */
static bool parse_bounded_int(const char *text, long min, long max, int *out_value)
{
    if (text[0] < '0' || text[0] > '9')
        return false;
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, DECIMAL_BASE);
    if (errno != 0 || *end != '\0' || value < min || value > max)
        return false;
    *out_value = (int)value;
    return true;
}

/* serve <port> <threads> [queue_cap] 를 ServerOptions 로. queue_cap 이 없으면 threads × 16 */
static bool parse_serve_options(const CliOptions *options, ServerOptions *out_server)
{
    *out_server = server_default_options();
    if (!parse_bounded_int(options->serve_args[0], 0, SERVER_MAX_PORT, &out_server->port) ||
        !parse_bounded_int(options->serve_args[1], 1, SERVE_MAX_WORKERS,
                           &out_server->worker_count))
        return false;
    out_server->queue_capacity = out_server->worker_count * SERVER_DEFAULT_QUEUE_PER_WORKER;
    return options->serve_arg_count < SERVE_MAX_ARGS ||
           parse_bounded_int(options->serve_args[2], 1, SERVE_MAX_QUEUE,
                             &out_server->queue_capacity);
}

/* SIGINT/SIGTERM 을 받았는지. 시그널 핸들러는 인자를 받지 못해서 전역일 수밖에 없는 유일한 상태다
 * (CLAUDE.md 의 전역 금지 규칙의 예외). 핸들러는 이 값만 바꾼다: async-signal-safe 하게,
 * 락·malloc·출력 없이. 실제 종료는 accept 루프가 ppoll 의 EINTR 로 깨어나 이 값을 보고 한다 */
static volatile sig_atomic_t serve_stop_requested = 0;

static void request_serve_stop(int signal_number)
{
    (void)signal_number;
    serve_stop_requested = 1;
}

/* 서버가 도는 동안의 시그널 설정. 끝나면 이전 상태로 되돌린다 */
typedef struct {
    sigset_t old_mask;
    struct sigaction old_int;
    struct sigaction old_term;
} SignalState;

/* 두 시그널을 이 스레드에서 막고 (이후 만드는 워커는 이 마스크를 물려받아 시그널을 받지 않는다),
 * 핸들러를 SA_RESTART 없이 단다. *out_wait_mask 는 accept 루프가 기다릴 때 쓸, 둘만 푼 마스크 */
static void install_serve_signals(SignalState *state, sigset_t *out_wait_mask)
{
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_signals, &state->old_mask);
    *out_wait_mask = state->old_mask;
    sigdelset(out_wait_mask, SIGINT);
    sigdelset(out_wait_mask, SIGTERM);
    serve_stop_requested = 0;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_serve_stop;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0; /* SA_RESTART 없음: 대기 중인 시스템 콜이 EINTR 로 돌아온다 */
    sigaction(SIGINT, &action, &state->old_int);
    sigaction(SIGTERM, &action, &state->old_term);
}

/* 마스크를 먼저 되돌린다: 종료 중에 온 두 번째 SIGINT 가 보류돼 있었다면 여기서 우리 핸들러로
 * 전달되어 무해하게 끝난다. 핸들러를 먼저 되돌리면 그 시그널이 기본 동작 (프로세스 종료) 이 된다 */
static void restore_serve_signals(const SignalState *state)
{
    pthread_sigmask(SIG_SETMASK, &state->old_mask, NULL);
    sigaction(SIGINT, &state->old_int, NULL);
    sigaction(SIGTERM, &state->old_term, NULL);
}

/* 서버를 띄우고 시그널이나 server_stop 으로 멈출 때까지 돈다. 종료 순서 (계획서 7 장):
 * listen 닫기 → 큐에 남은 요청 처리 후 워커 join (server_run 안) → 엔진 닫기 (호출한 쪽) */
static void serve_until_stopped(Server *server, const ServerOptions *options,
                                const sigset_t *wait_mask, FILE *out)
{
    server_watch_signals(server, wait_mask, &serve_stop_requested);
    fprintf(out, "minidb: listening on port %d (workers %d, queue %d)\n", server_port(server),
            options->worker_count, options->queue_capacity);
    fflush(out);
    server_run(server);
    fprintf(out, "minidb: stopped\n");
    fflush(out);
}

static int run_serve(const CliOptions *options, FILE *out, FILE *err)
{
    ServerOptions server_options;
    if (!parse_serve_options(options, &server_options)) {
        fputs(USAGE, err);
        return CLI_EXIT_USAGE;
    }
    if (!check_data_dir(options, err))
        return CLI_EXIT_USAGE;
    ErrorCode error = ERR_NONE;
    char message[MAX_MESSAGE];
    Engine *engine = engine_open(options->data_dir, &error, message);
    if (engine == NULL) {
        fprintf(err, "minidb: %s\n", message);
        return CLI_EXIT_USAGE;
    }
    for (int i = 0; i < engine_warning_count(engine); i++)
        fprintf(err, "minidb: warning: %s\n", engine_warning(engine, i));
    SignalState signals;
    sigset_t wait_mask;
    install_serve_signals(&signals, &wait_mask); /* 워커를 만드는 server_create 보다 먼저 */
    Server *server = server_create(engine, &server_options, message);
    int code = CLI_EXIT_OK;
    if (server == NULL) {
        fprintf(err, "minidb: %s\n", message);
        code = CLI_EXIT_USAGE;
    } else {
        serve_until_stopped(server, &server_options, &wait_mask, out);
    }
    server_destroy(server);
    engine_close(engine);
    restore_serve_signals(&signals);
    return code;
}

/* ---- 진입점 ---- */

int cli_run(int argc, char **argv, FILE *out, FILE *err)
{
    CliOptions options;
    if (!parse_options(argc, argv, &options)) {
        fputs(USAGE, err);
        return CLI_EXIT_USAGE;
    }
    if (options.is_serve) /* 서버 인자를 먼저 검사하고 (사용법), 그다음 디렉터리를 본다 */
        return run_serve(&options, out, err);
    if (!check_data_dir(&options, err))
        return CLI_EXIT_USAGE;
    size_t len = 0;
    char *text = read_text_file(options.file, &len, err);
    if (text == NULL)
        return CLI_EXIT_USAGE;
    int code = CLI_EXIT_SQL_ERROR;
    if (!reject_nul_byte(options.file, text, len, err))
        code = options.is_init ? run_init(&options, text, len, out, err)
                               : run_script(&options, text, out, err);
    free(text);
    return code;
}
