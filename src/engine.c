#include "engine.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "executor.h"
#include "parser.h"
#include "table.h"
#include "validator.h"

#define INITIAL_TABLE_CAPACITY 8
#define MS_PER_SECOND 1000.0
#define NS_PER_MS 1000000.0

struct Engine {
    Table **tables;
    int table_count;
    int table_capacity;
    char (*warnings)[MAX_MESSAGE]; /* 열 때 생긴 경고 (헤더 복구). 테이블마다 최대 1 개 */
    int warning_count;
};

/* ---- 열기 ---- */

/* 실패를 out_error / out_message 에 기록하고 그대로 돌려준다 */
static ErrorCode open_failure(ErrorCode code, ErrorCode *out_error, char out_message[MAX_MESSAGE],
                              const char *format, ...) __attribute__((format(printf, 4, 5)));

static ErrorCode open_failure(ErrorCode code, ErrorCode *out_error, char out_message[MAX_MESSAGE],
                              const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(out_message, MAX_MESSAGE, format, args);
    va_end(args);
    *out_error = code;
    return code;
}

/* 이름이 정확히 ".tbl" 로 끝나는 파일만 테이블이다 ("a.tbl.bak", "x.TBL" 은 아님) */
static bool is_table_file(const char *file_name)
{
    size_t len = strlen(file_name);
    size_t ext_len = strlen(TABLE_FILE_EXT);
    return len > ext_len && strcmp(file_name + len - ext_len, TABLE_FILE_EXT) == 0;
}

static bool add_table(Engine *engine, Table *table)
{
    if (engine->table_count == engine->table_capacity) {
        int capacity = engine->table_capacity == 0 ? INITIAL_TABLE_CAPACITY
                                                   : engine->table_capacity * 2;
        Table **grown = realloc(engine->tables, (size_t)capacity * sizeof(*grown));
        if (grown == NULL)
            return false;
        engine->tables = grown;
        engine->table_capacity = capacity;
    }
    engine->tables[engine->table_count++] = table;
    return true;
}

/* 테이블을 열며 헤더를 바로잡았으면 경고 문장을 남긴다. 테이블마다 최대 1 개라서
 * 경고 배열은 테이블 배열과 같은 크기로 두면 충분하다 */
static bool add_recovery_warning(Engine *engine, const Table *table)
{
    TableRecovery recovery;
    if (!table_find_recovery(table, &recovery))
        return true;
    char (*grown)[MAX_MESSAGE] = realloc(engine->warnings, (size_t)engine->table_capacity *
                                                               sizeof(*grown));
    if (grown == NULL)
        return false;
    engine->warnings = grown;
    snprintf(engine->warnings[engine->warning_count++], MAX_MESSAGE,
             "table '%s' header recovered: row_count %d -> %d, next_id %d -> %d",
             table_name(table), recovery.old_row_count, recovery.row_count,
             recovery.old_next_id, recovery.next_id);
    return true;
}

/* data_dir/file_name 을 열어 엔진에 붙인다. 이름이 겹치면 (Users.tbl 과 users.tbl) 손상으로 본다 */
static ErrorCode open_table_file(Engine *engine, const char *data_dir, const char *file_name,
                                 ErrorCode *out_error, char out_message[MAX_MESSAGE])
{
    char path[MAX_PATH];
    if (snprintf(path, sizeof(path), "%s/%s", data_dir, file_name) >= (int)sizeof(path))
        return open_failure(ERR_IO, out_error, out_message, "path too long: %s", file_name);
    ErrorCode error = ERR_NONE;
    Table *table = table_open(path, &error);
    if (table == NULL)
        return open_failure(error, out_error, out_message, "cannot open table file '%s': %s",
                            file_name, error_name(error));
    if (validator_find_table(engine->tables, engine->table_count, table_name(table)) != NULL) {
        table_close(table);
        return open_failure(ERR_CORRUPT, out_error, out_message,
                            "duplicate table name from file '%s'", file_name);
    }
    if (!add_table(engine, table)) {
        table_close(table);
        return open_failure(ERR_INTERNAL, out_error, out_message, "out of memory");
    }
    if (!add_recovery_warning(engine, table)) /* 테이블은 엔진 소유가 됐으므로 engine_close 가 닫는다 */
        return open_failure(ERR_INTERNAL, out_error, out_message, "out of memory");
    return ERR_NONE;
}

/* readdir 은 DIR 핸들마다 상태를 가지므로 핸들을 공유하지 않는 한 스레드에 안전하다 */
static ErrorCode open_all_tables(Engine *engine, DIR *dir, const char *data_dir,
                                 ErrorCode *out_error, char out_message[MAX_MESSAGE])
{
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_table_file(entry->d_name))
            continue;
        ErrorCode error = open_table_file(engine, data_dir, entry->d_name, out_error,
                                          out_message);
        if (error != ERR_NONE)
            return error;
    }
    return ERR_NONE;
}

Engine *engine_open(const char *data_dir, ErrorCode *out_error, char out_message[MAX_MESSAGE])
{
    *out_error = ERR_NONE;
    out_message[0] = '\0';
    Engine *engine = calloc(1, sizeof(*engine));
    if (engine == NULL) {
        open_failure(ERR_INTERNAL, out_error, out_message, "out of memory");
        return NULL;
    }
    DIR *dir = opendir(data_dir);
    if (dir == NULL) {
        open_failure(ERR_IO, out_error, out_message, "cannot open data directory '%s'", data_dir);
        goto cleanup;
    }
    ErrorCode error = open_all_tables(engine, dir, data_dir, out_error, out_message);
    closedir(dir);
    if (error == ERR_NONE)
        return engine;

cleanup:
    engine_close(engine); /* 이미 연 테이블까지 모두 닫는다 */
    return NULL;
}

void engine_close(Engine *engine)
{
    if (engine == NULL)
        return;
    for (int i = 0; i < engine->table_count; i++)
        table_close(engine->tables[i]);
    free(engine->tables);
    free(engine->warnings);
    free(engine);
}

int engine_warning_count(const Engine *engine)
{
    return engine->warning_count;
}

const char *engine_warning(const Engine *engine, int index)
{
    return engine->warnings[index];
}

/* ---- 실행 ---- */

static double elapsed_ms_since(const struct timespec *start)
{
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    return (double)(end.tv_sec - start->tv_sec) * MS_PER_SECOND +
           (double)(end.tv_nsec - start->tv_nsec) / NS_PER_MS;
}

/* 파싱된 문장 하나를 검증하고 실행한다. 검증에서 실패하면 테이블을 건드리지 않는다 */
static Result execute_statement(Engine *engine, const Statement *stmt)
{
    Plan plan;
    char message[MAX_MESSAGE];
    ErrorCode error = validator_check(stmt, engine->tables, engine->table_count, &plan, message);
    if (error != ERR_NONE)
        return result_error(error, stmt->line, "%s", message);
    return executor_run(stmt, &plan);
}

Result engine_execute(Engine *engine, const char *sql, const Value *params, int param_count)
{
    (void)params;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    if (param_count > 0) /* TODO(S42): 바인딩 */
        return result_error(ERR_UNSUPPORTED, 0, "parameter binding is not supported yet");

    Statement stmt;
    ParseError parse_error;
    Result result;
    ErrorCode error = parser_parse_single(sql, strlen(sql), &stmt, &parse_error);
    if (error != ERR_NONE)
        result = result_error(error, parse_error.line, "%s", parse_error.message);
    else
        result = execute_statement(engine, &stmt);
    statement_free(&stmt);
    result.elapsed_ms = elapsed_ms_since(&start);
    return result;
}

/* ---- 스크립트 ---- */

static void deliver(ResultHandler on_result, void *ctx, Result *result, int stmt_index)
{
    if (on_result != NULL)
        on_result(result, stmt_index, ctx);
    result_free(result); /* 콜백이 끝나면 엔진이 해제한다 */
}

/* 스크립트 모드 파서에서 꺼낸 문장 하나의 결과. 파싱 에러면 파서의 줄·메시지를 담는다 */
static Result run_next(Engine *engine, Parser *parser, const Statement *stmt, ErrorCode error)
{
    if (error != ERR_NONE)
        return result_error(error, parser_error_line(parser), "%s", parser_error_message(parser));
    return execute_statement(engine, stmt);
}

int engine_execute_script(Engine *engine, const char *script, ResultHandler on_result, void *ctx)
{
    Parser *parser = parser_create(script, strlen(script));
    if (parser == NULL) {
        Result result = result_error(ERR_INTERNAL, 0, "out of memory");
        deliver(on_result, ctx, &result, 0);
        return 1;
    }
    int error_count = 0;
    Statement stmt;
    ErrorCode error;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    /* 에러 문장은 parser 가 ; 까지 건너뛰어 두므로 다음 호출이 곧 다음 문장이다 */
    while (parser_next_statement(parser, &stmt, &error)) {
        Result result = run_next(engine, parser, &stmt, error);
        statement_free(&stmt);
        result.elapsed_ms = elapsed_ms_since(&start);
        if (result.error != ERR_NONE)
            error_count++;
        deliver(on_result, ctx, &result, parser_statement_number(parser));
        clock_gettime(CLOCK_MONOTONIC, &start); /* 콜백(출력) 시간은 다음 문장에 넣지 않는다 */
    }
    parser_destroy(parser);
    return error_count;
}
