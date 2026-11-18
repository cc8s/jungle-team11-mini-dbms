#include "result.h"

#include <stdlib.h>
#include <string.h>

#include "row.h"

#define RESULT_INITIAL_ROWS 16

void result_init(struct result *r)
{
    memset(r, 0, sizeof(*r));
}

int result_add_row(struct result *r, const void *row)
{
    size_t row_size = r->schema->row_size;

    if (r->row_count == r->row_cap) {
        size_t cap = r->row_cap ? r->row_cap * 2 : RESULT_INITIAL_ROWS;
        char *grown = realloc(r->rows, cap * row_size);
        if (!grown) {
            return -1;
        }
        r->rows = grown;
        r->row_cap = cap;
    }
    memcpy(r->rows + r->row_count * row_size, row, row_size);
    r->row_count++;
    return 0;
}

const void *result_row(const struct result *r, size_t index)
{
    return r->rows + index * r->schema->row_size;
}

static void print_cell(const struct result *r, int col, const void *row, FILE *out)
{
    const struct column *c = &r->schema->columns[col];

    if (c->type == COLUMN_INT) {
        fprintf(out, "%lld", row_get_int(r->schema, col, row));
        return;
    }
    char buf[SCHEMA_VARCHAR_MAX + 1];
    row_get_string(r->schema, col, row, buf, sizeof(buf));
    fputs(buf, out);
}

void result_print(const struct result *r, FILE *out)
{
    if (r->type == RESULT_INSERT) {
        fprintf(out, "INSERT 1 (id = %lld)\n", r->inserted_id);
        return;
    }

    for (int i = 0; i < r->column_count; i++) {
        fprintf(out, "%s%s", i ? " | " : "", r->schema->columns[r->columns[i]].name);
    }
    fputc('\n', out);
    for (size_t k = 0; k < r->row_count; k++) {
        const void *row = result_row(r, k);
        for (int i = 0; i < r->column_count; i++) {
            if (i) {
                fputs(" | ", out);
            }
            print_cell(r, r->columns[i], row, out);
        }
        fputc('\n', out);
    }
    fprintf(out, "(%zu %s)\n", r->row_count, r->row_count == 1 ? "row" : "rows");
}

const char *result_plan_name(enum result_plan plan)
{
    switch (plan) {
    case RESULT_PLAN_INDEX_LOOKUP:
        return "index lookup";
    case RESULT_PLAN_INDEX_RANGE:
        return "index range";
    case RESULT_PLAN_SCAN:
    default:
        return "full scan";
    }
}

void result_free(struct result *r)
{
    free(r->rows);
    r->rows = NULL;
    r->row_count = 0;
    r->row_cap = 0;
}
