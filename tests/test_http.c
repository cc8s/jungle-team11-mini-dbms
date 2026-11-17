#include "test.h"

#include <stdlib.h>
#include <string.h>

#include "http.h"

#define HEALTH_REQUEST "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"
#define QUERY_BODY "{\"sql\": \"SELECT * FROM users\"}" /* 30 바이트 */
#define QUERY_BODY_LEN 30
#define LARGE_HEADER_VALUE 9000

static HttpStatus parse_text(const char *text, HttpRequest *out_request)
{
    return http_parse_request(text, strlen(text), out_request);
}

static HttpStatus status_of(const char *text)
{
    HttpRequest request;
    return parse_text(text, &request);
}

/* "POST /query HTTP/1.1\r\n" + headers + "\r\n" + body_len 바이트 'x' 본문 (호출한 쪽이 free) */
static char *build_post(const char *headers, size_t body_len, size_t *out_len)
{
    const char *line = "POST /query HTTP/1.1\r\n";
    size_t head_len = strlen(line) + strlen(headers) + 2;
    char *text = malloc(head_len + body_len + 1);
    CHECK(text != NULL);
    snprintf(text, head_len + 1, "%s%s\r\n", line, headers);
    memset(text + head_len, 'x', body_len);
    text[head_len + body_len] = '\0';
    *out_len = head_len + body_len;
    return text;
}

/* T32-01 */
static void test_get_health(void)
{
    HttpRequest request;
    CHECK(parse_text(HEALTH_REQUEST, &request) == HTTP_PARSE_OK);
    CHECK_STR_EQ(request.method, "GET");
    CHECK_STR_EQ(request.path, "/health");
    CHECK(!request.has_content_length);
    CHECK(request.body_len == 0);
}

/* T32-02 */
static void test_post_with_body(void)
{
    const char *text = "POST /query HTTP/1.1\r\nContent-Length: 30\r\n\r\n" QUERY_BODY;
    HttpRequest request;
    CHECK(parse_text(text, &request) == HTTP_PARSE_OK);
    CHECK_STR_EQ(request.method, "POST");
    CHECK_STR_EQ(request.path, "/query");
    CHECK(request.content_length == QUERY_BODY_LEN);
    CHECK(request.body_len == QUERY_BODY_LEN);
    CHECK(memcmp(request.body, QUERY_BODY, QUERY_BODY_LEN) == 0);
}

/* T32-03 */
static void test_header_name_case(void)
{
    const char *text = "POST /query HTTP/1.1\r\ncontent-LENGTH: 30\r\n\r\n" QUERY_BODY;
    HttpRequest request;
    CHECK(parse_text(text, &request) == HTTP_PARSE_OK);
    CHECK(request.body_len == QUERY_BODY_LEN);
}

/* T32-04 */
static void test_header_value_spaces(void)
{
    const char *text = "POST /query HTTP/1.1\r\nContent-Length:   30  \r\n\r\n" QUERY_BODY;
    HttpRequest request;
    CHECK(parse_text(text, &request) == HTTP_PARSE_OK);
    CHECK(request.content_length == QUERY_BODY_LEN);
}

/* T32-05 */
static void test_http_1_0(void)
{
    CHECK(status_of("GET /health HTTP/1.0\r\n\r\n") == HTTP_PARSE_OK);
}

/* E32-01 */
static void test_post_without_length(void)
{
    CHECK(status_of("POST /query HTTP/1.1\r\nHost: a\r\n\r\n") == HTTP_STATUS_LENGTH_REQUIRED);
}

/* E32-02 */
static void test_bad_content_length(void)
{
    const char *values[] = {"abc", "-1", "1e3", "+3", "", "3 4", "0x10"};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        char text[128];
        snprintf(text, sizeof(text), "POST /query HTTP/1.1\r\nContent-Length: %s\r\n\r\n",
                 values[i]);
        CHECK(status_of(text) == HTTP_STATUS_BAD_REQUEST);
    }
}

/* E32-03: 본문을 보내지 않아도 헤더만 보고 거절한다 */
static void test_body_too_large(void)
{
    const char *head = "POST /query HTTP/1.1\r\nContent-Length: 65537\r\n\r\n";
    HttpRequest request;
    CHECK(http_parse_head(head, strlen(head), &request) == HTTP_STATUS_PAYLOAD_TOO_LARGE);
    CHECK(status_of(head) == HTTP_STATUS_PAYLOAD_TOO_LARGE);
    CHECK(status_of("POST /q HTTP/1.1\r\nContent-Length: 99999999999999999999999\r\n\r\n") ==
          HTTP_STATUS_PAYLOAD_TOO_LARGE); /* 엄청 큰 수도 넘치지 않고 413 */
}

/* E32-04 */
static void test_body_at_limit(void)
{
    size_t len = 0;
    char *text = build_post("Content-Length: 65536\r\n", HTTP_MAX_BODY, &len);
    HttpRequest request;
    CHECK(http_parse_request(text, len, &request) == HTTP_PARSE_OK);
    CHECK(request.body_len == HTTP_MAX_BODY);
    free(text);
}

/* E32-05 */
static void test_duplicate_content_length(void)
{
    CHECK(status_of("POST /q HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabcd") ==
          HTTP_STATUS_BAD_REQUEST);
    CHECK(status_of("POST /q HTTP/1.1\r\nContent-Length: 3\r\ncontent-length: 3\r\n\r\nabc") ==
          HTTP_PARSE_OK); /* 같은 값 두 번은 허용 */
}

/* E32-06 */
static void test_chunked_not_implemented(void)
{
    CHECK(status_of("POST /query HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n") ==
          HTTP_STATUS_NOT_IMPLEMENTED);
}

/* E32-07 */
static void test_headers_too_large(void)
{
    char *text = malloc(LARGE_HEADER_VALUE + 64);
    CHECK(text != NULL);
    int prefix = snprintf(text, 64, "GET /health HTTP/1.1\r\nX-Big: ");
    memset(text + prefix, 'a', LARGE_HEADER_VALUE);
    strcpy(text + prefix + LARGE_HEADER_VALUE, "\r\n\r\n");
    CHECK(status_of(text) == HTTP_STATUS_HEADERS_TOO_LARGE);
    text[prefix + LARGE_HEADER_VALUE] = '\0'; /* 끝 빈 줄 없이 8KB 를 넘음 */
    CHECK(status_of(text) == HTTP_STATUS_HEADERS_TOO_LARGE);
    free(text);
}

/* E32-07 경계: 헤더 영역이 정확히 8KB 면 허용 */
static void test_headers_at_limit(void)
{
    char text[HTTP_MAX_HEAD + 1];
    int prefix = snprintf(text, sizeof(text), "GET /health HTTP/1.1\r\nX-Pad: ");
    int pad = HTTP_MAX_HEAD - prefix - 4; /* 끝의 "\r\n\r\n" */
    memset(text + prefix, 'a', (size_t)pad);
    memcpy(text + prefix + pad, "\r\n\r\n", 5);
    CHECK(strlen(text) == HTTP_MAX_HEAD);
    CHECK(status_of(text) == HTTP_PARSE_OK);
}

/* E32-08 */
static void test_bad_request_line(void)
{
    const char *lines[] = {
        "GET\r\n\r\n",                 "GET /\r\n\r\n",
        "GET  /health HTTP/1.1\r\n\r\n", "GET /health  HTTP/1.1\r\n\r\n",
        "GET health HTTP/1.1\r\n\r\n", "get /health HTTP/1.1\r\n\r\n",
        " GET / HTTP/1.1\r\n\r\n",     "GET / HTTP/1.1 extra\r\n\r\n",
        "GET / FTP/1.1\r\n\r\n",       "\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        CHECK(status_of(lines[i]) == HTTP_STATUS_BAD_REQUEST);
}

/* E32-08 (헤더 줄 형식) */
static void test_bad_header_line(void)
{
    CHECK(status_of("GET / HTTP/1.1\r\nNoColon\r\n\r\n") == HTTP_STATUS_BAD_REQUEST);
    CHECK(status_of("GET / HTTP/1.1\r\n: empty-name\r\n\r\n") == HTTP_STATUS_BAD_REQUEST);
    CHECK(status_of("GET / HTTP/1.1\r\nBad Name: x\r\n\r\n") == HTTP_STATUS_BAD_REQUEST);
    CHECK(status_of("GET / HTTP/1.1\r\nA: b\r\n folded\r\n\r\n") == HTTP_STATUS_BAD_REQUEST);
}

/* E32-09 */
static void test_unsupported_version(void)
{
    CHECK(status_of("GET / HTTP/2.0\r\n\r\n") == HTTP_STATUS_VERSION_NOT_SUPPORTED);
    CHECK(status_of("GET / HTTP/0.9\r\n\r\n") == HTTP_STATUS_VERSION_NOT_SUPPORTED);
}

/* E32-10 */
static void test_bare_newlines(void)
{
    HttpRequest request;
    CHECK(parse_text("POST /query HTTP/1.1\nContent-Length: 30\n\n" QUERY_BODY, &request) ==
          HTTP_PARSE_OK);
    CHECK(memcmp(request.body, QUERY_BODY, QUERY_BODY_LEN) == 0);
}

/* E32-11 */
static void test_nul_in_header(void)
{
    const char text[] = "GET /health HTTP/1.1\r\nX-A: a\0b\r\n\r\n";
    HttpRequest request;
    CHECK(http_parse_request(text, sizeof(text) - 1, &request) == HTTP_STATUS_BAD_REQUEST);
}

/* E32-12 */
static void test_query_string_ignored(void)
{
    HttpRequest request;
    CHECK(parse_text("GET /query?x=1&y=2 HTTP/1.1\r\n\r\n", &request) == HTTP_PARSE_OK);
    CHECK_STR_EQ(request.path, "/query");
}

/* 그 밖: 본문이 덜 옴, 더 옴, 경로가 너무 김 */
static void test_body_length_mismatch(void)
{
    CHECK(status_of("POST /q HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc") ==
          HTTP_STATUS_BAD_REQUEST);
    HttpRequest request;
    CHECK(parse_text("POST /q HTTP/1.1\r\nContent-Length: 3\r\n\r\nabcdef", &request) ==
          HTTP_PARSE_OK);
    CHECK(request.body_len == 3 && memcmp(request.body, "abc", 3) == 0);
    CHECK(status_of("GET /health HTTP/1.1\r\nHost: a\r\n") == HTTP_STATUS_BAD_REQUEST); /* 빈 줄 없음 */
}

static void test_path_too_long(void)
{
    char text[HTTP_MAX_PATH + 64];
    int prefix = snprintf(text, sizeof(text), "GET /");
    memset(text + prefix, 'p', HTTP_MAX_PATH);
    strcpy(text + prefix + HTTP_MAX_PATH, " HTTP/1.1\r\n\r\n");
    CHECK(status_of(text) == HTTP_STATUS_URI_TOO_LONG);
}

int main(void)
{
    printf("test_http\n");
    RUN(test_get_health);
    RUN(test_post_with_body);
    RUN(test_header_name_case);
    RUN(test_header_value_spaces);
    RUN(test_http_1_0);
    RUN(test_post_without_length);
    RUN(test_bad_content_length);
    RUN(test_body_too_large);
    RUN(test_body_at_limit);
    RUN(test_duplicate_content_length);
    RUN(test_chunked_not_implemented);
    RUN(test_headers_too_large);
    RUN(test_headers_at_limit);
    RUN(test_bad_request_line);
    RUN(test_bad_header_line);
    RUN(test_unsupported_version);
    RUN(test_bare_newlines);
    RUN(test_nul_in_header);
    RUN(test_query_string_ignored);
    RUN(test_body_length_mismatch);
    RUN(test_path_too_long);
    TEST_DONE();
    return 0;
}
