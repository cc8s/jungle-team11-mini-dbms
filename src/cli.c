#include "cli.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "parser.h"

#define CLI_DEFAULT_SCHEMA_DIR "schema"
#define CLI_DEFAULT_DATA_DIR   "data"
#define CLI_READ_CHUNK         (64 * 1024)

/* -t: 문장마다 탐색 방법과 실행 시간을 "-- ..." 주석 줄로 덧붙인다 (CLI 옵션이라 cli_main 만 바꾼다) */
static bool g_show_stats = false;

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* 테이블을 처음 여는 문장은 열기(인덱스 재구성) 시간을 따로 보여 주고 질의 시간에서 뺀다 */
static void print_stats(const struct result *res, double elapsed_ms, FILE *out)
{
    double query_ms = elapsed_ms - res->table_open_ms;

    if (res->table_open_ms > 0) {
        fprintf(out, "-- table opened in %.3f ms (index rebuilt)\n", res->table_open_ms);
    }
    if (res->type == RESULT_INSERT) {
        fprintf(out, "-- %.3f ms\n", query_ms);
        return;
    }
    fprintf(out, "-- %s, examined %zu rows, %.3f ms\n", result_plan_name(res->plan),
            res->rows_examined, query_ms);
}

int cli_run_sql(struct executor *ex, const char *sql, FILE *out)
{
    struct statement stmt;
    struct result res;
    char err[512];
    int failures = 0;

    for (;;) {
        size_t consumed;
        enum parse_result pr = parser_parse(sql, &consumed, &stmt, err, sizeof(err));

        sql += consumed;
        if (pr == PARSE_EMPTY) {
            break;
        }
        double start = now_ms();
        if (pr == PARSE_ERROR || executor_execute(ex, &stmt, &res, err, sizeof(err)) < 0) {
            fprintf(out, "ERROR: %s\n", err);
            failures++;
            continue;
        }
        double elapsed = now_ms() - start;
        result_print(&res, out);
        if (g_show_stats) {
            print_stats(&res, elapsed, out);
        }
        result_free(&res);
    }
    return failures;
}

/* 파일(또는 stdin) 전체를 NUL 로 끝나는 문자열로 읽는다 */
static char *read_all(FILE *fp)
{
    size_t len = 0;
    size_t cap = CLI_READ_CHUNK;
    char *buf = malloc(cap + 1);

    while (buf) {
        size_t n = fread(buf + len, 1, cap - len, fp);
        len += n;
        if (n == 0) {
            break;
        }
        if (len == cap) {
            cap *= 2;
            char *grown = realloc(buf, cap + 1);
            if (!grown) {
                free(buf);
                return NULL;
            }
            buf = grown;
        }
    }
    if (buf) {
        buf[len] = '\0';
    }
    return buf;
}

/* 마지막 공백이 아닌 글자가 ';' 이면 문장이 끝난 것으로 본다 */
static bool ends_statement(const char *s, size_t len)
{
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n' ||
                       s[len - 1] == '\r')) {
        len--;
    }
    return len > 0 && s[len - 1] == ';';
}

static int run_repl(struct executor *ex)
{
    char line[4096];
    char *pending = NULL;
    size_t pending_len = 0;
    int failures = 0;

    printf("minidb — SQL 을 입력하고 ; 로 끝내세요. 종료: .exit 또는 Ctrl-D\n");
    for (;;) {
        fputs(pending_len ? "   ...> " : "minidb> ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }
        if (pending_len == 0 && strncmp(line, ".exit", 5) == 0) {
            break;
        }
        size_t n = strlen(line);
        char *grown = realloc(pending, pending_len + n + 1);
        if (!grown) {
            break;
        }
        pending = grown;
        memcpy(pending + pending_len, line, n + 1);
        pending_len += n;
        if (ends_statement(pending, pending_len)) {
            failures += cli_run_sql(ex, pending, stdout);
            pending_len = 0;
        }
    }
    if (pending_len) {
        failures += cli_run_sql(ex, pending, stdout);
    }
    free(pending);
    putchar('\n');
    return failures;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-s schema_dir] [-d data_dir] [-n] [-t] [query.sql]\n"
            "  -n  인덱스를 쓰지 않는다 (모든 SELECT 를 선형 탐색)\n"
            "  -t  문장마다 탐색 방법, 확인한 행 수, 실행 시간을 출력한다\n",
            prog);
}

int cli_main(int argc, char **argv)
{
    const char *schema_dir = CLI_DEFAULT_SCHEMA_DIR;
    const char *data_dir = CLI_DEFAULT_DATA_DIR;
    bool use_index = true;
    int opt;

    while ((opt = getopt(argc, argv, "s:d:nth")) != -1) {
        if (opt == 's') {
            schema_dir = optarg;
        } else if (opt == 'd') {
            data_dir = optarg;
        } else if (opt == 'n') {
            use_index = false;
        } else if (opt == 't') {
            g_show_stats = true;
        } else {
            usage(argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }
    if (argc - optind > 1) {
        usage(argv[0]);
        return 2;
    }

    struct executor ex;
    if (executor_init(&ex, schema_dir, data_dir) < 0) {
        fprintf(stderr, "path too long\n");
        return 2;
    }
    ex.use_index = use_index;

    int failures;
    if (optind < argc || !isatty(STDIN_FILENO)) {
        FILE *fp = optind < argc ? fopen(argv[optind], "rb") : stdin;
        if (!fp) {
            perror(argv[optind]);
            executor_close(&ex);
            return 2;
        }
        char *sql = read_all(fp);
        if (fp != stdin) {
            fclose(fp);
        }
        if (!sql) {
            fprintf(stderr, "out of memory\n");
            executor_close(&ex);
            return 2;
        }
        failures = cli_run_sql(&ex, sql, stdout);
        free(sql);
    } else {
        failures = run_repl(&ex);
    }
    executor_close(&ex);
    return failures ? 1 : 0;
}
