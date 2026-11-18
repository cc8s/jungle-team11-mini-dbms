#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define HTTP_READ_CHUNK 4096

const char *http_status_text(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 413:
        return "Payload Too Large";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 503:
        return "Service Unavailable";
    default:
        return "Unknown";
    }
}

/* 헤더 끝(\r\n\r\n) 바로 뒤 위치. 없으면 NULL */
static const char *find_header_end(const char *buf, size_t len)
{
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return buf + i + 4;
        }
    }
    return NULL;
}

/* "METHOD SP PATH SP HTTP/1.x" */
static bool parse_request_line(const char *line, size_t len, struct http_request *req)
{
    const char *sp1 = memchr(line, ' ', len);
    if (!sp1 || sp1 == line || (size_t)(sp1 - line) >= sizeof(req->method)) {
        return false;
    }
    const char *rest = sp1 + 1;
    const char *sp2 = memchr(rest, ' ', len - (size_t)(rest - line));
    if (!sp2 || sp2 == rest || (size_t)(sp2 - rest) >= sizeof(req->path) || *rest != '/') {
        return false;
    }
    size_t ver_len = len - (size_t)(sp2 + 1 - line);
    if (ver_len != 8 || strncmp(sp2 + 1, "HTTP/1.", 7) != 0) {
        return false;
    }
    memcpy(req->method, line, (size_t)(sp1 - line));
    req->method[sp1 - line] = '\0';
    memcpy(req->path, rest, (size_t)(sp2 - rest));
    req->path[sp2 - rest] = '\0';
    return true;
}

/* 헤더 한 줄 "Name: value" 에서 Content-Length 와 Transfer-Encoding 만 본다 */
static enum http_parse_result parse_header_line(const char *line, size_t len, long long *content_length,
                                                int *status)
{
    const char *colon = memchr(line, ':', len);
    if (!colon || colon == line) {
        *status = 400;
        return HTTP_PARSE_BAD;
    }
    size_t name_len = (size_t)(colon - line);
    const char *value = colon + 1;
    const char *end = line + len;
    while (value < end && (*value == ' ' || *value == '\t')) {
        value++;
    }

    if (name_len == 14 && strncasecmp(line, "Content-Length", 14) == 0) {
        long long n = 0;
        if (value == end) {
            *status = 400;
            return HTTP_PARSE_BAD;
        }
        for (const char *p = value; p < end; p++) {
            if (!isdigit((unsigned char)*p)) {
                *status = 400;
                return HTTP_PARSE_BAD;
            }
            n = n * 10 + (*p - '0');
            if (n > HTTP_MAX_BODY) {
                *status = 413;
                return HTTP_PARSE_TOO_LARGE;
            }
        }
        *content_length = n;
    } else if (name_len == 17 && strncasecmp(line, "Transfer-Encoding", 17) == 0) {
        *status = 501;
        return HTTP_PARSE_UNSUPPORTED;
    }
    return HTTP_PARSE_OK;
}

enum http_parse_result http_parse_request(const char *buf, size_t len, struct http_request *req,
                                          int *status)
{
    *status = 0;
    if (!buf) {
        *status = 400;
        return HTTP_PARSE_BAD;
    }
    const char *body = find_header_end(buf, len);
    if (!body) {
        if (len > HTTP_MAX_HEADER) {
            *status = 431;
            return HTTP_PARSE_TOO_LARGE;
        }
        return HTTP_PARSE_INCOMPLETE;
    }

    /* 줄 찾기는 길이를 아는 memmem 으로 한다. 헤더 안에 NUL 바이트가 섞여도 넘어가지 않는다 */
    const char *header_end = body - 2; /* 마지막 빈 줄의 \r\n 자리 */
    const char *line = buf;
    const char *eol = memmem(line, (size_t)(body - line), "\r\n", 2);
    if (!eol || eol > header_end || !parse_request_line(line, (size_t)(eol - line), req)) {
        *status = 400;
        return HTTP_PARSE_BAD;
    }

    long long content_length = 0;
    for (line = eol + 2; line < header_end; line = eol + 2) {
        eol = memmem(line, (size_t)(body - line), "\r\n", 2);
        if (!eol) {
            *status = 400;
            return HTTP_PARSE_BAD;
        }
        enum http_parse_result r =
            parse_header_line(line, (size_t)(eol - line), &content_length, status);
        if (r != HTTP_PARSE_OK) {
            return r;
        }
    }

    size_t have = len - (size_t)(body - buf);
    if (have < (size_t)content_length) {
        return HTTP_PARSE_INCOMPLETE;
    }
    req->body = body;
    req->body_len = (size_t)content_length;
    return HTTP_PARSE_OK;
}

int http_read_request(int fd, char **buf_out, struct http_request *req, int *status)
{
    size_t cap = HTTP_READ_CHUNK;
    size_t len = 0;
    char *buf = malloc(cap + 1);

    *status = 0;
    *buf_out = NULL;
    while (buf) {
        if (len == cap) {
            if (cap >= HTTP_MAX_HEADER + HTTP_MAX_BODY) {
                *status = 413;
                break;
            }
            cap *= 2;
            char *grown = realloc(buf, cap + 1);
            if (!grown) {
                break;
            }
            buf = grown;
        }
        ssize_t n = recv(fd, buf + len, cap - len, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) { /* 끊김, 또는 SO_RCVTIMEO 시간 초과 */
            break;
        }
        len += (size_t)n;
        buf[len] = '\0'; /* strstr 이 버퍼 끝을 넘지 않게 */

        enum http_parse_result r = http_parse_request(buf, len, req, status);
        if (r == HTTP_PARSE_OK) {
            *buf_out = buf;
            return 0;
        }
        if (r != HTTP_PARSE_INCOMPLETE) {
            break;
        }
    }
    free(buf);
    return -1;
}

static int send_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, data, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

int http_write_response(int fd, int status, const char *content_type, const char *body,
                        size_t body_len)
{
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, http_status_text(status), content_type, body_len);

    if (send_all(fd, head, (size_t)n) < 0) {
        return -1;
    }
    return send_all(fd, body, body_len);
}
