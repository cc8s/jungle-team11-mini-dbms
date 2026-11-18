#include "schema.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SCHEMA_FILE_MAX (64 * 1024)

static int fail(char *err, size_t err_size, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(err, err_size, fmt, ap);
    va_end(ap);
    return -1;
}

static bool is_valid_name(const char *s)
{
    if (!isalpha((unsigned char)s[0]) && s[0] != '_') {
        return false;
    }
    for (const char *p = s; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_') {
            return false;
        }
    }
    return true;
}

/* "INT" 또는 "VARCHAR(n)" 을 해석해 col 의 type, size 를 채운다. */
static int parse_type(const char *type, struct column *col, int line, char *err, size_t err_size)
{
    if (strcasecmp(type, "INT") == 0) {
        col->type = COLUMN_INT;
        col->size = SCHEMA_INT_SIZE;
        return 0;
    }
    if (strncasecmp(type, "VARCHAR(", 8) != 0) {
        return fail(err, err_size, "line %d: unknown type '%s' (INT or VARCHAR(n))", line, type);
    }

    char *end;
    errno = 0;
    long n = strtol(type + 8, &end, 10);
    if (end == type + 8 || strcmp(end, ")") != 0) {
        return fail(err, err_size, "line %d: VARCHAR needs a length, e.g. VARCHAR(32)", line);
    }
    if (errno == ERANGE || n < 1 || n > SCHEMA_VARCHAR_MAX) {
        return fail(err, err_size, "line %d: VARCHAR length must be 1..%d", line,
                    SCHEMA_VARCHAR_MAX);
    }
    col->type = COLUMN_VARCHAR;
    col->size = (uint32_t)n;
    return 0;
}

/* "name TYPE" 한 줄. 이름과 타입 사이 공백 외에 다른 토큰이 있으면 오류. */
static int parse_line(const char *line_text, struct schema *s, int line, char *err,
                      size_t err_size)
{
    char name[SCHEMA_NAME_MAX + 1];
    char type[32];
    char extra[2];
    int n = sscanf(line_text, "%64s %31s %1s", name, type, extra);

    if (n != 2) {
        return fail(err, err_size, "line %d: expected '<name> <type>'", line);
    }
    if (strlen(name) >= SCHEMA_NAME_MAX || !is_valid_name(name)) {
        return fail(err, err_size, "line %d: invalid column name '%s'", line, name);
    }
    if (schema_find_column(s, name) >= 0) {
        return fail(err, err_size, "line %d: duplicate column '%s'", line, name);
    }
    if (s->column_count == SCHEMA_MAX_COLUMNS) {
        return fail(err, err_size, "line %d: too many columns (max %d)", line,
                    SCHEMA_MAX_COLUMNS);
    }

    struct column *col = &s->columns[s->column_count];
    if (parse_type(type, col, line, err, err_size) < 0) {
        return -1;
    }
    strcpy(col->name, name);
    col->offset = s->row_size;
    s->row_size += col->size;
    s->column_count++;
    return 0;
}

int schema_parse(const char *text, const char *table, struct schema *out, char *err,
                 size_t err_size)
{
    memset(out, 0, sizeof(*out));
    if (strlen(table) >= SCHEMA_NAME_MAX || !is_valid_name(table)) {
        return fail(err, err_size, "invalid table name '%s'", table);
    }
    strcpy(out->table, table);

    int line = 0;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char buf[256];

        line++;
        if (len >= sizeof(buf)) {
            return fail(err, err_size, "line %d: too long", line);
        }
        memcpy(buf, p, len);
        buf[len] = '\0';
        char *hash = strchr(buf, '#');
        if (hash) {
            *hash = '\0';
        }

        char probe[2];
        if (sscanf(buf, "%1s", probe) == 1 && parse_line(buf, out, line, err, err_size) < 0) {
            return -1;
        }
        p += len + (eol ? 1 : 0);
    }

    if (out->column_count == 0) {
        return fail(err, err_size, "schema has no columns");
    }
    if (strcasecmp(out->columns[0].name, "id") != 0 || out->columns[0].type != COLUMN_INT) {
        return fail(err, err_size, "first column must be 'id INT'");
    }
    return 0;
}

int schema_load(const char *dir, const char *table, struct schema *out, char *err, size_t err_size)
{
    char path[512];

    /* 테이블 이름이 경로에 들어가므로 ../ 같은 이름을 먼저 막는다 */
    if (strlen(table) >= SCHEMA_NAME_MAX || !is_valid_name(table)) {
        return fail(err, err_size, "invalid table name '%s'", table);
    }
    snprintf(path, sizeof(path), "%s/%s.schema", dir, table);

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return fail(err, err_size, "table '%s' does not exist", table);
    }
    char *text = malloc(SCHEMA_FILE_MAX + 1);
    if (!text) {
        fclose(fp);
        return fail(err, err_size, "out of memory");
    }
    size_t n = fread(text, 1, SCHEMA_FILE_MAX + 1, fp);
    fclose(fp);
    if (n > SCHEMA_FILE_MAX) {
        free(text);
        return fail(err, err_size, "%s: file too large", path);
    }
    text[n] = '\0';

    int rc = schema_parse(text, table, out, err, err_size);
    free(text);
    return rc;
}

int schema_find_column(const struct schema *s, const char *name)
{
    for (int i = 0; i < s->column_count; i++) {
        if (strcasecmp(s->columns[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}
