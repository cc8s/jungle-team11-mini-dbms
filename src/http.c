#include "http.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>

#define HTTP_VERSION_PREFIX "HTTP/"
#define HTTP_VERSION_LEN 8 /* "HTTP/1.1" */
#define HEADER_CONTENT_LENGTH "content-length"
#define HEADER_TRANSFER_ENCODING "transfer-encoding"
#define METHOD_POST "POST"
#define DECIMAL_BASE 10
#define HEAD_END_MAX_LEN 4      /* "\r\n\r\n" */
#define RESPONSE_HEAD_MAX 1024  /* 상태 줄 + 고정 헤더 + extra_headers */
#define RESPONSE_PART_COUNT 2   /* 헤더, 본문 */
#define MS_PER_SECOND 1000
#define US_PER_MS 1000

/* 줄 하나: 버퍼 안의 시작과 길이 (끝의 \r\n 또는 \n 은 뺀 것) */
typedef struct {
    const char *text;
    size_t len;
} Line;

/* 헤더를 읽으며 모으는 상태 */
typedef struct {
    bool has_content_length;
    size_t content_length;
    bool has_transfer_encoding;
} HeaderState;

/* ---- 줄 나누기 ---- */

bool http_find_head_end(const char *buf, size_t len, size_t *out_head_len)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != '\n')
            continue;
        size_t next = i + 1;
        if (next < len && buf[next] == '\r')
            next++;
        if (next < len && buf[next] == '\n') {
            *out_head_len = next + 1;
            return true;
        }
    }
    return false;
}

/* *pos 부터 다음 줄을 꺼낸다. \n 앞의 \r 하나는 줄에서 뺀다 (CRLF 와 LF 를 같이 받는다) */
static bool next_line(const char *buf, size_t len, size_t *pos, Line *out_line)
{
    if (*pos >= len)
        return false;
    const char *start = buf + *pos;
    const char *newline = memchr(start, '\n', len - *pos);
    size_t line_len = newline == NULL ? len - *pos : (size_t)(newline - start);
    *pos += line_len + (newline != NULL);
    if (line_len > 0 && start[line_len - 1] == '\r')
        line_len--;
    out_line->text = start;
    out_line->len = line_len;
    return true;
}

/* ---- 요청 줄: METHOD SP TARGET SP VERSION ---- */

static bool is_valid_method(const char *text, size_t len)
{
    if (len == 0 || len >= HTTP_MAX_METHOD)
        return false;
    for (size_t i = 0; i < len; i++) {
        if (text[i] < 'A' || text[i] > 'Z')
            return false;
    }
    return true;
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* HTTP/1.0, HTTP/1.1 만 받는다. 형식은 맞는데 다른 버전 (HTTP/2.0) 이면 505, 형식이 틀리면 400 */
static HttpStatus check_version(const char *text, size_t len)
{
    size_t prefix_len = strlen(HTTP_VERSION_PREFIX);
    if (len != HTTP_VERSION_LEN || memcmp(text, HTTP_VERSION_PREFIX, prefix_len) != 0)
        return HTTP_STATUS_BAD_REQUEST;
    const char *number = text + prefix_len; /* "1.1" */
    if (!is_digit(number[0]) || number[1] != '.' || !is_digit(number[2]))
        return HTTP_STATUS_BAD_REQUEST;
    if (memcmp(text, "HTTP/1.0", len) == 0 || memcmp(text, "HTTP/1.1", len) == 0)
        return HTTP_PARSE_OK;
    return HTTP_STATUS_VERSION_NOT_SUPPORTED;
}

/* 대상 "/query?x=1" 에서 '?' 앞까지를 경로로 복사한다. '/' 로 시작해야 한다 */
static HttpStatus copy_path(const char *target, size_t len, HttpRequest *out_request)
{
    if (len == 0 || target[0] != '/')
        return HTTP_STATUS_BAD_REQUEST;
    const char *query = memchr(target, '?', len);
    size_t path_len = query == NULL ? len : (size_t)(query - target);
    if (path_len >= HTTP_MAX_PATH)
        return HTTP_STATUS_URI_TOO_LONG;
    memcpy(out_request->path, target, path_len);
    out_request->path[path_len] = '\0';
    return HTTP_PARSE_OK;
}

/* 구분자는 정확히 공백 하나씩: 공백 두 개면 빈 칸이 생겨 400 이 된다 */
static HttpStatus parse_request_line(Line line, HttpRequest *out_request)
{
    const char *first_space = memchr(line.text, ' ', line.len);
    if (first_space == NULL)
        return HTTP_STATUS_BAD_REQUEST;
    size_t method_len = (size_t)(first_space - line.text);
    if (!is_valid_method(line.text, method_len))
        return HTTP_STATUS_BAD_REQUEST;
    memcpy(out_request->method, line.text, method_len);
    out_request->method[method_len] = '\0';

    const char *target = first_space + 1;
    size_t rest_len = line.len - method_len - 1;
    const char *second_space = memchr(target, ' ', rest_len);
    if (second_space == NULL)
        return HTTP_STATUS_BAD_REQUEST;
    size_t target_len = (size_t)(second_space - target);
    const char *version = second_space + 1;
    HttpStatus status = check_version(version, rest_len - target_len - 1);
    if (status != HTTP_PARSE_OK)
        return status;
    return copy_path(target, target_len, out_request);
}

/* ---- 헤더 ---- */

static char to_lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* 헤더 이름은 대소문자를 구분하지 않는다 (RFC 9110). expected 는 소문자로 준다 */
static bool is_header_name(const char *name, size_t len, const char *expected)
{
    if (len != strlen(expected))
        return false;
    for (size_t i = 0; i < len; i++) {
        if (to_lower_ascii(name[i]) != expected[i])
            return false;
    }
    return true;
}

static bool is_blank(char c)
{
    return c == ' ' || c == '\t';
}

/* 숫자만 받는다 (부호, 지수, 16 진수, 빈 값은 400). 한도를 넘으면 숫자는 끝까지 검사한 뒤 413.
 * 한도를 넘은 뒤에는 더 곱하지 않아서 아주 긴 숫자도 넘치지 않는다 */
static HttpStatus parse_content_length(const char *value, size_t len, size_t *out_length)
{
    if (len == 0)
        return HTTP_STATUS_BAD_REQUEST;
    size_t length = 0;
    bool is_too_large = false;
    for (size_t i = 0; i < len; i++) {
        if (!is_digit(value[i]))
            return HTTP_STATUS_BAD_REQUEST;
        if (!is_too_large)
            length = length * DECIMAL_BASE + (size_t)(value[i] - '0');
        is_too_large = is_too_large || length > HTTP_MAX_BODY;
    }
    if (is_too_large)
        return HTTP_STATUS_PAYLOAD_TOO_LARGE;
    *out_length = length;
    return HTTP_PARSE_OK;
}

static HttpStatus apply_content_length(const char *value, size_t len, HeaderState *state)
{
    size_t length = 0;
    HttpStatus status = parse_content_length(value, len, &length);
    if (status != HTTP_PARSE_OK)
        return status;
    /* 서로 다른 값 두 개는 어느 쪽을 믿을지 모호하다 (요청 밀반입 공격의 통로) */
    if (state->has_content_length && state->content_length != length)
        return HTTP_STATUS_BAD_REQUEST;
    state->has_content_length = true;
    state->content_length = length;
    return HTTP_PARSE_OK;
}

/* "Name: value". 이름 앞뒤 공백 금지 (줄 앞 공백은 폐지된 접힌 헤더라 거절), 값은 앞뒤 공백을 뗀다 */
static HttpStatus parse_header_line(Line line, HeaderState *state)
{
    const char *colon = memchr(line.text, ':', line.len);
    if (colon == NULL || colon == line.text)
        return HTTP_STATUS_BAD_REQUEST;
    size_t name_len = (size_t)(colon - line.text);
    for (size_t i = 0; i < name_len; i++) {
        if (is_blank(line.text[i]))
            return HTTP_STATUS_BAD_REQUEST;
    }
    const char *value = colon + 1;
    size_t value_len = line.len - name_len - 1;
    while (value_len > 0 && is_blank(value[0])) {
        value++;
        value_len--;
    }
    while (value_len > 0 && is_blank(value[value_len - 1]))
        value_len--;

    if (is_header_name(line.text, name_len, HEADER_CONTENT_LENGTH))
        return apply_content_length(value, value_len, state);
    if (is_header_name(line.text, name_len, HEADER_TRANSFER_ENCODING))
        state->has_transfer_encoding = true;
    return HTTP_PARSE_OK;
}

/* ---- 전체 ---- */

static HttpStatus parse_headers(const char *head, size_t head_len, size_t pos,
                                HeaderState *state)
{
    Line line;
    while (next_line(head, head_len, &pos, &line) && line.len > 0) {
        HttpStatus status = parse_header_line(line, state);
        if (status != HTTP_PARSE_OK)
            return status;
    }
    return HTTP_PARSE_OK;
}

HttpStatus http_parse_head(const char *head, size_t head_len, HttpRequest *out_request)
{
    memset(out_request, 0, sizeof(*out_request));
    if (head_len > HTTP_MAX_HEAD)
        return HTTP_STATUS_HEADERS_TOO_LARGE;
    if (memchr(head, '\0', head_len) != NULL) /* C 문자열로 다루는 곳에서 뒤가 잘리지 않게 */
        return HTTP_STATUS_BAD_REQUEST;
    size_t pos = 0;
    Line line;
    if (!next_line(head, head_len, &pos, &line))
        return HTTP_STATUS_BAD_REQUEST;
    HttpStatus status = parse_request_line(line, out_request);
    HeaderState state = {false, 0, false};
    if (status == HTTP_PARSE_OK)
        status = parse_headers(head, head_len, pos, &state);
    if (status != HTTP_PARSE_OK)
        return status;
    if (state.has_transfer_encoding) /* chunked 등은 지원하지 않는다 */
        return HTTP_STATUS_NOT_IMPLEMENTED;
    if (!state.has_content_length && strcmp(out_request->method, METHOD_POST) == 0)
        return HTTP_STATUS_LENGTH_REQUIRED;
    out_request->has_content_length = state.has_content_length;
    out_request->content_length = state.content_length;
    return HTTP_PARSE_OK;
}

HttpStatus http_parse_request(const char *buf, size_t len, HttpRequest *out_request)
{
    size_t head_len = 0;
    if (!http_find_head_end(buf, len, &head_len)) {
        memset(out_request, 0, sizeof(*out_request));
        return len >= HTTP_MAX_HEAD ? HTTP_STATUS_HEADERS_TOO_LARGE : HTTP_STATUS_BAD_REQUEST;
    }
    HttpStatus status = http_parse_head(buf, head_len, out_request);
    if (status != HTTP_PARSE_OK)
        return status;
    if (len - head_len < out_request->content_length)
        return HTTP_STATUS_BAD_REQUEST; /* 본문이 덜 왔다 */
    out_request->body = out_request->content_length > 0 ? buf + head_len : NULL;
    out_request->body_len = out_request->content_length;
    return HTTP_PARSE_OK;
}

/* ---- 소켓 읽기 ---- */

bool http_set_read_timeout(int fd, int timeout_ms)
{
    struct timeval timeout = {
        .tv_sec = timeout_ms / MS_PER_SECOND,
        .tv_usec = (timeout_ms % MS_PER_SECOND) * US_PER_MS,
    };
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0;
}

/* buf[*len..limit) 에 한 번 더 받는다. 받은 만큼 *len 을 늘린다.
 * EAGAIN/EWOULDBLOCK 은 SO_RCVTIMEO 가 지난 것 (408), 0 바이트는 상대가 닫은 것 */
static HttpStatus receive_more(int fd, char *buf, size_t limit, size_t *len)
{
    for (;;) {
        ssize_t n = recv(fd, buf + *len, limit - *len, 0);
        if (n > 0) {
            *len += (size_t)n;
            return HTTP_PARSE_OK;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return HTTP_STATUS_REQUEST_TIMEOUT;
        return HTTP_READ_CLOSED;
    }
}

/* 헤더 끝 빈 줄이 보일 때까지 받는다. 새로 받은 부분과 그 앞 3 바이트만 다시 훑어서
 * 1 바이트씩 오는 요청도 O(n) 으로 끝난다. 빈 줄 없이 HTTP_MAX_HEAD 를 받으면 431 */
static HttpStatus receive_head(int fd, char *buf, size_t capacity, size_t *len,
                               size_t *out_head_len)
{
    size_t scanned = 0;
    for (;;) {
        size_t start = scanned >= HEAD_END_MAX_LEN ? scanned - (HEAD_END_MAX_LEN - 1) : 0;
        size_t found = 0;
        if (http_find_head_end(buf + start, *len - start, &found)) {
            *out_head_len = start + found;
            return HTTP_PARSE_OK;
        }
        if (*len >= HTTP_MAX_HEAD)
            return HTTP_STATUS_HEADERS_TOO_LARGE;
        scanned = *len;
        HttpStatus status = receive_more(fd, buf, capacity, len);
        if (status != HTTP_PARSE_OK)
            return status;
    }
}

HttpStatus http_read_request(int fd, char *buf, size_t capacity, HttpRequest *out_request)
{
    memset(out_request, 0, sizeof(*out_request));
    if (capacity < HTTP_MAX_HEAD)
        return HTTP_STATUS_INTERNAL_ERROR; /* 호출한 쪽 실수: 헤더도 못 담는 버퍼 */
    size_t len = 0;
    size_t head_len = 0;
    HttpStatus status = receive_head(fd, buf, capacity, &len, &head_len);
    if (status == HTTP_PARSE_OK)
        status = http_parse_head(buf, head_len, out_request); /* 413 등은 본문을 받기 전에 */
    if (status != HTTP_PARSE_OK)
        return status;
    size_t needed = head_len + out_request->content_length;
    if (needed > capacity)
        return HTTP_STATUS_PAYLOAD_TOO_LARGE;
    while (len < needed && status == HTTP_PARSE_OK) /* 한도를 needed 로 둬서 더 보낸 것은 읽지 않는다 */
        status = receive_more(fd, buf, needed, &len);
    if (status != HTTP_PARSE_OK)
        return status;
    out_request->body = out_request->content_length > 0 ? buf + head_len : NULL;
    out_request->body_len = out_request->content_length;
    return HTTP_PARSE_OK;
}

/* ---- 소켓 쓰기 ---- */

typedef struct {
    int status;
    const char *reason;
} ReasonPhrase;

/* 상태 코드 → 사유 문구는 이 표 하나에서만 정한다 */
static const ReasonPhrase REASON_PHRASES[] = {
    {HTTP_STATUS_OK, "OK"},
    {HTTP_STATUS_BAD_REQUEST, "Bad Request"},
    {HTTP_STATUS_NOT_FOUND, "Not Found"},
    {HTTP_STATUS_METHOD_NOT_ALLOWED, "Method Not Allowed"},
    {HTTP_STATUS_REQUEST_TIMEOUT, "Request Timeout"},
    {HTTP_STATUS_LENGTH_REQUIRED, "Length Required"},
    {HTTP_STATUS_PAYLOAD_TOO_LARGE, "Payload Too Large"},
    {HTTP_STATUS_URI_TOO_LONG, "URI Too Long"},
    {HTTP_STATUS_HEADERS_TOO_LARGE, "Request Header Fields Too Large"},
    {HTTP_STATUS_INTERNAL_ERROR, "Internal Server Error"},
    {HTTP_STATUS_NOT_IMPLEMENTED, "Not Implemented"},
    {HTTP_STATUS_SERVICE_UNAVAILABLE, "Service Unavailable"},
    {HTTP_STATUS_VERSION_NOT_SUPPORTED, "HTTP Version Not Supported"},
};

const char *http_reason_phrase(int status)
{
    for (size_t i = 0; i < sizeof(REASON_PHRASES) / sizeof(REASON_PHRASES[0]); i++) {
        if (REASON_PHRASES[i].status == status)
            return REASON_PHRASES[i].reason;
    }
    return "Unknown";
}

/* 앞쪽의 빈 조각을 건너뛴다 (본문 없는 응답, 다 보낸 조각) */
static int skip_empty_parts(struct iovec **parts, int count)
{
    while (count > 0 && (*parts)->iov_len == 0) {
        (*parts)++;
        count--;
    }
    return count;
}

/* 조각들을 sendmsg 로 끝까지 보낸다. 일부만 나가면 보낸 만큼 조각을 앞으로 당겨 다시 보낸다.
 * MSG_NOSIGNAL: 상대가 끊은 소켓에 쓰면 SIGPIPE 로 프로세스가 죽는 대신 EPIPE 로 돌아온다 */
static bool send_all_parts(int fd, struct iovec *parts, int count)
{
    count = skip_empty_parts(&parts, count);
    while (count > 0) {
        struct msghdr message = {.msg_iov = parts, .msg_iovlen = (size_t)count};
        ssize_t n = sendmsg(fd, &message, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        size_t sent = (size_t)n;
        while (count > 0 && sent >= parts->iov_len) {
            sent -= parts->iov_len;
            parts++;
            count--;
        }
        if (count > 0) {
            parts->iov_base = (char *)parts->iov_base + sent;
            parts->iov_len -= sent;
        }
        count = skip_empty_parts(&parts, count);
    }
    return true;
}

bool http_write_all(int fd, const void *buf, size_t len)
{
    struct iovec part = {(void *)buf, len};
    return send_all_parts(fd, &part, 1);
}

/* 헤더와 본문을 따로 send 하면 작은 패킷 두 개가 Nagle 과 지연 ACK 에 걸려 수십 ms 늦어질 수 있다.
 * 그래서 두 조각을 한 번의 sendmsg 로 보낸다 */
bool http_write_response(int fd, const HttpResponse *response)
{
    char head[RESPONSE_HEAD_MAX];
    int head_len = snprintf(head, sizeof(head),
                            "HTTP/1.1 %d %s\r\n"
                            "Content-Type: %s\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: close\r\n"
                            "%s\r\n",
                            response->status, http_reason_phrase(response->status),
                            response->content_type, response->body_len,
                            response->extra_headers == NULL ? "" : response->extra_headers);
    if (head_len < 0 || (size_t)head_len >= sizeof(head))
        return false;
    struct iovec parts[RESPONSE_PART_COUNT] = {
        {head, (size_t)head_len},
        {(void *)response->body, response->body == NULL ? 0 : response->body_len},
    };
    return send_all_parts(fd, parts, RESPONSE_PART_COUNT);
}
