#include "table.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "row.h"

struct max_id_ctx {
    const struct schema *schema;
    long long max_id;
};

static int max_id_fn(const void *row, uint64_t offset, void *ctx)
{
    struct max_id_ctx *c = ctx;
    long long id = row_get_int(c->schema, 0, row);

    (void)offset;
    if (id > c->max_id) {
        c->max_id = id;
    }
    return 0;
}

int table_open(struct table *t, const char *schema_dir, const char *data_dir, const char *name,
               char *err, size_t err_size)
{
    char path[512];

    memset(t, 0, sizeof(*t));
    if (schema_load(schema_dir, name, &t->schema, err, err_size) < 0) {
        return -1;
    }
    if (mkdir(data_dir, 0755) < 0 && errno != EEXIST) {
        snprintf(err, err_size, "cannot create %s: %s", data_dir, strerror(errno));
        return -1;
    }
    snprintf(path, sizeof(path), "%s/%s.tbl", data_dir, name);
    if (storage_open(&t->storage, path, t->schema.row_size, err, err_size) < 0) {
        return -1;
    }

    /* 헤더의 next_id 는 마지막 flush 이후 늦을 수 있다. 실제 행에서 다시 계산한다 */
    struct max_id_ctx ctx = {&t->schema, 0};
    if (storage_scan(&t->storage, max_id_fn, &ctx) < 0) {
        snprintf(err, err_size, "cannot read %s", path);
        storage_close(&t->storage);
        return -1;
    }
    if (ctx.max_id + 1 > t->storage.next_id) {
        t->storage.next_id = ctx.max_id + 1;
    }
    return 0;
}

long long table_insert(struct table *t, void *row)
{
    long long id = t->storage.next_id;
    uint64_t offset;

    row_set_int(&t->schema, 0, row, id);
    if (storage_append(&t->storage, row, &offset) < 0) {
        return -1;
    }
    t->storage.next_id = id + 1;
    return id;
}

void table_close(struct table *t)
{
    storage_close(&t->storage);
}
