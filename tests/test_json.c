#include "test.h"

#include <stdint.h>

#include "json.h"

#define DEEP_NESTING 1000

static JsonStatus parse_text(const char *text, QueryRequest *out_request)
{
    char message[MAX_MESSAGE];
    JsonStatus status = json_parse_query(text, strlen(text), out_request, message);
    CHECK(status == JSON_OK ? message[0] == '\0' : message[0] != '\0'); /* 실패면 이유가 있다 */
    return status;
}

/* 실패해야 하는 입력: 상태를 돌려주고, 요청이 비어 있는지도 확인한다 */
static JsonStatus status_of(const char *text)
{
    QueryRequest request;
    JsonStatus status = parse_text(text, &request);
    if (status != JSON_OK) {
        CHECK(request.sql == NULL);
        CHECK(request.param_count == 0);
    }
    query_request_free(&request);
    return status;
}

static void check_string_value(const Value *value, const char *expected, size_t expected_len)
{
    CHECK(value->type == VAL_STRING);
    CHECK(value->s.len == expected_len);
    CHECK(memcmp(value->s.ptr, expected, expected_len) == 0);
    CHECK(value->s.ptr[expected_len] == '\0');
}

/* T34-01 */
static void test_sql_only(void)
{
    QueryRequest request;
    CHECK(parse_text("{\"sql\":\"SELECT * FROM users\"}", &request) == JSON_OK);
    CHECK_STR_EQ(request.sql, "SELECT * FROM users");
    CHECK(request.sql_len == strlen("SELECT * FROM users"));
    CHECK(request.param_count == 0);
    query_request_free(&request);
    query_request_free(&request); /* 두 번 불러도 안전 */
}

/* T34-02 */
static void test_params(void)
{
    QueryRequest request;
    const char *text = "{\"sql\":\"SELECT * FROM users WHERE id = ? AND x = ?\","
                       "\"params\":[3,\"kim\",-2147483648,2147483647,\"\"]}";
    CHECK(parse_text(text, &request) == JSON_OK);
    CHECK(request.param_count == 5);
    CHECK(request.params[0].type == VAL_INT && request.params[0].i == 3);
    check_string_value(&request.params[1], "kim", 3);
    CHECK(request.params[2].type == VAL_INT && request.params[2].i == INT32_MIN);
    CHECK(request.params[3].type == VAL_INT && request.params[3].i == INT32_MAX);
    check_string_value(&request.params[4], "", 0);
    query_request_free(&request);
}

/* T34-03 */
static void test_escapes(void)
{
    QueryRequest request;
    CHECK(parse_text("{\"sql\":\"a\\\"b\\\\c\\nd\\/e\\tf\\b\\f\\r\"}", &request) == JSON_OK);
    CHECK_STR_EQ(request.sql, "a\"b\\c\nd/e\tf\b\f\r");
    query_request_free(&request);
}

/* T34-04 */
static void test_unicode(void)
{
    QueryRequest request;
    /* 날것 UTF-8, \uXXXX (3 바이트), \u00e9 (2 바이트), 서로게이트 쌍 (4 바이트 이모지) */
    const char *text = "{\"sql\":\"김\",\"params\":[\"\\uae40\",\"\\u00e9\",\"\\ud83d\\ude00\"]}";
    CHECK(parse_text(text, &request) == JSON_OK);
    CHECK_STR_EQ(request.sql, "\xea\xb9\x80"); /* 김 */
    check_string_value(&request.params[0], "\xea\xb9\x80", 3);
    check_string_value(&request.params[1], "\xc3\xa9", 2);
    check_string_value(&request.params[2], "\xf0\x9f\x98\x80", 4);
    query_request_free(&request);
}

/* T34-05 */
static void test_whitespace_and_key_order(void)
{
    QueryRequest request;
    CHECK(parse_text(" \n{ \"params\" : [ ] ,\t\"sql\" : \"x\" }\r\n ", &request) == JSON_OK);
    CHECK_STR_EQ(request.sql, "x");
    CHECK(request.param_count == 0);
    query_request_free(&request);
}

/* T34-06 */
static void test_unknown_keys_ignored(void)
{
    QueryRequest request;
    const char *text = "{\"debug\":true,\"sql\":\"x\",\"meta\":{\"a\":[1,2.5e3,null,false,"
                       "{\"b\":\"c\"}]},\"n\":-0.5}";
    CHECK(parse_text(text, &request) == JSON_OK);
    CHECK_STR_EQ(request.sql, "x");
    query_request_free(&request);
}

/* E34-01 */
static void test_empty_body(void)
{
    CHECK(status_of("") == JSON_BAD_REQUEST);
    CHECK(status_of("   ") == JSON_BAD_REQUEST);
}

/* E34-02 */
static void test_broken_json(void)
{
    const char *texts[] = {
        "{\"sql\":",        "{\"sql\":\"x\"",      "{\"sql\" \"x\"}",   "{sql:\"x\"}",
        "{\"sql\":\"x\",}", "{\"sql\":\"x\"",       "{\"sql\":\"abc",    "{,\"sql\":\"x\"}",
        "{\"sql\":\"x\",\"params\":[1,]}",          "{\"sql\":\"x\",\"params\":[01]}",
        "{\"sql\":\"x\",\"params\":[1 2]}",         "{\"sql\":\"x\",\"a\":tru}",
        "{\"sql\":\"\\q\"}", "{\"sql\":\"\\u12g4\"}", "{\"sql\":\"x\",\"params\":[-]}",
    };
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++)
        CHECK(status_of(texts[i]) == JSON_BAD_REQUEST);
}

/* E34-03 */
static void test_top_level_not_object(void)
{
    CHECK(status_of("[\"sql\"]") == JSON_BAD_REQUEST);
    CHECK(status_of("\"sql\"") == JSON_BAD_REQUEST);
    CHECK(status_of("3") == JSON_BAD_REQUEST);
}

/* E34-04 */
static void test_missing_or_wrong_sql(void)
{
    CHECK(status_of("{}") == JSON_BAD_REQUEST);
    CHECK(status_of("{\"sql\":1}") == JSON_BAD_REQUEST);
    CHECK(status_of("{\"sql\":null}") == JSON_BAD_REQUEST);
    CHECK(status_of("{\"params\":[1]}") == JSON_BAD_REQUEST);
}

/* E34-05 */
static void test_params_not_array(void)
{
    CHECK(status_of("{\"sql\":\"x\",\"params\":3}") == JSON_BAD_REQUEST);
    CHECK(status_of("{\"sql\":\"x\",\"params\":{}}") == JSON_BAD_REQUEST);
}

/* E34-06 */
static void test_param_wrong_types(void)
{
    const char *texts[] = {
        "{\"sql\":\"x\",\"params\":[1.5]}",  "{\"sql\":\"x\",\"params\":[true]}",
        "{\"sql\":\"x\",\"params\":[null]}", "{\"sql\":\"x\",\"params\":[{}]}",
        "{\"sql\":\"x\",\"params\":[[1]]}",  "{\"sql\":\"x\",\"params\":[1e3]}",
    };
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++)
        CHECK(status_of(texts[i]) == JSON_BAD_PARAM);
    /* 문법 오류가 같이 있으면 문법 오류가 우선 */
    CHECK(status_of("{\"sql\":\"x\",\"params\":[true]") == JSON_BAD_REQUEST);
}

/* E34-07 */
static void test_param_out_of_range(void)
{
    CHECK(status_of("{\"sql\":\"x\",\"params\":[2147483648]}") == JSON_BAD_PARAM);
    CHECK(status_of("{\"sql\":\"x\",\"params\":[-2147483649]}") == JSON_BAD_PARAM);
    CHECK(status_of("{\"sql\":\"x\",\"params\":[99999999999999999999999999]}") ==
          JSON_BAD_PARAM); /* 아주 긴 수도 넘치지 않는다 */
}

/* "params":[1,1,...] (count 개) 인 요청 (호출한 쪽이 free) */
static char *build_params(int count)
{
    size_t capacity = 64 + (size_t)count * 2;
    char *text = malloc(capacity);
    CHECK(text != NULL);
    size_t len = (size_t)snprintf(text, capacity, "{\"sql\":\"x\",\"params\":[");
    for (int i = 0; i < count; i++)
        len += (size_t)snprintf(text + len, capacity - len, i == 0 ? "1" : ",1");
    snprintf(text + len, capacity - len, "]}");
    return text;
}

/* E34-08 */
static void test_too_many_params(void)
{
    char *text = build_params(JSON_MAX_PARAMS + 1);
    CHECK(status_of(text) == JSON_BAD_PARAM);
    free(text);
    text = build_params(JSON_MAX_PARAMS);
    QueryRequest request;
    CHECK(parse_text(text, &request) == JSON_OK);
    CHECK(request.param_count == JSON_MAX_PARAMS);
    query_request_free(&request);
    free(text);
}

/* E34-09 */
static void test_nul_in_string(void)
{
    CHECK(status_of("{\"sql\":\"x\",\"params\":[\"a\\u0000b\"]}") == JSON_BAD_PARAM);
    CHECK(status_of("{\"sql\":\"SELECT\\u0000x\"}") == JSON_BAD_REQUEST); /* sql 의 \0 은 잘림 */
}

/* E34-10 */
static void test_lone_surrogate(void)
{
    CHECK(status_of("{\"sql\":\"\\uD800\"}") == JSON_BAD_REQUEST);
    CHECK(status_of("{\"sql\":\"\\uDC00\"}") == JSON_BAD_REQUEST);
    CHECK(status_of("{\"sql\":\"\\uD800\\u0041\"}") == JSON_BAD_REQUEST);
    CHECK(status_of("{\"sql\":\"\\uD800x\"}") == JSON_BAD_REQUEST);
}

/* E34-11 */
static void test_raw_control_character(void)
{
    CHECK(status_of("{\"sql\":\"a\nb\"}") == JSON_BAD_REQUEST);
    CHECK(status_of("{\"sql\":\"a\tb\"}") == JSON_BAD_REQUEST);
}

/* E34-12 */
static void test_duplicate_keys(void)
{
    CHECK(status_of("{\"sql\":\"a\",\"sql\":\"b\"}") == JSON_BAD_REQUEST);
    CHECK(status_of("{\"sql\":\"a\",\"params\":[],\"params\":[]}") == JSON_BAD_REQUEST);
    CHECK(status_of("{\"sql\":\"a\",\"x\":1,\"x\":2}") == JSON_BAD_REQUEST);
    CHECK(status_of("{\"sql\":\"a\",\"s\\u0071l\":\"b\"}") == JSON_BAD_REQUEST); /* 디코딩 후 비교 */
}

/* E34-13 */
static void test_trailing_garbage(void)
{
    CHECK(status_of("{\"sql\":\"x\"} xyz") == JSON_BAD_REQUEST);
    CHECK(status_of("{\"sql\":\"x\"}{}") == JSON_BAD_REQUEST);
}

/* E34-14 */
static void test_deep_nesting(void)
{
    size_t prefix_len = strlen("{\"sql\":\"x\",\"a\":");
    char *text = malloc(prefix_len + 2 * DEEP_NESTING + 2);
    CHECK(text != NULL);
    memcpy(text, "{\"sql\":\"x\",\"a\":", prefix_len);
    memset(text + prefix_len, '[', DEEP_NESTING);
    memset(text + prefix_len + DEEP_NESTING, ']', DEEP_NESTING);
    memcpy(text + prefix_len + 2 * DEEP_NESTING, "}", 2);
    CHECK(status_of(text) == JSON_BAD_REQUEST);
    free(text);
    /* 한도 안의 중첩은 허용 */
    CHECK(status_of("{\"sql\":\"x\",\"a\":[[[[{\"b\":[[]]}]]]]}") == JSON_OK);
}

/* 본문 중간의 \0 바이트 (문자열 밖) */
static void test_nul_byte_in_body(void)
{
    const char text[] = "{\"sql\":\"x\"\0}";
    QueryRequest request;
    char message[MAX_MESSAGE];
    CHECK(json_parse_query(text, sizeof(text) - 1, &request, message) == JSON_BAD_REQUEST);
    query_request_free(&request);
}

/* ---- S35 응답 ---- */

#define USERS_RECORD_SIZE 40 /* id 4 + name 32 + age 4 */
#define NAME_SIZE 32
#define ROW_LIMIT 1000
#define WIDE_VARCHAR 1024
#define FIXED_ELAPSED_MS 0.25

/* users (id INT, name VARCHAR(32), age INT) 모양의 SELECT 결과를 만든다. rows 는 행 수만큼 할당 */
static Result make_users_result(int64_t row_count)
{
    Result result;
    result_init(&result);
    const char *names[] = {"id", "name", "age"};
    const ColumnType types[] = {COL_INT, COL_VARCHAR, COL_INT};
    const int sizes[] = {4, NAME_SIZE, 4};
    int offset = 0;
    for (int i = 0; i < 3; i++) {
        snprintf(result.columns[i].name, MAX_NAME, "%s", names[i]);
        result.columns[i].type = types[i];
        result.columns[i].size = sizes[i];
        result.columns[i].offset = offset;
        offset += sizes[i];
    }
    result.column_count = 3;
    result.record_size = USERS_RECORD_SIZE;
    result.row_count = row_count;
    result.rows = calloc((size_t)(row_count > 0 ? row_count : 1), USERS_RECORD_SIZE);
    CHECK(result.rows != NULL);
    result.elapsed_ms = FIXED_ELAPSED_MS;
    return result;
}

static void put_int(uint8_t *cell, int32_t value)
{
    uint32_t bits = (uint32_t)value;
    for (int i = 0; i < 4; i++)
        cell[i] = (uint8_t)((bits >> (8 * i)) & 0xFF);
}

static void put_user(Result *result, int64_t row, int32_t id, const char *name, int32_t age)
{
    uint8_t *record = result->rows + row * result->record_size;
    put_int(record, id);
    memset(record + 4, 0, NAME_SIZE);
    memcpy(record + 4, name, strnlen(name, NAME_SIZE));
    put_int(record + 4 + NAME_SIZE, age);
}

/* result 를 JSON 으로 써서 문자열로 돌려준다 (호출한 쪽이 free) */
static char *to_json(const Result *result, int64_t max_rows)
{
    JsonBuf buf;
    json_buf_init(&buf);
    CHECK(json_write_result(result, max_rows, &buf) == ERR_NONE);
    CHECK(!buf.is_out_of_memory);
    CHECK(strlen(buf.data) == buf.len);
    return buf.data; /* 소유권을 넘긴다 */
}

/* JSON 문자열 하나 (escaped) 를 {"sql": ...} 로 감싸 다시 파싱해서 원래 값이 나오는지 */
static void check_round_trip(const char *value, size_t len)
{
    JsonBuf buf;
    json_buf_init(&buf);
    json_buf_append_text(&buf, "{\"sql\":");
    json_buf_append_escaped(&buf, value, len);
    json_buf_append_text(&buf, "}");
    CHECK(!buf.is_out_of_memory);
    QueryRequest request;
    char message[MAX_MESSAGE];
    CHECK(json_parse_query(buf.data, buf.len, &request, message) == JSON_OK);
    CHECK(request.sql_len == len && memcmp(request.sql, value, len) == 0);
    query_request_free(&request);
    json_buf_free(&buf);
}

/* T35-01, T35-04 */
static void test_select_json(void)
{
    Result result = make_users_result(3);
    put_user(&result, 0, 1, "kim", 25);
    put_user(&result, 1, 2, "lee", 30);
    put_user(&result, 2, 3, "park", 22);
    result.used_index = true;
    char *json = to_json(&result, ROW_LIMIT);
    CHECK_STR_EQ(json, "{\"ok\":true,\"columns\":[\"id\",\"name\",\"age\"],"
                       "\"rows\":[[1,\"kim\",25],[2,\"lee\",30],[3,\"park\",22]],"
                       "\"row_count\":3,\"truncated\":false,\"used_index\":true,"
                       "\"elapsed_ms\":0.250}");
    free(json);
    result_free(&result);
}

/* T35-02 */
static void test_insert_json(void)
{
    Result result;
    result_init(&result);
    result.affected_rows = 1;
    result.last_insert_id = 7;
    result.elapsed_ms = FIXED_ELAPSED_MS;
    char *json = to_json(&result, ROW_LIMIT);
    CHECK_STR_EQ(json, "{\"ok\":true,\"affected_rows\":1,\"last_insert_id\":7,"
                       "\"elapsed_ms\":0.250}");
    free(json);
}

/* T35-03, E35-02 */
static void test_error_json(void)
{
    Result result = result_error(ERR_UNKNOWN_COLUMN, 1, "Unknown column 'nmae' in table 'users'");
    char *json = to_json(&result, ROW_LIMIT);
    CHECK_STR_EQ(json, "{\"ok\":false,\"error\":{\"code\":\"UNKNOWN_COLUMN\","
                       "\"message\":\"Unknown column 'nmae' in table 'users'\"}}");
    free(json);
    result = result_error(ERR_SYNTAX, 1, "near \"it's\" \\ here");
    json = to_json(&result, ROW_LIMIT);
    CHECK(strstr(json, "\"message\":\"near \\\"it's\\\" \\\\ here\"") != NULL);
    free(json);
}

/* E35-01 */
static void test_escape_round_trip(void)
{
    const char *values[] = {"a\"b", "back\\slash", "line\nbreak", "tab\there", "\r\b\f",
                            "\x01\x1f", "</script>", "plain"};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
        check_round_trip(values[i], strlen(values[i]));
    JsonBuf buf;
    json_buf_init(&buf);
    json_buf_append_escaped(&buf, "a\"\\\n\t\x01", 6);
    CHECK_STR_EQ(buf.data, "\"a\\\"\\\\\\n\\t\\u0001\"");
    json_buf_free(&buf);
}

/* E35-03 */
static void test_zero_rows(void)
{
    Result result = make_users_result(0);
    char *json = to_json(&result, ROW_LIMIT);
    CHECK(strstr(json, "\"rows\":[],\"row_count\":0,\"truncated\":false") != NULL);
    free(json);
    result_free(&result);
}

static int64_t count_rows_in_json(const char *json)
{
    int64_t count = 0;
    for (const char *p = strstr(json, "\"rows\":["); p != NULL && *p != '\0'; p++) {
        if (p[0] == '[' && p[1] != '[' && p[-1] != ':')
            count++;
        if (p[0] == ']' && p[1] == ',' && p[2] == '"')
            break;
    }
    return count;
}

/* E35-04, E35-05 */
static void test_row_limit(void)
{
    int64_t counts[] = {ROW_LIMIT + 1, ROW_LIMIT};
    for (int k = 0; k < 2; k++) {
        Result result = make_users_result(counts[k]);
        for (int64_t row = 0; row < counts[k]; row++)
            put_user(&result, row, (int32_t)row + 1, "u", 1);
        char *json = to_json(&result, ROW_LIMIT);
        CHECK(count_rows_in_json(json) == ROW_LIMIT);
        char expected[64];
        snprintf(expected, sizeof(expected), "\"row_count\":%lld,\"truncated\":%s",
                 (long long)counts[k], counts[k] > ROW_LIMIT ? "true" : "false");
        CHECK(strstr(json, expected) != NULL);
        free(json);
        result_free(&result);
    }
}

/* E35-06, E35-07 */
static void test_korean_and_extreme_ints(void)
{
    Result result = make_users_result(2);
    put_user(&result, 0, INT32_MIN, "김철수", -1);
    put_user(&result, 1, INT32_MAX, "", 0);
    char *json = to_json(&result, ROW_LIMIT);
    CHECK(strstr(json, "[[-2147483648,\"김철수\",-1],[2147483647,\"\",0]]") != NULL);
    free(json);
    result_free(&result);
}

/* E35-08 */
static void test_wide_rows_grow_buffer(void)
{
    Result result;
    result_init(&result);
    snprintf(result.columns[0].name, MAX_NAME, "memo");
    result.columns[0].type = COL_VARCHAR;
    result.columns[0].size = WIDE_VARCHAR;
    result.column_count = 1;
    result.record_size = WIDE_VARCHAR;
    result.row_count = ROW_LIMIT;
    result.rows = malloc((size_t)ROW_LIMIT * WIDE_VARCHAR);
    CHECK(result.rows != NULL);
    memset(result.rows, 'z', (size_t)ROW_LIMIT * WIDE_VARCHAR); /* 칸이 꽉 차 \0 이 없다 */
    char *json = to_json(&result, ROW_LIMIT);
    CHECK(strlen(json) > (size_t)ROW_LIMIT * WIDE_VARCHAR);
    CHECK(strstr(json, "\"truncated\":false") != NULL);
    free(json);
    result_free(&result);
}

int main(void)
{
    printf("test_json\n");
    RUN(test_sql_only);
    RUN(test_params);
    RUN(test_escapes);
    RUN(test_unicode);
    RUN(test_whitespace_and_key_order);
    RUN(test_unknown_keys_ignored);
    RUN(test_empty_body);
    RUN(test_broken_json);
    RUN(test_top_level_not_object);
    RUN(test_missing_or_wrong_sql);
    RUN(test_params_not_array);
    RUN(test_param_wrong_types);
    RUN(test_param_out_of_range);
    RUN(test_too_many_params);
    RUN(test_nul_in_string);
    RUN(test_lone_surrogate);
    RUN(test_raw_control_character);
    RUN(test_duplicate_keys);
    RUN(test_trailing_garbage);
    RUN(test_deep_nesting);
    RUN(test_nul_byte_in_body);
    RUN(test_select_json);
    RUN(test_insert_json);
    RUN(test_error_json);
    RUN(test_escape_round_trip);
    RUN(test_zero_rows);
    RUN(test_row_limit);
    RUN(test_korean_and_extreme_ints);
    RUN(test_wide_rows_grow_buffer);
    TEST_DONE();
    return 0;
}
