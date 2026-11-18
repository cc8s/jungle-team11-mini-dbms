#include "storage.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STORAGE_MAGIC     "MDB1"
#define STORAGE_SCAN_ROWS 1024 /* scan 이 한 번에 읽는 행 수 */

struct storage_header {
    char magic[4];
    uint32_t row_size;
    int64_t next_id;
    char reserved[STORAGE_HEADER_SIZE - 16];
};

_Static_assert(sizeof(struct storage_header) == STORAGE_HEADER_SIZE, "header must be 32 bytes");

static int fail(char *err, size_t err_size, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(err, err_size, fmt, ap);
    va_end(ap);
    return -1;
}

/* pwrite 는 요청보다 적게 쓸 수 있으므로 다 쓸 때까지 반복한다 */
static int write_all(int fd, const char *buf, size_t len, off_t offset)
{
    while (len > 0) {
        ssize_t n = pwrite(fd, buf, len, offset);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        buf += n;
        len -= (size_t)n;
        offset += n;
    }
    return 0;
}

static int read_all(int fd, char *buf, size_t len, off_t offset)
{
    while (len > 0) {
        ssize_t n = pread(fd, buf, len, offset);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        buf += n;
        len -= (size_t)n;
        offset += n;
    }
    return 0;
}

static uint64_t row_offset(const struct storage *st, uint64_t index)
{
    return STORAGE_HEADER_SIZE + index * st->row_size;
}

static int write_header(struct storage *st)
{
    struct storage_header h;

    memset(&h, 0, sizeof(h));
    memcpy(h.magic, STORAGE_MAGIC, 4);
    h.row_size = st->row_size;
    h.next_id = st->next_id;
    return write_all(st->fd, (const char *)&h, sizeof(h), 0);
}

/* flush_lock 을 잡은 상태에서 부른다 */
static int flush_locked(struct storage *st)
{
    if (st->wbuf_len > 0) {
        if (write_all(st->fd, st->wbuf, st->wbuf_len, (off_t)row_offset(st, st->flushed_count)) <
            0) {
            return -1;
        }
        st->flushed_count += st->wbuf_len / st->row_size;
        st->wbuf_len = 0;
    }
    return write_header(st);
}

static int open_existing(struct storage *st, off_t size, char *err, size_t err_size)
{
    struct storage_header h;

    if (size < STORAGE_HEADER_SIZE ||
        read_all(st->fd, (char *)&h, sizeof(h), 0) < 0 ||
        memcmp(h.magic, STORAGE_MAGIC, 4) != 0) {
        return fail(err, err_size, "not a table file (bad header)");
    }
    if (h.row_size != st->row_size) {
        return fail(err, err_size, "table file row size %u does not match schema (%u)",
                    h.row_size, st->row_size);
    }
    st->next_id = h.next_id;
    st->flushed_count = (uint64_t)(size - STORAGE_HEADER_SIZE) / st->row_size;
    return 0;
}

int storage_open(struct storage *st, const char *path, uint32_t row_size, char *err,
                 size_t err_size)
{
    struct stat sb;

    memset(st, 0, sizeof(*st));
    st->row_size = row_size;
    st->next_id = 1;
    st->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (st->fd < 0) {
        return fail(err, err_size, "cannot open %s: %s", path, strerror(errno));
    }

    int rc;
    if (fstat(st->fd, &sb) < 0) {
        rc = fail(err, err_size, "cannot stat %s: %s", path, strerror(errno));
    } else if (sb.st_size == 0) {
        rc = write_header(st) < 0 ? fail(err, err_size, "cannot write header") : 0;
    } else {
        rc = open_existing(st, sb.st_size, err, err_size);
    }

    /* 버퍼는 행 크기의 배수로 잡아야 flush 단위가 행 경계와 맞는다 */
    st->wbuf_cap = row_size >= STORAGE_WRITE_BUFFER ? row_size
                                                   : STORAGE_WRITE_BUFFER / row_size * row_size;
    st->wbuf = rc == 0 ? malloc(st->wbuf_cap) : NULL;
    if (rc == 0 && !st->wbuf) {
        rc = fail(err, err_size, "out of memory");
    }
    if (rc < 0) {
        close(st->fd);
        free(st->wbuf);
        st->fd = -1;
        st->wbuf = NULL;
        return -1;
    }
    st->row_count = st->flushed_count;
    pthread_mutex_init(&st->flush_lock, NULL);
    return 0;
}

int storage_append(struct storage *st, const void *row, uint64_t *offset_out)
{
    int rc = 0;

    pthread_mutex_lock(&st->flush_lock);
    if (st->wbuf_len + st->row_size > st->wbuf_cap && flush_locked(st) < 0) {
        rc = -1;
    } else {
        memcpy(st->wbuf + st->wbuf_len, row, st->row_size);
        st->wbuf_len += st->row_size;
        *offset_out = row_offset(st, st->row_count);
        st->row_count++;
    }
    pthread_mutex_unlock(&st->flush_lock);
    return rc;
}

/* upto_offset 까지의 행이 파일에 있도록 필요하면 flush 한다 */
static int ensure_flushed(struct storage *st, uint64_t end_offset)
{
    int rc = 0;

    pthread_mutex_lock(&st->flush_lock);
    if (end_offset > row_offset(st, st->flushed_count)) {
        rc = flush_locked(st);
    }
    pthread_mutex_unlock(&st->flush_lock);
    return rc;
}

int storage_read(struct storage *st, uint64_t offset, void *row)
{
    uint64_t end = offset + st->row_size;

    if (offset < STORAGE_HEADER_SIZE || (offset - STORAGE_HEADER_SIZE) % st->row_size != 0 ||
        end > row_offset(st, st->row_count)) {
        return -1;
    }
    if (ensure_flushed(st, end) < 0) {
        return -1;
    }
    return read_all(st->fd, row, st->row_size, (off_t)offset);
}

int storage_scan(struct storage *st, storage_scan_fn fn, void *ctx)
{
    uint64_t total = st->row_count; /* 쓰기 락이 막고 있으므로 scan 도중에 늘지 않는다 */

    if (ensure_flushed(st, row_offset(st, total)) < 0) {
        return -1;
    }
    char *chunk = malloc((size_t)STORAGE_SCAN_ROWS * st->row_size);
    if (!chunk) {
        return -1;
    }

    int rc = 0;
    for (uint64_t i = 0; i < total && rc == 0;) {
        uint64_t n = total - i < STORAGE_SCAN_ROWS ? total - i : STORAGE_SCAN_ROWS;
        if (read_all(st->fd, chunk, n * st->row_size, (off_t)row_offset(st, i)) < 0) {
            rc = -1;
            break;
        }
        for (uint64_t k = 0; k < n; k++) {
            if (fn(chunk + k * st->row_size, row_offset(st, i + k), ctx) != 0) {
                free(chunk);
                return 0;
            }
        }
        i += n;
    }
    free(chunk);
    return rc;
}

int storage_flush(struct storage *st)
{
    pthread_mutex_lock(&st->flush_lock);
    int rc = flush_locked(st);
    pthread_mutex_unlock(&st->flush_lock);
    return rc;
}

uint64_t storage_row_count(const struct storage *st)
{
    return st->row_count;
}

int storage_close(struct storage *st)
{
    if (st->fd < 0) {
        return 0;
    }
    int rc = storage_flush(st);
    close(st->fd);
    free(st->wbuf);
    pthread_mutex_destroy(&st->flush_lock);
    st->fd = -1;
    st->wbuf = NULL;
    return rc;
}
