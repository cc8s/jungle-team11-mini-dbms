#include "cli.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "engine.h"
#include "format.h"
#include "parser.h"
#include "table.h"

#define DEFAULT_DATA_DIR "data"
#define INIT_COMMAND "init"
#define INIT_STATEMENT_INDEX 1

static const char USAGE[] =
    "usage: minidb [-d <data_dir>] [-q] <file.sql>\n"
    "       minidb [-d <data_dir>] init <schema.sql>\n"
    "  -d <data_dir>  data directory (default: " DEFAULT_DATA_DIR ")\n"
    "  -q             print only errors and a summary\n";

typedef struct {
    const char *data_dir;
    const char *file;
    bool is_init;
    bool is_quiet;
} CliOptions;

/* ---- 옵션 ---- */

/* 위치 인자: 첫 번째가 "init" 이면 다음 인자가 스키마 파일, 아니면 그것이 SQL 파일 */
static bool take_positional(CliOptions *options, const char *arg)
{
    if (options->file != NULL)
        return false;
    if (!options->is_init && strcmp(arg, INIT_COMMAND) == 0) {
        options->is_init = true;
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
    ScriptOutput output = { .out = out, .err = err, .is_quiet = options->is_quiet };
    int error_count = engine_execute_script(engine, text, print_result, &output);
    engine_close(engine);
    if (options->is_quiet)
        fprintf(out, "%d statements, %d errors\n", output.statement_count, error_count);
    return error_count > 0 ? CLI_EXIT_SQL_ERROR : CLI_EXIT_OK;
}

/* ---- 진입점 ---- */

int cli_run(int argc, char **argv, FILE *out, FILE *err)
{
    CliOptions options;
    if (!parse_options(argc, argv, &options)) {
        fputs(USAGE, err);
        return CLI_EXIT_USAGE;
    }
    if (!is_directory(options.data_dir)) {
        fprintf(err, "minidb: data directory '%s' does not exist\n", options.data_dir);
        return CLI_EXIT_USAGE;
    }
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
