#include "executor.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "row.h"

#define EXECUTOR_ROW_MAX (SCHEMA_MAX_COLUMNS * SCHEMA_VARCHAR_MAX)

static int fail(char *err, size_t err_size, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(err, err_size, fmt, ap);
    va_end(ap);
    return -1;
}

int executor_init(struct executor *ex, const char *schema_dir, const char *data_dir)
{
    memset(ex, 0, sizeof(*ex));
    if (strlen(schema_dir) >= EXECUTOR_PATH_MAX || strlen(data_dir) >= EXECUTOR_PATH_MAX) {
        return -1;
    }
    strcpy(ex->schema_dir, schema_dir);
    strcpy(ex->data_dir, data_dir);
    pthread_mutex_init(&ex->tables_lock, NULL);
    return 0;
}

/* 이미 연 테이블을 찾고, 없으면 연다 */
static struct table *get_table(struct executor *ex, const char *name, char *err, size_t err_size)
{
    struct table *found = NULL;

    pthread_mutex_lock(&ex->tables_lock);
    for (int i = 0; i < ex->table_count; i++) {
        if (strcmp(ex->tables[i]->schema.table, name) == 0) {
            found = ex->tables[i];
            break;
        }
    }
    if (!found && ex->table_count == EXECUTOR_MAX_TABLES) {
        fail(err, err_size, "too many open tables (max %d)", EXECUTOR_MAX_TABLES);
    } else if (!found) {
        struct table *t = malloc(sizeof(*t));
        if (!t) {
            fail(err, err_size, "out of memory");
        } else if (table_open(t, ex->schema_dir, ex->data_dir, name, err, err_size) < 0) {
            free(t);
        } else {
            ex->tables[ex->table_count++] = t;
            found = t;
        }
    }
    pthread_mutex_unlock(&ex->tables_lock);
    return found;
}

static int find_column(const struct schema *s, const char *name, char *err, size_t err_size)
{
    int col = schema_find_column(s, name);

    if (col < 0) {
        fail(err, err_size, "unknown column '%s' in table '%s'", name, s->table);
    }
    return col;
}

/* 값의 타입이 컬럼과 맞는지. 맞으면 0 */
static int check_type(const struct schema *s, int col, const struct sql_value *v, char *err,
                      size_t err_size)
{
    const struct column *c = &s->columns[col];

    if (c->type == COLUMN_INT && v->type != SQL_VALUE_INT) {
        return fail(err, err_size, "column '%s' expects INT", c->name);
    }
    if (c->type == COLUMN_VARCHAR && v->type != SQL_VALUE_STRING) {
        return fail(err, err_size, "column '%s' expects a string", c->name);
    }
    return 0;
}

static int set_value(const struct schema *s, int col, void *row, const struct sql_value *v,
                     char *err, size_t err_size)
{
    if (check_type(s, col, v, err, err_size) < 0) {
        return -1;
    }
    if (s->columns[col].type == COLUMN_INT) {
        row_set_int(s, col, row, v->int_value);
        return 0;
    }
    if (row_set_string(s, col, row, v->str_value) < 0) {
        return fail(err, err_size, "value too long for column '%s' (max %u bytes)",
                    s->columns[col].name, s->columns[col].size);
    }
    return 0;
}

/* INSERT 의 값 i 가 들어갈 컬럼 위치들을 targets 에 채운다 */
static int resolve_insert_targets(const struct schema *s, const struct statement *stmt,
                                  int *targets, char *err, size_t err_size)
{
    if (stmt->column_count == 0) {
        int expected = s->column_count - 1; /* id 는 자동 */
        if (stmt->value_count != expected) {
            return fail(err, err_size, "table '%s' expects %d values (id is automatic), got %d",
                        s->table, expected, stmt->value_count);
        }
        for (int i = 0; i < expected; i++) {
            targets[i] = i + 1;
        }
        return 0;
    }

    bool used[SCHEMA_MAX_COLUMNS] = {false};
    for (int i = 0; i < stmt->column_count; i++) {
        int col = find_column(s, stmt->columns[i], err, err_size);
        if (col < 0) {
            return -1;
        }
        if (col == 0) {
            return fail(err, err_size, "id is assigned automatically");
        }
        if (used[col]) {
            return fail(err, err_size, "column '%s' given twice", s->columns[col].name);
        }
        used[col] = true;
        targets[i] = col;
    }
    return 0;
}

static int execute_insert(struct table *t, const struct statement *stmt, struct result *out,
                          char *err, size_t err_size)
{
    const struct schema *s = &t->schema;
    int targets[SQL_MAX_COLUMNS];
    char row[EXECUTOR_ROW_MAX];

    if (resolve_insert_targets(s, stmt, targets, err, err_size) < 0) {
        return -1;
    }
    memset(row, 0, s->row_size); /* 지정하지 않은 컬럼은 0 / 빈 문자열 */
    for (int i = 0; i < stmt->value_count; i++) {
        if (set_value(s, targets[i], row, &stmt->values[i], err, err_size) < 0) {
            return -1;
        }
    }

    long long id = table_insert(t, row);
    if (id < 0) {
        return fail(err, err_size, "write failed");
    }
    out->type = RESULT_INSERT;
    out->inserted_id = id;
    return 0;
}

struct select_ctx {
    const struct schema *schema;
    const struct where_clause *where;
    int where_col;
    struct result *out;
    bool is_oom;
};

static bool row_matches(const struct select_ctx *c, const void *row)
{
    const struct where_clause *w = c->where;

    if (w->type == WHERE_NONE) {
        return true;
    }
    if (c->schema->columns[c->where_col].type == COLUMN_INT) {
        long long v = row_get_int(c->schema, c->where_col, row);
        if (w->type == WHERE_BETWEEN) {
            return w->low <= v && v <= w->high;
        }
        return v == w->value.int_value;
    }
    char buf[SCHEMA_VARCHAR_MAX + 1];
    row_get_string(c->schema, c->where_col, row, buf, sizeof(buf));
    return strcmp(buf, w->value.str_value) == 0;
}

static int collect_row(const void *row, uint64_t offset, void *ctx)
{
    struct select_ctx *c = ctx;

    (void)offset;
    if (!row_matches(c, row)) {
        return 0;
    }
    if (result_add_row(c->out, row) < 0) {
        c->is_oom = true;
        return 1;
    }
    return 0;
}

static int resolve_select_columns(const struct schema *s, const struct statement *stmt,
                                  struct result *out, char *err, size_t err_size)
{
    if (stmt->is_select_all) {
        out->column_count = s->column_count;
        for (int i = 0; i < s->column_count; i++) {
            out->columns[i] = i;
        }
        return 0;
    }
    out->column_count = stmt->column_count;
    for (int i = 0; i < stmt->column_count; i++) {
        out->columns[i] = find_column(s, stmt->columns[i], err, err_size);
        if (out->columns[i] < 0) {
            return -1;
        }
    }
    return 0;
}

static int check_where(const struct schema *s, const struct where_clause *w, int *where_col,
                       char *err, size_t err_size)
{
    *where_col = -1;
    if (w->type == WHERE_NONE) {
        return 0;
    }
    *where_col = find_column(s, w->column, err, err_size);
    if (*where_col < 0) {
        return -1;
    }
    if (w->type == WHERE_BETWEEN) {
        if (s->columns[*where_col].type != COLUMN_INT) {
            return fail(err, err_size, "BETWEEN needs an INT column ('%s' is VARCHAR)",
                        s->columns[*where_col].name);
        }
        return 0;
    }
    return check_type(s, *where_col, &w->value, err, err_size);
}

static int execute_select(struct table *t, const struct statement *stmt, struct result *out,
                          char *err, size_t err_size)
{
    const struct schema *s = &t->schema;
    struct select_ctx ctx = {.schema = s, .where = &stmt->where, .out = out};

    out->type = RESULT_SELECT;
    out->schema = s;
    if (resolve_select_columns(s, stmt, out, err, err_size) < 0 ||
        check_where(s, &stmt->where, &ctx.where_col, err, err_size) < 0) {
        return -1;
    }
    if (storage_scan(&t->storage, collect_row, &ctx) < 0) {
        result_free(out);
        return fail(err, err_size, "read failed");
    }
    if (ctx.is_oom) {
        result_free(out);
        return fail(err, err_size, "out of memory");
    }
    return 0;
}

int executor_execute(struct executor *ex, const struct statement *stmt, struct result *out,
                     char *err, size_t err_size)
{
    result_init(out);
    struct table *t = get_table(ex, stmt->table, err, err_size);
    if (!t) {
        return -1;
    }
    if (stmt->type == STATEMENT_INSERT) {
        return execute_insert(t, stmt, out, err, err_size);
    }
    return execute_select(t, stmt, out, err, err_size);
}

void executor_close(struct executor *ex)
{
    for (int i = 0; i < ex->table_count; i++) {
        table_close(ex->tables[i]);
        free(ex->tables[i]);
    }
    ex->table_count = 0;
    pthread_mutex_destroy(&ex->tables_lock);
}
