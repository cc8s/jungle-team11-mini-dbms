#include "json.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEY_SQL "sql"
#define KEY_PARAMS "params"
#define HEX_DIGITS 4             /* \uXXXX */
#define UNICODE_ESCAPE_LEN 6     /* \uXXXX 전체 */
#define HIGH_SURROGATE_MIN 0xD800
#define HIGH_SURROGATE_MAX 0xDBFF
#define LOW_SURROGATE_MIN 0xDC00
#define LOW_SURROGATE_MAX 0xDFFF
#define SURROGATE_BASE 0x10000
#define SURROGATE_SHIFT 10
#define INT32_MAGNITUDE_MAX 2147483648ULL /* |INT32_MIN| */
#define DECIMAL_BASE 10
#define INITIAL_KEY_CAPACITY 4
#define END_OF_TEXT (-1)
#define INITIAL_BUF_CAPACITY 256
#define NUMBER_TEXT_SIZE 32      /* int64 또는 %.3f 숫자 하나 */
#define CONTROL_ESCAPE_SIZE 7    /* "\u001f" + \0 */
#define FIRST_PRINTABLE 0x20

/* 파서 상태. 문법 오류는 처음 것 하나만 기록하고 바로 멈춘다.
 * params 오류는 기록만 하고 계속 읽는다: 뒤에 문법 오류가 있으면 그쪽을 알려야 하기 때문 */
typedef struct {
    const char *text;
    size_t len;
    size_t pos;
    int depth;
    bool is_broken;
    bool is_out_of_memory;
    bool has_bad_param;
    char *message; /* 호출한 쪽의 out_message (MAX_MESSAGE) */
    char param_message[MAX_MESSAGE];
} JsonParser;

/* 이스케이프를 푼 문자열 (malloc, 끝에 \0) */
typedef struct {
    char *ptr;
    size_t len;
} JsonString;

/* 최상위 객체에서 본 키 (같은 키 두 번 검사용) */
typedef struct {
    JsonString *keys;
    int count;
    int capacity;
} SeenKeys;

typedef struct {
    bool is_int32; /* 소수점·지수가 없고 int32 범위 안 */
    int32_t value;
} JsonNumber;

static bool skip_value(JsonParser *parser);

/* ---- 공통 ---- */

static bool fail(JsonParser *parser, const char *reason)
{
    if (!parser->is_broken)
        snprintf(parser->message, MAX_MESSAGE, "invalid JSON at byte %zu: %s", parser->pos,
                 reason);
    parser->is_broken = true;
    return false;
}

static bool fail_no_memory(JsonParser *parser)
{
    parser->is_out_of_memory = true;
    return fail(parser, "out of memory");
}

static void reject_param(JsonParser *parser, const char *reason)
{
    if (!parser->has_bad_param)
        snprintf(parser->param_message, MAX_MESSAGE, "%s", reason);
    parser->has_bad_param = true;
}

static int peek(const JsonParser *parser)
{
    return parser->pos < parser->len ? (unsigned char)parser->text[parser->pos] : END_OF_TEXT;
}

static void skip_whitespace(JsonParser *parser)
{
    int c = peek(parser);
    while (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        parser->pos++;
        c = peek(parser);
    }
}

static bool consume(JsonParser *parser, char expected)
{
    skip_whitespace(parser);
    if (peek(parser) != expected)
        return false;
    parser->pos++;
    return true;
}

static bool is_digit(int c)
{
    return c >= '0' && c <= '9';
}

/* 중첩 한도: 재귀로 들어가기 전에 검사해서 [[[[...]]]] 1,000 단계도 스택을 넘치지 않는다 */
static bool enter_container(JsonParser *parser)
{
    if (++parser->depth > JSON_MAX_DEPTH)
        return fail(parser, "nesting too deep");
    parser->pos++; /* '{' 또는 '[' */
    return true;
}

/* ---- 문자열 ---- */

/* 닫는 따옴표 위치. 디코딩한 결과는 원문보다 길어지지 않으므로 (\uXXXX 6 바이트 → UTF-8 최대 3,
 * 서로게이트 쌍 12 바이트 → 4) 이 길이만큼만 할당하면 된다 */
static bool find_string_end(JsonParser *parser, size_t *out_end)
{
    for (size_t i = parser->pos; i < parser->len; i++) {
        if (parser->text[i] == '\\')
            i++;
        else if (parser->text[i] == '"') {
            *out_end = i;
            return true;
        }
    }
    return fail(parser, "unterminated string");
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + DECIMAL_BASE;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + DECIMAL_BASE;
    return -1;
}

/* pos 가 'u' 다음을 가리킬 때 16 진수 4 자리를 읽는다 */
static bool read_hex4(JsonParser *parser, size_t end, uint32_t *out_code)
{
    if (end - parser->pos < HEX_DIGITS)
        return fail(parser, "incomplete \\u escape");
    uint32_t code = 0;
    for (int i = 0; i < HEX_DIGITS; i++) {
        int digit = hex_value(parser->text[parser->pos + (size_t)i]);
        if (digit < 0)
            return fail(parser, "invalid \\u escape");
        code = code * 16 + (uint32_t)digit;
    }
    parser->pos += HEX_DIGITS;
    *out_code = code;
    return true;
}

static void append_utf8(char *buf, size_t *len, uint32_t code)
{
    unsigned char *out = (unsigned char *)buf + *len;
    if (code < 0x80) {
        out[0] = (unsigned char)code;
        *len += 1;
    } else if (code < 0x800) {
        out[0] = (unsigned char)(0xC0 | (code >> 6));
        out[1] = (unsigned char)(0x80 | (code & 0x3F));
        *len += 2;
    } else if (code < SURROGATE_BASE) {
        out[0] = (unsigned char)(0xE0 | (code >> 12));
        out[1] = (unsigned char)(0x80 | ((code >> 6) & 0x3F));
        out[2] = (unsigned char)(0x80 | (code & 0x3F));
        *len += 3;
    } else {
        out[0] = (unsigned char)(0xF0 | (code >> 18));
        out[1] = (unsigned char)(0x80 | ((code >> 12) & 0x3F));
        out[2] = (unsigned char)(0x80 | ((code >> 6) & 0x3F));
        out[3] = (unsigned char)(0x80 | (code & 0x3F));
        *len += 4;
    }
}

/* \uXXXX. U+FFFF 를 넘는 문자는 서로게이트 쌍 (😀) 으로 오므로 둘을 합쳐 한 글자로 만든다.
 * 짝 없는 서로게이트는 UTF-8 로 나타낼 수 없어 거절한다 */
static bool decode_unicode(JsonParser *parser, size_t end, char *buf, size_t *len)
{
    uint32_t code = 0;
    if (!read_hex4(parser, end, &code))
        return false;
    if (code >= LOW_SURROGATE_MIN && code <= LOW_SURROGATE_MAX)
        return fail(parser, "unpaired low surrogate");
    if (code >= HIGH_SURROGATE_MIN && code <= HIGH_SURROGATE_MAX) {
        if (end - parser->pos < UNICODE_ESCAPE_LEN || parser->text[parser->pos] != '\\' ||
            parser->text[parser->pos + 1] != 'u')
            return fail(parser, "unpaired high surrogate");
        parser->pos += 2;
        uint32_t low = 0;
        if (!read_hex4(parser, end, &low))
            return false;
        if (low < LOW_SURROGATE_MIN || low > LOW_SURROGATE_MAX)
            return fail(parser, "unpaired high surrogate");
        code = SURROGATE_BASE + ((code - HIGH_SURROGATE_MIN) << SURROGATE_SHIFT) +
               (low - LOW_SURROGATE_MIN);
    }
    append_utf8(buf, len, code);
    return true;
}

static char simple_escape(char c)
{
    switch (c) {
    case '"': return '"';
    case '\\': return '\\';
    case '/': return '/';
    case 'b': return '\b';
    case 'f': return '\f';
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    default: return '\0';
    }
}

/* pos 가 '\\' 를 가리킬 때 이스케이프 하나를 풀어 buf 뒤에 붙인다 */
static bool decode_escape(JsonParser *parser, size_t end, char *buf, size_t *len)
{
    char kind = parser->text[parser->pos + 1];
    parser->pos += 2;
    if (kind == 'u')
        return decode_unicode(parser, end, buf, len);
    char decoded = simple_escape(kind);
    if (decoded == '\0')
        return fail(parser, "invalid escape");
    buf[(*len)++] = decoded;
    return true;
}

/* pos 가 여는 따옴표를 가리킬 때 문자열을 읽어 이스케이프를 푼다.
 * 날것의 제어 문자 (0x00~0x1F, 줄바꿈 포함) 는 JSON 규칙상 이스케이프해야 해서 거절한다 */
static bool parse_string(JsonParser *parser, JsonString *out_string)
{
    parser->pos++; /* '"' */
    size_t end = 0;
    if (!find_string_end(parser, &end))
        return false;
    char *buf = malloc(end - parser->pos + 1);
    if (buf == NULL)
        return fail_no_memory(parser);
    size_t len = 0;
    bool is_ok = true;
    while (is_ok && parser->pos < end) {
        unsigned char c = (unsigned char)parser->text[parser->pos];
        if (c < 0x20)
            is_ok = fail(parser, "control character in string");
        else if (c == '\\')
            is_ok = decode_escape(parser, end, buf, &len);
        else
            buf[len++] = parser->text[parser->pos++];
    }
    if (!is_ok) {
        free(buf);
        return false;
    }
    buf[len] = '\0';
    parser->pos = end + 1;
    *out_string = (JsonString){buf, len};
    return true;
}

static bool skip_string(JsonParser *parser)
{
    JsonString ignored;
    if (!parse_string(parser, &ignored))
        return false;
    free(ignored.ptr);
    return true;
}

/* ---- 숫자, 리터럴 ---- */

/* 숫자 하나 이상. 크기는 int32 범위를 넘으면 더 곱하지 않아서 아주 긴 수도 넘치지 않는다 */
static bool read_digits(JsonParser *parser, uint64_t *magnitude)
{
    if (!is_digit(peek(parser)))
        return fail(parser, "invalid number");
    while (is_digit(peek(parser))) {
        if (*magnitude <= INT32_MAGNITUDE_MAX)
            *magnitude = *magnitude * DECIMAL_BASE + (uint64_t)(peek(parser) - '0');
        parser->pos++;
    }
    return true;
}

/* 소수부 (.[0-9]+) 와 지수부 ([eE][+-]?[0-9]+). 하나라도 있으면 정수가 아니다 */
static bool read_fraction_and_exponent(JsonParser *parser, bool *out_is_integer)
{
    uint64_t ignored = 0;
    *out_is_integer = true;
    if (peek(parser) == '.') {
        parser->pos++;
        *out_is_integer = false;
        if (!read_digits(parser, &ignored))
            return false;
    }
    if (peek(parser) == 'e' || peek(parser) == 'E') {
        parser->pos++;
        *out_is_integer = false;
        if (peek(parser) == '+' || peek(parser) == '-')
            parser->pos++;
        if (!read_digits(parser, &ignored))
            return false;
    }
    return true;
}

/* -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)? 를 검사하고, int32 정수면 값을 낸다 */
static bool parse_number(JsonParser *parser, JsonNumber *out_number)
{
    bool is_negative = peek(parser) == '-';
    if (is_negative)
        parser->pos++;
    bool starts_with_zero = peek(parser) == '0';
    uint64_t magnitude = 0;
    size_t digits_start = parser->pos;
    if (!read_digits(parser, &magnitude))
        return false;
    if (starts_with_zero && parser->pos - digits_start > 1)
        return fail(parser, "leading zero in number");
    bool is_integer = true;
    if (!read_fraction_and_exponent(parser, &is_integer))
        return false;
    uint64_t limit = is_negative ? INT32_MAGNITUDE_MAX : INT32_MAGNITUDE_MAX - 1;
    out_number->is_int32 = is_integer && magnitude <= limit;
    out_number->value = (int32_t)(is_negative ? -(int64_t)magnitude : (int64_t)magnitude);
    return true;
}

static bool skip_literal(JsonParser *parser)
{
    const char *literals[] = {"true", "false", "null"};
    for (size_t i = 0; i < sizeof(literals) / sizeof(literals[0]); i++) {
        size_t literal_len = strlen(literals[i]);
        if (parser->len - parser->pos >= literal_len &&
            memcmp(parser->text + parser->pos, literals[i], literal_len) == 0) {
            parser->pos += literal_len;
            return true;
        }
    }
    return fail(parser, "unexpected character");
}

/* ---- 건너뛰기 (모르는 키의 값, 잘못된 params 원소) ---- */

/* 쉼표로 구분된 목록을 닫는 문자까지 읽는다. parse_item 이 원소 하나를 읽는다 */
static bool parse_list(JsonParser *parser, char closing, bool (*parse_item)(JsonParser *, void *),
                       void *ctx)
{
    if (consume(parser, closing))
        return true;
    for (;;) {
        if (!parse_item(parser, ctx))
            return false;
        if (consume(parser, closing))
            return true;
        if (!consume(parser, ','))
            return fail(parser, "expected ',' or closing bracket");
    }
}

static bool skip_item(JsonParser *parser, void *ctx)
{
    (void)ctx;
    return skip_value(parser);
}

static bool skip_member(JsonParser *parser, void *ctx)
{
    (void)ctx;
    skip_whitespace(parser);
    if (peek(parser) != '"')
        return fail(parser, "expected a string key");
    if (!skip_string(parser) || !consume(parser, ':'))
        return parser->is_broken ? false : fail(parser, "expected ':'");
    return skip_value(parser);
}

static bool skip_container(JsonParser *parser, char closing,
                           bool (*parse_item)(JsonParser *, void *))
{
    if (!enter_container(parser))
        return false;
    bool is_ok = parse_list(parser, closing, parse_item, NULL);
    parser->depth--;
    return is_ok;
}

static bool skip_value(JsonParser *parser)
{
    skip_whitespace(parser);
    int c = peek(parser);
    if (c == '{')
        return skip_container(parser, '}', skip_member);
    if (c == '[')
        return skip_container(parser, ']', skip_item);
    if (c == '"')
        return skip_string(parser);
    if (c == '-' || is_digit(c)) {
        JsonNumber ignored;
        return parse_number(parser, &ignored);
    }
    if (c == END_OF_TEXT)
        return fail(parser, "unexpected end of input");
    return skip_literal(parser);
}

/* ---- params ---- */

/* 한도를 넘은 값은 저장하지 않고 해제한다 (계속 읽어서 문법 오류는 그대로 잡는다) */
static void store_param(JsonParser *parser, QueryRequest *request, Value value)
{
    if (request->param_count >= JSON_MAX_PARAMS) {
        reject_param(parser, "\"params\" has more than 64 elements");
        value_free(&value);
        return;
    }
    request->params[request->param_count++] = value;
}

/* 원소 하나: 문자열 → VAL_STRING, int32 정수 → VAL_INT, 그 밖 (실수, bool, null, 객체, 배열) 은
 * 문법만 검사하고 ERR_PARAM 으로 기록한다 */
static bool parse_param(JsonParser *parser, void *ctx)
{
    QueryRequest *request = ctx;
    skip_whitespace(parser);
    int c = peek(parser);
    if (c == '"') {
        JsonString text;
        if (!parse_string(parser, &text))
            return false;
        Value value = {.type = VAL_STRING, .s = {text.ptr, text.len}};
        if (memchr(text.ptr, '\0', text.len) != NULL) { /* VARCHAR 에 \0 은 넣지 않는다 */
            reject_param(parser, "string parameters must not contain \\u0000");
            value_free(&value);
            return true;
        }
        store_param(parser, request, value);
        return true;
    }
    if (c == '-' || is_digit(c)) {
        JsonNumber number;
        if (!parse_number(parser, &number))
            return false;
        if (!number.is_int32)
            reject_param(parser, "number parameters must be int32 integers");
        else
            store_param(parser, request, value_from_int(number.value));
        return true;
    }
    if (!skip_value(parser))
        return false;
    reject_param(parser, "parameters must be integers or strings");
    return true;
}

static bool parse_params(JsonParser *parser, QueryRequest *request)
{
    skip_whitespace(parser);
    if (peek(parser) != '[')
        return fail(parser, "\"params\" must be an array");
    if (!enter_container(parser))
        return false;
    bool is_ok = parse_list(parser, ']', parse_param, request);
    parser->depth--;
    return is_ok;
}

/* ---- 최상위 객체 ---- */

static bool parse_sql(JsonParser *parser, QueryRequest *request)
{
    skip_whitespace(parser);
    if (peek(parser) != '"')
        return fail(parser, "\"sql\" must be a string");
    JsonString text;
    if (!parse_string(parser, &text))
        return false;
    if (memchr(text.ptr, '\0', text.len) != NULL) { /* 엔진은 C 문자열을 받으므로 뒤가 잘린다 */
        free(text.ptr);
        return fail(parser, "\"sql\" must not contain \\u0000");
    }
    request->sql = text.ptr;
    request->sql_len = text.len;
    return true;
}

static bool is_key(const JsonString *key, const char *name)
{
    return key->len == strlen(name) && memcmp(key->ptr, name, key->len) == 0;
}

/* 키를 기록하고, 이미 본 키면 false. 같은 키가 두 번이면 어느 값을 쓸지 모호하다 */
static bool remember_key(JsonParser *parser, SeenKeys *seen, JsonString key)
{
    for (int i = 0; i < seen->count; i++) {
        if (seen->keys[i].len == key.len && memcmp(seen->keys[i].ptr, key.ptr, key.len) == 0) {
            free(key.ptr);
            return fail(parser, "duplicate key");
        }
    }
    if (seen->count == seen->capacity) {
        int capacity = seen->capacity == 0 ? INITIAL_KEY_CAPACITY : seen->capacity * 2;
        JsonString *grown = realloc(seen->keys, (size_t)capacity * sizeof(*grown));
        if (grown == NULL) {
            free(key.ptr);
            return fail_no_memory(parser);
        }
        seen->keys = grown;
        seen->capacity = capacity;
    }
    seen->keys[seen->count++] = key;
    return true;
}

typedef struct {
    QueryRequest *request;
    SeenKeys seen;
} TopLevel;

static bool parse_member(JsonParser *parser, void *ctx)
{
    TopLevel *top = ctx;
    skip_whitespace(parser);
    if (peek(parser) != '"')
        return fail(parser, "expected a string key");
    JsonString key;
    if (!parse_string(parser, &key) || !remember_key(parser, &top->seen, key))
        return false;
    if (!consume(parser, ':'))
        return fail(parser, "expected ':'");
    const JsonString *stored = &top->seen.keys[top->seen.count - 1];
    if (is_key(stored, KEY_SQL))
        return parse_sql(parser, top->request);
    if (is_key(stored, KEY_PARAMS))
        return parse_params(parser, top->request);
    return skip_value(parser); /* 모르는 키는 값까지 건너뛴다 */
}

static bool parse_document(JsonParser *parser, QueryRequest *request)
{
    skip_whitespace(parser);
    if (peek(parser) != '{')
        return fail(parser, "the request body must be a JSON object");
    TopLevel top = {request, {NULL, 0, 0}};
    bool is_ok = enter_container(parser) && parse_list(parser, '}', parse_member, &top);
    parser->depth--;
    for (int i = 0; i < top.seen.count; i++)
        free(top.seen.keys[i].ptr);
    free(top.seen.keys);
    if (!is_ok)
        return false;
    skip_whitespace(parser);
    return parser->pos == parser->len || fail(parser, "unexpected data after the object");
}

/* 문법 오류 > sql 없음 > params 오류 순서로 하나만 알린다. 실패면 요청을 비운다 */
static JsonStatus finish(JsonParser *parser, QueryRequest *request)
{
    JsonStatus status = JSON_OK;
    if (parser->is_out_of_memory)
        status = JSON_NO_MEMORY;
    else if (parser->is_broken)
        status = JSON_BAD_REQUEST;
    else if (request->sql == NULL) {
        snprintf(parser->message, MAX_MESSAGE, "missing \"sql\"");
        status = JSON_BAD_REQUEST;
    } else if (parser->has_bad_param) {
        snprintf(parser->message, MAX_MESSAGE, "%s", parser->param_message);
        status = JSON_BAD_PARAM;
    }
    if (status != JSON_OK)
        query_request_free(request);
    return status;
}

JsonStatus json_parse_query(const char *text, size_t len, QueryRequest *out_request,
                            char out_message[MAX_MESSAGE])
{
    memset(out_request, 0, sizeof(*out_request));
    out_message[0] = '\0';
    JsonParser parser = {.text = text, .len = len, .message = out_message};
    parse_document(&parser, out_request);
    return finish(&parser, out_request);
}

void query_request_free(QueryRequest *request)
{
    free(request->sql);
    request->sql = NULL;
    request->sql_len = 0;
    for (int i = 0; i < request->param_count; i++)
        value_free(&request->params[i]);
    request->param_count = 0;
}

/* ---- 응답 버퍼 ---- */

void json_buf_init(JsonBuf *buf)
{
    *buf = (JsonBuf){NULL, 0, 0, false};
}

void json_buf_free(JsonBuf *buf)
{
    free(buf->data);
    json_buf_init(buf);
}

/* 모자라면 2 배씩 늘린다: n 바이트를 붙이는 데 복사가 O(n) 으로 끝난다. +1 은 끝의 \0 자리 */
static bool reserve(JsonBuf *buf, size_t extra)
{
    if (buf->is_out_of_memory)
        return false;
    size_t needed = buf->len + extra + 1;
    if (needed <= buf->capacity)
        return true;
    size_t capacity = buf->capacity == 0 ? INITIAL_BUF_CAPACITY : buf->capacity;
    while (capacity < needed)
        capacity *= 2;
    char *grown = realloc(buf->data, capacity);
    if (grown == NULL) {
        buf->is_out_of_memory = true;
        return false;
    }
    buf->data = grown;
    buf->capacity = capacity;
    return true;
}

void json_buf_append(JsonBuf *buf, const char *text, size_t len)
{
    if (!reserve(buf, len))
        return;
    memcpy(buf->data + buf->len, text, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
}

void json_buf_append_text(JsonBuf *buf, const char *text)
{
    json_buf_append(buf, text, strlen(text));
}

void json_buf_append_int(JsonBuf *buf, int64_t number)
{
    char text[NUMBER_TEXT_SIZE];
    int len = snprintf(text, sizeof(text), "%" PRId64, number);
    json_buf_append(buf, text, (size_t)len);
}

/* 이스케이프가 필요한 바이트의 표기. 필요 없으면 NULL. 짧은 표기가 있는 것은 그것을, 나머지 제어 문자는
 * \u00XX 를 쓴다 (control 버퍼에 만든다) */
static const char *escape_of(unsigned char c, char control[CONTROL_ESCAPE_SIZE])
{
    switch (c) {
    case '"': return "\\\"";
    case '\\': return "\\\\";
    case '\n': return "\\n";
    case '\r': return "\\r";
    case '\t': return "\\t";
    case '\b': return "\\b";
    case '\f': return "\\f";
    default: break;
    }
    if (c >= FIRST_PRINTABLE)
        return NULL;
    snprintf(control, CONTROL_ESCAPE_SIZE, "\\u%04x", c);
    return control;
}

/* 이스케이프할 필요 없는 바이트는 한 덩어리로 모아서 붙인다 (바이트마다 append 하지 않음) */
void json_buf_append_escaped(JsonBuf *buf, const char *text, size_t len)
{
    json_buf_append(buf, "\"", 1);
    size_t run_start = 0;
    for (size_t i = 0; i < len; i++) {
        char control[CONTROL_ESCAPE_SIZE];
        const char *escaped = escape_of((unsigned char)text[i], control);
        if (escaped == NULL)
            continue;
        json_buf_append(buf, text + run_start, i - run_start);
        json_buf_append_text(buf, escaped);
        run_start = i + 1;
    }
    json_buf_append(buf, text + run_start, len - run_start);
    json_buf_append(buf, "\"", 1);
}

/* ---- Result → JSON ---- */

static void append_bool(JsonBuf *buf, bool value)
{
    json_buf_append_text(buf, value ? "true" : "false");
}

static void append_elapsed(JsonBuf *buf, double elapsed_ms)
{
    char text[NUMBER_TEXT_SIZE];
    int len = snprintf(text, sizeof(text), "%.3f", elapsed_ms);
    json_buf_append_text(buf, ",\"elapsed_ms\":");
    json_buf_append(buf, text, (size_t)len);
}

static void append_columns(JsonBuf *buf, const Result *result)
{
    json_buf_append_text(buf, "\"columns\":[");
    for (int col = 0; col < result->column_count; col++) {
        if (col > 0)
            json_buf_append(buf, ",", 1);
        const char *name = result->columns[col].name;
        json_buf_append_escaped(buf, name, strlen(name));
    }
    json_buf_append(buf, "]", 1);
}

/* 행 하나: 정수 칸은 숫자, VARCHAR 칸은 문자열 ([3,"park",22]) */
static void append_row(JsonBuf *buf, const Result *result, int64_t row)
{
    json_buf_append(buf, "[", 1);
    for (int col = 0; col < result->column_count; col++) {
        if (col > 0)
            json_buf_append(buf, ",", 1);
        if (result->columns[col].type == COL_INT) {
            json_buf_append_int(buf, result_get_int(result, row, col));
            continue;
        }
        size_t len = 0;
        const char *text = result_get_str(result, row, col, &len);
        json_buf_append_escaped(buf, text, len);
    }
    json_buf_append(buf, "]", 1);
}

static void append_select(JsonBuf *buf, const Result *result, int64_t max_rows)
{
    int64_t shown = result->row_count < max_rows ? result->row_count : max_rows;
    json_buf_append_text(buf, "{\"ok\":true,");
    append_columns(buf, result);
    json_buf_append_text(buf, ",\"rows\":[");
    for (int64_t row = 0; row < shown; row++) {
        if (row > 0)
            json_buf_append(buf, ",", 1);
        append_row(buf, result, row);
    }
    json_buf_append_text(buf, "],\"row_count\":");
    json_buf_append_int(buf, result->row_count);
    json_buf_append_text(buf, ",\"truncated\":");
    append_bool(buf, result->row_count > shown);
    json_buf_append_text(buf, ",\"used_index\":");
    append_bool(buf, result->used_index);
    append_elapsed(buf, result->elapsed_ms);
    json_buf_append(buf, "}", 1);
}

static void append_insert(JsonBuf *buf, const Result *result)
{
    json_buf_append_text(buf, "{\"ok\":true,\"affected_rows\":");
    json_buf_append_int(buf, result->affected_rows);
    json_buf_append_text(buf, ",\"last_insert_id\":");
    json_buf_append_int(buf, result->last_insert_id);
    append_elapsed(buf, result->elapsed_ms);
    json_buf_append(buf, "}", 1);
}

static void append_error(JsonBuf *buf, const Result *result)
{
    json_buf_append_text(buf, "{\"ok\":false,\"error\":{\"code\":");
    const char *code = error_name(result->error);
    json_buf_append_escaped(buf, code, strlen(code));
    json_buf_append_text(buf, ",\"message\":");
    json_buf_append_escaped(buf, result->message, strnlen(result->message, MAX_MESSAGE));
    json_buf_append_text(buf, "}}");
}

ErrorCode json_write_result(const Result *result, int64_t max_rows, JsonBuf *out_buf)
{
    if (result->error != ERR_NONE)
        append_error(out_buf, result);
    else if (result->column_count > 0)
        append_select(out_buf, result, max_rows < 0 ? 0 : max_rows);
    else
        append_insert(out_buf, result);
    return out_buf->is_out_of_memory ? ERR_INTERNAL : ERR_NONE;
}
