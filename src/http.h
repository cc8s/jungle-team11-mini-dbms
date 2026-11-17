#ifndef HTTP_H
#define HTTP_H

/* HTTP/1.x 요청 파싱. 파싱 함수는 소켓을 모르고 버퍼만 받는다 (소켓 읽기는 S33).
 * 지원 범위: 요청 줄, 헤더 (이름 대소문자 무시), Content-Length 본문. chunked 는 501. */

#include <stdbool.h>
#include <stddef.h>

#define HTTP_MAX_HEAD 8192    /* 요청 줄 + 헤더 + 빈 줄. 넘으면 431 */
#define HTTP_MAX_BODY 65536   /* 본문 (= SQL 문장 한도와 같은 크기). 넘으면 413 */
#define HTTP_MAX_METHOD 16    /* \0 포함 */
#define HTTP_MAX_PATH 2048    /* \0 포함, 쿼리 문자열을 뗀 경로. 넘으면 414 */

/* 파싱 결과. 성공은 0, 실패는 응답으로 보낼 HTTP 상태 코드 그대로 */
typedef enum {
    HTTP_PARSE_OK = 0,
    HTTP_STATUS_BAD_REQUEST = 400,
    HTTP_STATUS_LENGTH_REQUIRED = 411,
    HTTP_STATUS_PAYLOAD_TOO_LARGE = 413,
    HTTP_STATUS_URI_TOO_LONG = 414,
    HTTP_STATUS_HEADERS_TOO_LARGE = 431,
    HTTP_STATUS_NOT_IMPLEMENTED = 501,
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

#endif
