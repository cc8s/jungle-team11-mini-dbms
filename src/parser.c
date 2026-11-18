#include "parser.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* 재귀 하강 파서. 토큰 하나를 미리 읽어 둔 상태(cur)에서 문법 규칙대로 소비한다. */
struct parser {
    struct tokenizer tz;
    struct token cur;
    char *err;
    size_t err_size;
    bool has_error;
};

static void advance(struct parser *p)
{
    tokenizer_next(&p->tz, &p->cur);
}

/* 오류는 처음 것 하나만 남긴다. 토큰 오류(닫히지 않은 문자열 등)는 그 설명을 그대로 쓴다. */
static void fail(struct parser *p, const char *fmt, ...)
{
    if (p->has_error) {
        return;
    }
    p->has_error = true;
    if (p->cur.type == TOKEN_ERROR) {
        snprintf(p->err, p->err_size, "%s at position %zu", p->cur.text, p->cur.pos);
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err, p->err_size, fmt, ap);
    va_end(ap);
}

static void fail_near(struct parser *p, const char *expected)
{
    if (p->cur.type == TOKEN_EOF) {
        fail(p, "syntax error: expected %s but reached end of input", expected);
        return;
    }
    fail(p, "syntax error near '%s': expected %s", p->cur.text, expected);
}

static bool accept_keyword(struct parser *p, const char *kw)
{
    if (token_is_keyword(&p->cur, kw)) {
        advance(p);
        return true;
    }
    return false;
}

static bool expect_keyword(struct parser *p, const char *kw)
{
    if (accept_keyword(p, kw)) {
        return true;
    }
    fail_near(p, kw);
    return false;
}

static bool accept(struct parser *p, enum token_type type)
{
    if (p->cur.type == type) {
        advance(p);
        return true;
    }
    return false;
}

static bool expect(struct parser *p, enum token_type type, const char *what)
{
    if (accept(p, type)) {
        return true;
    }
    fail_near(p, what);
    return false;
}

static bool parse_name(struct parser *p, char *dst, const char *what)
{
    if (p->cur.type != TOKEN_IDENT) {
        fail_near(p, what);
        return false;
    }
    if (strlen(p->cur.text) >= SQL_NAME_MAX) {
        fail(p, "name too long: '%s'", p->cur.text);
        return false;
    }
    strcpy(dst, p->cur.text);
    advance(p);
    return true;
}

static bool parse_value(struct parser *p, struct sql_value *v)
{
    if (p->cur.type == TOKEN_INT) {
        v->type = SQL_VALUE_INT;
        v->int_value = p->cur.int_value;
        v->str_value[0] = '\0';
    } else if (p->cur.type == TOKEN_STRING) {
        v->type = SQL_VALUE_STRING;
        v->int_value = 0;
        strcpy(v->str_value, p->cur.text);
    } else {
        fail_near(p, "a value");
        return false;
    }
    advance(p);
    return true;
}

static bool parse_int(struct parser *p, long long *dst)
{
    if (p->cur.type != TOKEN_INT) {
        fail_near(p, "an integer");
        return false;
    }
    *dst = p->cur.int_value;
    advance(p);
    return true;
}

/* c1, c2, ... 를 columns 에 채운다. */
static bool parse_column_list(struct parser *p, struct statement *stmt)
{
    do {
        if (stmt->column_count == SQL_MAX_COLUMNS) {
            fail(p, "too many columns (max %d)", SQL_MAX_COLUMNS);
            return false;
        }
        if (!parse_name(p, stmt->columns[stmt->column_count], "a column name")) {
            return false;
        }
        stmt->column_count++;
    } while (accept(p, TOKEN_COMMA));
    return true;
}

static bool parse_value_list(struct parser *p, struct statement *stmt)
{
    do {
        if (stmt->value_count == SQL_MAX_COLUMNS) {
            fail(p, "too many values (max %d)", SQL_MAX_COLUMNS);
            return false;
        }
        if (!parse_value(p, &stmt->values[stmt->value_count])) {
            return false;
        }
        stmt->value_count++;
    } while (accept(p, TOKEN_COMMA));
    return true;
}

/* INSERT 는 이미 소비된 상태 */
static bool parse_insert(struct parser *p, struct statement *stmt)
{
    stmt->type = STATEMENT_INSERT;
    if (!expect_keyword(p, "INTO") || !parse_name(p, stmt->table, "a table name")) {
        return false;
    }
    if (accept(p, TOKEN_LPAREN)) {
        if (!parse_column_list(p, stmt) || !expect(p, TOKEN_RPAREN, "')'")) {
            return false;
        }
    }
    if (!expect_keyword(p, "VALUES") || !expect(p, TOKEN_LPAREN, "'('") ||
        !parse_value_list(p, stmt) || !expect(p, TOKEN_RPAREN, "')'")) {
        return false;
    }
    if (stmt->column_count > 0 && stmt->column_count != stmt->value_count) {
        fail(p, "column count (%d) does not match value count (%d)", stmt->column_count,
             stmt->value_count);
        return false;
    }
    return true;
}

static bool parse_where(struct parser *p, struct where_clause *where)
{
    if (!parse_name(p, where->column, "a column name")) {
        return false;
    }
    if (accept(p, TOKEN_EQUAL)) {
        where->type = WHERE_EQUAL;
        return parse_value(p, &where->value);
    }
    if (accept_keyword(p, "BETWEEN")) {
        where->type = WHERE_BETWEEN;
        return parse_int(p, &where->low) && expect_keyword(p, "AND") &&
               parse_int(p, &where->high);
    }
    fail_near(p, "'=' or BETWEEN");
    return false;
}

/* SELECT 는 이미 소비된 상태 */
static bool parse_select(struct parser *p, struct statement *stmt)
{
    stmt->type = STATEMENT_SELECT;
    if (accept(p, TOKEN_STAR)) {
        stmt->is_select_all = true;
    } else if (!parse_column_list(p, stmt)) {
        return false;
    }
    if (!expect_keyword(p, "FROM") || !parse_name(p, stmt->table, "a table name")) {
        return false;
    }
    if (accept_keyword(p, "WHERE")) {
        return parse_where(p, &stmt->where);
    }
    return true;
}

enum parse_result parser_parse(const char *sql, size_t *consumed, struct statement *stmt, char *err,
                               size_t err_size)
{
    struct parser p = {.err = err, .err_size = err_size};

    memset(stmt, 0, sizeof(*stmt));
    err[0] = '\0';
    tokenizer_init(&p.tz, sql);
    advance(&p);

    while (p.cur.type == TOKEN_SEMICOLON) { /* 빈 문장 ;; 은 건너뛴다 */
        advance(&p);
    }
    if (p.cur.type == TOKEN_EOF) {
        *consumed = p.tz.pos;
        return PARSE_EMPTY;
    }

    if (accept_keyword(&p, "INSERT")) {
        parse_insert(&p, stmt);
    } else if (accept_keyword(&p, "SELECT")) {
        parse_select(&p, stmt);
    } else {
        fail_near(&p, "INSERT or SELECT");
    }

    if (!p.has_error && p.cur.type != TOKEN_SEMICOLON && p.cur.type != TOKEN_EOF) {
        fail_near(&p, "';' or end of input");
    }
    if (p.has_error) {
        /* 다음 문장은 실패한 문장의 ';' 뒤부터. CLI 가 한 문장 실패 뒤에도 계속할 수 있게 한다. */
        const char *semi = strchr(sql + p.cur.pos, ';');
        *consumed = semi ? (size_t)(semi - sql) + 1 : strlen(sql);
        return PARSE_ERROR;
    }
    *consumed = p.tz.pos; /* ';' 까지 소비했거나 입력 끝 */
    return PARSE_OK;
}
