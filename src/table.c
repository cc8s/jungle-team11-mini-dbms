#include "table.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "row.h"

struct rebuild_ctx {
    struct table *t;
    long long duplicate_id;
    bool has_duplicate;
    bool is_oom;
};

/* 시작할 때 파일을 한 번 훑으며 id → 오프셋을 트리에 넣는다 (D003) */
static int rebuild_fn(const void *row, uint64_t offset, void *ctx)
{
    struct rebuild_ctx *c = ctx;
    long long id = row_get_int(&c->t->schema, 0, row);
    enum bptree_result r = bptree_insert(&c->t->index, id, offset);

    if (r == BPTREE_DUPLICATE) {
        c->duplicate_id = id;
        c->has_duplicate = true;
        return 1;
    }
    if (r == BPTREE_NO_MEMORY) {
        c->is_oom = true;
        return 1;
    }
    return 0;
}

static int rebuild_index(struct table *t, const char *path, char *err, size_t err_size)
{
    struct rebuild_ctx ctx = {.t = t};

    if (bptree_init(&t->index, BPTREE_DEFAULT_ORDER) < 0) {
        snprintf(err, err_size, "out of memory");
        return -1;
    }
    if (storage_scan(&t->storage, rebuild_fn, &ctx) < 0) {
        snprintf(err, err_size, "cannot read %s", path);
        return -1;
    }
    if (ctx.has_duplicate) {
        snprintf(err, err_size, "%s is corrupted: id %lld appears twice", path, ctx.duplicate_id);
        return -1;
    }
    if (ctx.is_oom) {
        snprintf(err, err_size, "out of memory while building index");
        return -1;
    }

    /* 헤더의 next_id 는 마지막 flush 이후 늦을 수 있다. 실제 가장 큰 id 로 맞춘다 (D008) */
    long long max_id;
    if (bptree_max_key(&t->index, &max_id) && max_id + 1 > t->storage.next_id) {
        t->storage.next_id = max_id + 1;
    }
    t->is_index_valid = true;
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
    if (rebuild_index(t, path, err, err_size) < 0) {
        bptree_free(&t->index);
        storage_close(&t->storage);
        return -1;
    }

    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    pthread_rwlock_init(&t->lock, &attr);
    pthread_rwlockattr_destroy(&attr);
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

    /* 파일이 원본이다. 트리에 못 넣으면(메모리 부족) 행은 그대로 두고 인덱스를 끈다.
     * 이후 조회는 선형 탐색으로 맞는 답을 내고, 다음 시작 때 트리를 다시 만든다 */
    if (t->is_index_valid && bptree_insert(&t->index, id, offset) != BPTREE_OK) {
        t->is_index_valid = false;
    }
    return id;
}

void table_close(struct table *t)
{
    storage_close(&t->storage);
    bptree_free(&t->index);
    pthread_rwlock_destroy(&t->lock);
}
