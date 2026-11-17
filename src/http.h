#ifndef HTTP_H
#define HTTP_H

/* HTTP/1.x 요청 파싱과 소켓 I/O.
 * - 파싱 함수 (http_parse_*) 는 소켓을 모르고 버퍼만 받는다.
 * - 소켓 함수 (http_read_request, http_write_*) 는 부분 읽기·쓰기를 끝까지 반복하고, EINTR 은 다시 시도한다.
 * 지원 범위: 요청 줄, 헤더 (이름 대소문자 무시), Content-Length 본문. chunked 는 501.
 * 응답은 늘 Connection: close (요청 하나에 연결 하나). */

#include <stdbool.h>
#include <stddef.h>

#define HTTP_MAX_HEAD 8192    /* 요청 줄 + 헤더 + 빈 줄. 넘으면 431 */
#define HTTP_MAX_BODY 65536   /* 본문 (= SQL 문장 한도와 같은 크기). 넘으면 413 */
#define HTTP_MAX_METHOD 16    /* \0 포함 */
#define HTTP_MAX_PATH 2048    /* \0 포함, 쿼리 문자열을 뗀 경로. 넘으면 414 */
#define HTTP_REQUEST_BUFFER_SIZE (HTTP_MAX_HEAD + HTTP_MAX_BODY) /* 요청 하나를 담는 버퍼 */
#define HTTP_READ_TIMEOUT_MS 5000 /* 느린 클라이언트가 워커를 붙잡지 못하게 */

/* 파싱·읽기 결과이자 응답 상태 코드. 성공은 HTTP_PARSE_OK (0), 실패는 보낼 상태 코드 그대로.
 * HTTP_READ_CLOSED 는 상대가 끊었거나 읽기 에러라서 응답하지 않고 닫아야 한다는 뜻 */
typedef enum {
    HTTP_READ_CLOSED = -1,
    HTTP_PARSE_OK = 0,
    HTTP_STATUS_OK = 200,
    HTTP_STATUS_BAD_REQUEST = 400,
    HTTP_STATUS_NOT_FOUND = 404,
    HTTP_STATUS_METHOD_NOT_ALLOWED = 405,
    HTTP_STATUS_REQUEST_TIMEOUT = 408,
    HTTP_STATUS_LENGTH_REQUIRED = 411,
    HTTP_STATUS_PAYLOAD_TOO_LARGE = 413,
    HTTP_STATUS_URI_TOO_LONG = 414,
    HTTP_STATUS_HEADERS_TOO_LARGE = 431,
    HTTP_STATUS_INTERNAL_ERROR = 500,
    HTTP_STATUS_NOT_IMPLEMENTED = 501,
    HTTP_STATUS_SERVICE_UNAVAILABLE = 503,
    HTTP_STATUS_VERSION_NOT_SUPPORTED = 505,
} HttpStatus;

typedef struct {
    char method[HTTP_MAX_METHOD]; /* "GET", "POST" ... (대문자 그대로) */
    char path[HTTP_MAX_PATH];     /* "/query" — '?' 뒤의 쿼리 문자열은 버린다 */
    bool has_content_length;
    size_t content_length;        /* Content-Length 가 없으면 0 */
    const char *body;             /* 파싱한 버퍼 안을 가리킨다 (버퍼보다 오래 쓰면 안 됨) */
    size_t body_len;              /* = content_length. 그 뒤에 더 온 바이트는 쓰지 않는다 */
} HttpRequest;

/* buf[0..len) 에서 헤더 끝 빈 줄 ("\r\n\r\n", CR 이 빠진 "\n\n" 도 허용) 을 찾는다.
 * 찾으면 빈 줄까지 포함한 헤더 길이를 *out_head_len 에 쓰고 true. 아직 없으면 false. */
bool http_find_head_end(const char *buf, size_t len, size_t *out_head_len);

/* 헤더 영역 head[0..head_len) (http_find_head_end 가 찾은 길이) 을 파싱한다. body 는 채우지 않는다.
 * 400 형식 오류·헤더 안 \0·Content-Length 가 숫자가 아님·서로 다른 Content-Length 두 개,
 * 411 POST 인데 Content-Length 없음, 413 Content-Length > HTTP_MAX_BODY (본문을 읽기 전에 거절),
 * 414 경로가 너무 김, 431 head_len > HTTP_MAX_HEAD, 501 Transfer-Encoding, 505 HTTP/1.0·1.1 외 버전 */
HttpStatus http_parse_head(const char *head, size_t head_len, HttpRequest *out_request);

/* 요청 전체가 든 버퍼를 파싱한다: 헤더 끝 찾기 → http_parse_head → 본문 Content-Length 바이트.
 * 헤더 끝이 없으면 431 (HTTP_MAX_HEAD 이상 읽었는데도 없음) 또는 400 (요청이 덜 옴),
 * 본문이 Content-Length 보다 짧으면 400. */
HttpStatus http_parse_request(const char *buf, size_t len, HttpRequest *out_request);

/* ---- 소켓 ---- */

/* fd 의 읽기 타임아웃 (SO_RCVTIMEO). 기다리는 recv 하나하나에 적용된다. 실패하면 false */
bool http_set_read_timeout(int fd, int timeout_ms);

/* fd 에서 요청 하나를 buf (capacity 바이트, HTTP_REQUEST_BUFFER_SIZE 권장) 에 읽고 파싱한다.
 * 헤더 끝까지 읽은 뒤 http_parse_head 로 먼저 검사하므로 413 등은 본문을 받기 전에 돌아온다.
 * 본문은 Content-Length 바이트까지만 읽는다 (더 보낸 것은 읽지 않음). out_request->body 는 buf 안을 가리킨다.
 * 반환: HTTP_PARSE_OK, 파싱 에러 상태 코드, 408 (타임아웃), HTTP_READ_CLOSED (상대가 끊음·읽기 에러).
 * 요청이 capacity 보다 크면 413. fd 는 닫지 않는다. */
HttpStatus http_read_request(int fd, char *buf, size_t capacity, HttpRequest *out_request);

/* 상태 코드의 사유 문구 ("OK", "Bad Request" ...). 표에 없으면 "Unknown" */
const char *http_reason_phrase(int status);

/* 보낼 응답. extra_headers 는 "Allow: GET\r\n" 처럼 줄마다 \r\n 으로 끝나는 문자열 (없으면 NULL) */
typedef struct {
    int status;
    const char *content_type;
    const char *extra_headers;
    const char *body;
    size_t body_len;
} HttpResponse;

/* buf 의 len 바이트를 끝까지 쓴다 (부분 쓰기 반복, EINTR 재시도).
 * 상대가 끊었으면 (EPIPE) false — SIGPIPE 로 프로세스가 죽지 않게 MSG_NOSIGNAL 로 보낸다 */
bool http_write_all(int fd, const void *buf, size_t len);

/* 상태 줄, Content-Type, Content-Length, Connection: close, extra_headers, 빈 줄, 본문을 쓴다.
 * 헤더와 본문을 시스템 콜 한 번 (sendmsg) 에 보낸다. 실패하면 false */
bool http_write_response(int fd, const HttpResponse *response);

#endif
