#include "api.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "parser.h"
#include "row.h"

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

char *api_error_json(const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    char *out = NULL;

    if (root && cJSON_AddFalseToObject(root, "ok") && cJSON_AddStringToObject(root, "error", msg)) {
        out = cJSON_PrintUnformatted(root);
    }
    cJSON_Delete(root);
    return out;
}

static char *fail(int *status, int code, const char *msg)
{
    *status = code;
    return api_error_json(msg);
}

/* 행 하나 → [값, 값, ...]. INT 는 숫자, VARCHAR 는 문자열 */
static cJSON *row_to_json(const struct result *res, const void *row)
{
    cJSON *arr = cJSON_CreateArray();
    char buf[SCHEMA_VARCHAR_MAX + 1];

    for (int i = 0; arr && i < res->column_count; i++) {
        int col = res->columns[i];
        cJSON *cell;
        if (res->schema->columns[col].type == COLUMN_INT) {
            /* JSON 숫자는 double 이라 2^53 을 넘는 INT 는 정밀도를 잃는다 (id 는 그만큼 커지지 않는다) */
            cell = cJSON_CreateNumber((double)row_get_int(res->schema, col, row));
        } else {
            row_get_string(res->schema, col, row, buf, sizeof(buf));
            cell = cJSON_CreateString(buf);
        }
        if (!cell || !cJSON_AddItemToArray(arr, cell)) {
            cJSON_Delete(cell);
            cJSON_Delete(arr);
            return NULL;
        }
    }
    return arr;
}

static bool add_select_fields(cJSON *root, const struct result *res)
{
    cJSON *columns = cJSON_AddArrayToObject(root, "columns");
    cJSON *rows = cJSON_AddArrayToObject(root, "rows");

    if (!columns || !rows) {
        return false;
    }
    for (int i = 0; i < res->column_count; i++) {
        cJSON *name = cJSON_CreateString(res->schema->columns[res->columns[i]].name);
        if (!name || !cJSON_AddItemToArray(columns, name)) {
            cJSON_Delete(name);
            return false;
        }
    }
    size_t shown = res->row_count < API_MAX_ROWS ? res->row_count : API_MAX_ROWS;
    for (size_t k = 0; k < shown; k++) {
        cJSON *row = row_to_json(res, result_row(res, k));
        if (!row || !cJSON_AddItemToArray(rows, row)) {
            cJSON_Delete(row);
            return false;
        }
    }
    return cJSON_AddNumberToObject(root, "row_count", (double)res->row_count) &&
           cJSON_AddBoolToObject(root, "truncated", res->row_count > API_MAX_ROWS) &&
           cJSON_AddStringToObject(root, "plan", result_plan_name(res->plan)) &&
           cJSON_AddNumberToObject(root, "rows_examined", (double)res->rows_examined);
}

static char *result_to_json(const struct result *res, double elapsed_ms)
{
    cJSON *root = cJSON_CreateObject();
    bool ok = root && cJSON_AddTrueToObject(root, "ok");

    if (ok && res->type == RESULT_INSERT) {
        ok = cJSON_AddNumberToObject(root, "inserted_id", (double)res->inserted_id) != NULL;
    } else if (ok) {
        ok = add_select_fields(root, res);
    }
    ok = ok && cJSON_AddNumberToObject(root, "elapsed_ms", elapsed_ms);

    char *out = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    return out;
}

/* cJSON 은 파싱할 때마다 전역 변수(global_error)에 오류 위치를 쓴다. 워커 여럿이 동시에 파싱하면
 * 그 전역에서 데이터 레이스가 난다(서버를 TSan 으로 띄워 동시 요청을 보내 찾음, cJSON.c:1110).
 * 외부 코드를 고치지 않고 파싱만 한 줄로 세운다. 파싱은 마이크로초 단위라 병목이 되지 않는다 */
static pthread_mutex_t g_json_parse_lock = PTHREAD_MUTEX_INITIALIZER;

/* 본문 JSON 에서 "sql" 문자열을 꺼내 dst 에 복사한다. 실패하면 오류 문구 */
static const char *extract_sql(const char *body, size_t body_len, char **dst)
{
    pthread_mutex_lock(&g_json_parse_lock);
    cJSON *root = cJSON_ParseWithLength(body, body_len);
    pthread_mutex_unlock(&g_json_parse_lock);
    const char *err = NULL;

    *dst = NULL;
    if (!root) {
        return "request body must be JSON like {\"sql\": \"...\"}";
    }
    const cJSON *sql = cJSON_GetObjectItemCaseSensitive(root, "sql");
    if (!cJSON_IsString(sql) || !sql->valuestring) {
        err = "\"sql\" must be a string";
    } else {
        *dst = strdup(sql->valuestring);
        if (!*dst) {
            err = "out of memory";
        }
    }
    cJSON_Delete(root);
    return err;
}

char *api_handle_query(struct executor *ex, const char *body, size_t body_len, int *status)
{
    char *sql;
    const char *bad = extract_sql(body, body_len, &sql);

    if (bad) {
        return fail(status, 400, bad);
    }

    struct statement stmt;
    char err[512];
    size_t consumed;
    enum parse_result pr = parser_parse(sql, &consumed, &stmt, err, sizeof(err));
    char *out;
    if (pr == PARSE_EMPTY) {
        out = fail(status, 400, "empty query");
    } else if (pr == PARSE_ERROR) {
        out = fail(status, 400, err);
    } else {
        struct statement extra;
        size_t more;
        if (parser_parse(sql + consumed, &more, &extra, err, sizeof(err)) != PARSE_EMPTY) {
            out = fail(status, 400, "only one statement per request");
        } else {
            struct result res;
            double start = now_ms();
            if (executor_execute(ex, &stmt, &res, err, sizeof(err)) < 0) {
                out = fail(status, 400, err);
            } else {
                double elapsed = now_ms() - start - res.table_open_ms;
                *status = 200;
                out = result_to_json(&res, elapsed);
                result_free(&res);
            }
        }
    }
    free(sql);
    return out;
}
