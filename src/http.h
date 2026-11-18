#ifndef MINI_DBMS_HTTP_H
#define MINI_DBMS_HTTP_H

/* 최소 HTTP/1.1 (D006): 요청 줄, Content-Length, 본문만 본다. chunked, keep-alive 는 없다.
 * 응답마다 Connection: close 로 연결을 닫는다. */

#include <stddef.h>

#define HTTP_MAX_HEADER (8 * 1024)
#define HTTP_MAX_BODY   (1024 * 1024)

struct http_request {
    char method[8];
    char path[256];
    const char *body; /* 받은 버퍼 안을 가리킨다 (NUL 로 끝나지 않음) */
    size_t body_len;
};

enum http_parse_result {
    HTTP_PARSE_OK,
    HTTP_PARSE_INCOMPLETE, /* 더 받아야 한다 */
    HTTP_PARSE_BAD,        /* 400 */
    HTTP_PARSE_TOO_LARGE,  /* 헤더 431 / 본문 413 — status 로 구분 */
    HTTP_PARSE_UNSUPPORTED /* chunked 등 501 */
};

/* buf[0..len) 를 해석한다. OK 면 req 를 채운다. 실패하면 *status 에 돌려줄 HTTP 상태 코드 */
enum http_parse_result http_parse_request(const char *buf, size_t len, struct http_request *req,
                                          int *status);

/* fd 에서 요청 하나를 끝까지 읽는다. 성공 0 (*buf_out 은 호출한 쪽이 free).
 * 실패 -1: *status 가 0 이 아니면 그 상태로 오류 응답을 보내야 하고, 0 이면(연결 끊김·시간 초과) 그냥 닫는다 */
int http_read_request(int fd, char **buf_out, struct http_request *req, int *status);

/* 상태 줄 + Content-Type + Content-Length + Connection: close + 본문. 성공 0 */
int http_write_response(int fd, int status, const char *content_type, const char *body,
                        size_t body_len);

const char *http_status_text(int status);

#endif
