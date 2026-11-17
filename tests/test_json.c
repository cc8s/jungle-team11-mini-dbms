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
    TEST_DONE();
    return 0;
}
