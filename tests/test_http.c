#include "http.h"
#include "test.h"

static struct http_request req;
static int status;

static enum http_parse_result parse(const char *raw)
{
    return http_parse_request(raw, strlen(raw), &req, &status);
}

/* TC-HTTP-001 EP: POST 본문과 Content-Length (헤더 이름 대소문자 무시) */
static void test_post_with_body(void)
{
    CHECK(parse("POST /query HTTP/1.1\r\nHost: x\r\ncontent-length: 5\r\n\r\nhello") ==
          HTTP_PARSE_OK);
    CHECK_STR_EQ(req.method, "POST");
    CHECK_STR_EQ(req.path, "/query");
    CHECK(req.body_len == 5 && memcmp(req.body, "hello", 5) == 0);
}

/* TC-HTTP-002 EP: 본문 없는 GET, HTTP/1.0 */
static void test_get_without_body(void)
{
    CHECK(parse("GET /health HTTP/1.0\r\n\r\n") == HTTP_PARSE_OK);
    CHECK_STR_EQ(req.method, "GET");
    CHECK(req.body_len == 0);
}

/* TC-HTTP-003 ST: 나뉘어 도착 — 헤더 중간, 본문 중간이면 더 받아야 한다 */
static void test_incomplete(void)
{
    CHECK(parse("POST /query HTTP/1.1\r\nContent-Le") == HTTP_PARSE_INCOMPLETE);
    CHECK(parse("POST /query HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc") == HTTP_PARSE_INCOMPLETE);
    CHECK(parse("POST /query HTTP/1.1\r\nContent-Length: 3\r\n\r\nabcEXTRA") == HTTP_PARSE_OK);
    CHECK(req.body_len == 3); /* 남는 바이트는 무시 */
}

/* TC-HTTP-004 EG: 잘못된 요청 줄과 헤더 → 400 */
static void test_bad_requests(void)
{
    static const char *bad[] = {
        "GARBAGE\r\n\r\n",
        "POST query HTTP/1.1\r\n\r\n",          /* 경로가 / 로 시작하지 않음 */
        "POST /query HTTP/2.0\r\n\r\n",         /* 버전 */
        "POST /query\r\n\r\n",                  /* 버전 없음 */
        "POST  /query HTTP/1.1\r\n\r\n",        /* 빈 경로 */
        "POST /q HTTP/1.1\r\nNoColon\r\n\r\n",  /* 헤더에 : 없음 */
        "POST /q HTTP/1.1\r\nContent-Length: 1x\r\n\r\n",
        "POST /q HTTP/1.1\r\nContent-Length:\r\n\r\n",
        "TOOLONGMETHOD /q HTTP/1.1\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(parse(bad[i]) == HTTP_PARSE_BAD);
        CHECK(status == 400);
    }
}

/* TC-HTTP-005 BVA: 본문 1MB 는 받고 1MB + 1 은 413, 헤더 8KB 를 넘도록 끝이 없으면 431 */
static void test_size_limits(void)
{
    char raw[128];

    snprintf(raw, sizeof(raw), "POST /q HTTP/1.1\r\nContent-Length: %d\r\n\r\n", HTTP_MAX_BODY);
    CHECK(parse(raw) == HTTP_PARSE_INCOMPLETE);
    snprintf(raw, sizeof(raw), "POST /q HTTP/1.1\r\nContent-Length: %d\r\n\r\n", HTTP_MAX_BODY + 1);
    CHECK(parse(raw) == HTTP_PARSE_TOO_LARGE && status == 413);

    static char big[HTTP_MAX_HEADER + 64];
    strcpy(big, "GET / HTTP/1.1\r\nX: ");
    memset(big + strlen(big), 'a', HTTP_MAX_HEADER);
    big[sizeof(big) - 1] = '\0';
    CHECK(http_parse_request(big, strlen(big), &req, &status) == HTTP_PARSE_TOO_LARGE);
    CHECK(status == 431);
}

/* TC-HTTP-006 EG: chunked 는 지원하지 않는다 → 501 */
static void test_chunked_unsupported(void)
{
    CHECK(parse("POST /q HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n") ==
          HTTP_PARSE_UNSUPPORTED);
    CHECK(status == 501);
}

/* TC-HTTP-007 EG: 헤더 안의 NUL 바이트 — 길이로 줄을 찾으므로 넘어가지 않는다 */
static void test_nul_in_header(void)
{
    static const char raw[] = "POST /q HTTP/1.1\r\nX: a\0b\r\nContent-Length: 2\r\n\r\nhi";

    CHECK(http_parse_request(raw, sizeof(raw) - 1, &req, &status) == HTTP_PARSE_OK);
    CHECK(req.body_len == 2 && memcmp(req.body, "hi", 2) == 0);
}

int main(void)
{
    RUN(test_post_with_body);
    RUN(test_get_without_body);
    RUN(test_incomplete);
    RUN(test_bad_requests);
    RUN(test_size_limits);
    RUN(test_chunked_unsupported);
    RUN(test_nul_in_header);
    TEST_DONE();
    return 0;
}
