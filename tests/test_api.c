#include <unistd.h>

#include "api.h"
#include "cJSON.h"
#include "test.h"

static char data_dir[64];
static struct executor ex;
static int status;

/* body 를 보내고 응답 JSON 을 파싱해 돌려준다. 호출한 쪽이 cJSON_Delete */
static cJSON *query(const char *body)
{
    char *out = api_handle_query(&ex, body, strlen(body), &status);
    CHECK(out != NULL);
    cJSON *json = cJSON_Parse(out);
    CHECK(json != NULL);
    free(out);
    return json;
}

static cJSON *sql(const char *text)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "sql", text);
    char *body = cJSON_PrintUnformatted(req);
    cJSON *res = query(body);
    free(body);
    cJSON_Delete(req);
    return res;
}

static const char *str_field(const cJSON *j, const char *name)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, name);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static double num_field(const cJSON *j, const char *name)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, name);
    return cJSON_IsNumber(v) ? v->valuedouble : -1;
}

static bool is_ok(const cJSON *j)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "ok"));
}

static void setup(void)
{
    char path[128];

    strcpy(data_dir, "/tmp/minidb-api-XXXXXX");
    CHECK(mkdtemp(data_dir) != NULL);
    snprintf(path, sizeof(path), "%s/users.tbl", data_dir);
    unlink(path);
    CHECK(executor_init(&ex, "schema", data_dir) == 0);
}

/* TC-API-001 EP: INSERT → inserted_id, SELECT → columns·rows (INT 는 숫자, VARCHAR 는 문자열) (D006) */
static void test_insert_and_select(void)
{
    cJSON *r = sql("INSERT INTO users VALUES ('kim', 'k@x', 20)");
    CHECK(status == 200 && is_ok(r) && num_field(r, "inserted_id") == 1);
    CHECK(num_field(r, "elapsed_ms") >= 0);
    cJSON_Delete(r);

    r = sql("SELECT id, name FROM users WHERE id = 1");
    CHECK(status == 200 && is_ok(r));
    const cJSON *cols = cJSON_GetObjectItemCaseSensitive(r, "columns");
    CHECK(cJSON_GetArraySize(cols) == 2);
    CHECK_STR_EQ(cJSON_GetArrayItem(cols, 1)->valuestring, "name");
    const cJSON *row = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(r, "rows"), 0);
    CHECK(cJSON_IsNumber(cJSON_GetArrayItem(row, 0)) && cJSON_GetArrayItem(row, 0)->valueint == 1);
    CHECK(cJSON_IsString(cJSON_GetArrayItem(row, 1)));
    CHECK_STR_EQ(cJSON_GetArrayItem(row, 1)->valuestring, "kim");
    CHECK(num_field(r, "row_count") == 1);
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(r, "truncated")));
    CHECK_STR_EQ(str_field(r, "plan"), "index lookup");
    CHECK(num_field(r, "rows_examined") == 1);
    cJSON_Delete(r);
}

/* TC-API-002 EG: 따옴표·역슬래시·한글이 JSON 왕복에서 그대로 */
static void test_json_escaping(void)
{
    cJSON *r = sql("INSERT INTO users (name) VALUES ('a\"b\\c ''q'' 김')");
    CHECK(status == 200);
    cJSON_Delete(r);
    r = sql("SELECT name FROM users WHERE id = 2");
    const cJSON *row = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(r, "rows"), 0);
    CHECK_STR_EQ(cJSON_GetArrayItem(row, 0)->valuestring, "a\"b\\c 'q' 김");
    cJSON_Delete(r);
}

/* TC-API-003 DT: 요청 오류 → 400 과 이유 */
static void test_bad_requests(void)
{
    static const struct {
        const char *body;
        const char *error_part;
    } cases[] = {
        {"not json", "must be JSON"},
        {"{\"query\": \"SELECT * FROM users\"}", "\"sql\" must be a string"},
        {"{\"sql\": 42}", "\"sql\" must be a string"},
        {"{\"sql\": \"\"}", "empty query"},
        {"{\"sql\": \"  ;  \"}", "empty query"},
        {"{\"sql\": \"SELECT * FORM users\"}", "near 'FORM'"},
        {"{\"sql\": \"SELECT * FROM users; SELECT * FROM users\"}", "only one statement"},
        {"{\"sql\": \"SELECT * FROM nope\"}", "does not exist"},
        {"{\"sql\": \"INSERT INTO users (id) VALUES (5)\"}", "assigned automatically"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cJSON *r = query(cases[i].body);
        CHECK(status == 400 && !is_ok(r));
        if (!strstr(str_field(r, "error"), cases[i].error_part)) {
            fprintf(stderr, "  %s → %s\n", cases[i].body, str_field(r, "error"));
        }
        CHECK(strstr(str_field(r, "error"), cases[i].error_part) != NULL);
        cJSON_Delete(r);
    }
    /* 끝의 ; 하나는 문장 하나로 본다 */
    cJSON *r = query("{\"sql\": \"SELECT id FROM users WHERE id = 1;\"}");
    CHECK(status == 200);
    cJSON_Delete(r);
}

/* TC-API-004 BVA: 응답 행 상한 — 10,000 행은 그대로, 10,001 행이면 10,000 개만 담고 truncated */
static void test_row_limit(void)
{
    struct result res;
    struct statement stmt;
    char err[256];
    size_t consumed;

    /* 실행기로 바로 채운다 (JSON 왕복 1만 번은 느리다). 이미 2행이 있다 */
    CHECK(parser_parse("INSERT INTO users (name) VALUES ('x')", &consumed, &stmt, err,
                       sizeof(err)) == PARSE_OK);
    for (int i = 0; i < API_MAX_ROWS - 2; i++) {
        CHECK(executor_execute(&ex, &stmt, &res, err, sizeof(err)) == 0);
    }
    cJSON *r = sql("SELECT id FROM users");
    CHECK(num_field(r, "row_count") == API_MAX_ROWS);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(r, "rows")) == API_MAX_ROWS);
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(r, "truncated")));
    cJSON_Delete(r);

    CHECK(executor_execute(&ex, &stmt, &res, err, sizeof(err)) == 0);
    r = sql("SELECT id FROM users");
    CHECK(num_field(r, "row_count") == API_MAX_ROWS + 1);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(r, "rows")) == API_MAX_ROWS);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "truncated")));
    CHECK_STR_EQ(str_field(r, "plan"), "full scan");
    cJSON_Delete(r);
}

int main(void)
{
    setup();
    RUN(test_insert_and_select);
    RUN(test_json_escaping);
    RUN(test_bad_requests);
    RUN(test_row_limit);
    executor_close(&ex);
    TEST_DONE();
    return 0;
}
