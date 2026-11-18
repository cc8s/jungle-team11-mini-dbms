#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "storage.h"
#include "test.h"

#define ROW_SIZE 24

static char dir[64];
static char path[128];
static char err[256];

/* 테스트마다 새 임시 파일 경로를 만든다 */
static void fresh_path(const char *name)
{
    if (dir[0] == '\0') {
        strcpy(dir, "/tmp/minidb-test-XXXXXX");
        CHECK(mkdtemp(dir) != NULL);
    }
    snprintf(path, sizeof(path), "%s/%s.tbl", dir, name);
    unlink(path);
}

static void make_row(char *row, int n)
{
    memset(row, 0, ROW_SIZE);
    snprintf(row, ROW_SIZE, "row-%d", n);
}

static off_t file_size(void)
{
    struct stat sb;

    CHECK(stat(path, &sb) == 0);
    return sb.st_size;
}

/* TC-STORE-001 EP: 새 파일은 헤더만, next_id 는 1 */
static void test_create(void)
{
    struct storage st;

    fresh_path("create");
    CHECK(storage_open(&st, path, ROW_SIZE, err, sizeof(err)) == 0);
    CHECK(storage_row_count(&st) == 0);
    CHECK(st.next_id == 1);
    CHECK(storage_close(&st) == 0);
    CHECK(file_size() == STORAGE_HEADER_SIZE);
}

/* TC-STORE-002 EP: 붙이고 오프셋으로 읽기. 오프셋 = 헤더 + i * 행 크기 */
static void test_append_and_read(void)
{
    struct storage st;
    char row[ROW_SIZE];
    char got[ROW_SIZE];
    uint64_t off[3];

    fresh_path("append");
    CHECK(storage_open(&st, path, ROW_SIZE, err, sizeof(err)) == 0);
    for (int i = 0; i < 3; i++) {
        make_row(row, i);
        CHECK(storage_append(&st, row, &off[i]) == 0);
        CHECK(off[i] == STORAGE_HEADER_SIZE + (uint64_t)i * ROW_SIZE);
    }
    CHECK(storage_row_count(&st) == 3);
    CHECK(storage_read(&st, off[1], got) == 0);
    CHECK_STR_EQ(got, "row-1");
    CHECK(storage_close(&st) == 0);
}

/* TC-STORE-003 EP: 쓰기 버퍼 — 붙인 직후엔 파일에 없고, 읽기가 flush 를 일으킨다 (D002) */
static void test_buffered_until_read(void)
{
    struct storage st;
    char row[ROW_SIZE];
    uint64_t off;

    fresh_path("buffer");
    CHECK(storage_open(&st, path, ROW_SIZE, err, sizeof(err)) == 0);
    make_row(row, 7);
    CHECK(storage_append(&st, row, &off) == 0);
    CHECK(file_size() == STORAGE_HEADER_SIZE); /* 아직 버퍼에만 있다 */
    CHECK(storage_read(&st, off, row) == 0);
    CHECK(file_size() == STORAGE_HEADER_SIZE + ROW_SIZE);
    CHECK(storage_close(&st) == 0);
}

/* TC-STORE-004 EP: 닫고 다시 열면 행 수와 next_id 가 남아 있다 */
static void test_reopen(void)
{
    struct storage st;
    char row[ROW_SIZE];
    uint64_t off;

    fresh_path("reopen");
    CHECK(storage_open(&st, path, ROW_SIZE, err, sizeof(err)) == 0);
    for (int i = 0; i < 5; i++) {
        make_row(row, i);
        CHECK(storage_append(&st, row, &off) == 0);
    }
    st.next_id = 6;
    CHECK(storage_close(&st) == 0);

    CHECK(storage_open(&st, path, ROW_SIZE, err, sizeof(err)) == 0);
    CHECK(storage_row_count(&st) == 5);
    CHECK(st.next_id == 6);
    CHECK(storage_read(&st, off, row) == 0);
    CHECK_STR_EQ(row, "row-4");
    CHECK(storage_close(&st) == 0);
}

struct scan_ctx {
    int seen;
    int stop_at;
    int order_ok;
};

static int count_rows(const void *row, uint64_t offset, void *ctx)
{
    struct scan_ctx *c = ctx;
    char want[ROW_SIZE];

    make_row(want, c->seen);
    if (strcmp(row, want) != 0 || offset != STORAGE_HEADER_SIZE + (uint64_t)c->seen * ROW_SIZE) {
        c->order_ok = 0;
    }
    c->seen++;
    return c->seen == c->stop_at;
}

/* TC-STORE-005 BVA: 쓰기 버퍼(64KB)와 scan 묶음(1024행)을 여러 번 넘는 행 수 */
static void test_scan_many(void)
{
    struct storage st;
    char row[ROW_SIZE];
    uint64_t off;
    const int n = 10000; /* 24B * 10000 = 240KB */

    fresh_path("scan");
    CHECK(storage_open(&st, path, ROW_SIZE, err, sizeof(err)) == 0);
    for (int i = 0; i < n; i++) {
        make_row(row, i);
        CHECK(storage_append(&st, row, &off) == 0);
    }
    struct scan_ctx c = {.order_ok = 1, .stop_at = -1};
    CHECK(storage_scan(&st, count_rows, &c) == 0);
    CHECK(c.seen == n && c.order_ok);
    CHECK(storage_close(&st) == 0);
    CHECK(file_size() == STORAGE_HEADER_SIZE + (off_t)n * ROW_SIZE);
}

/* TC-STORE-006 EP: 콜백이 0 이 아닌 값을 돌려주면 멈춘다 */
static void test_scan_stop(void)
{
    struct storage st;
    char row[ROW_SIZE];
    uint64_t off;

    fresh_path("stop");
    CHECK(storage_open(&st, path, ROW_SIZE, err, sizeof(err)) == 0);
    for (int i = 0; i < 10; i++) {
        make_row(row, i);
        CHECK(storage_append(&st, row, &off) == 0);
    }
    struct scan_ctx c = {.order_ok = 1, .stop_at = 3};
    CHECK(storage_scan(&st, count_rows, &c) == 0);
    CHECK(c.seen == 3);
    CHECK(storage_close(&st) == 0);
}

/* TC-STORE-007 EG: 범위 밖, 행 경계가 아닌 오프셋, 헤더 오프셋 */
static void test_read_bad_offset(void)
{
    struct storage st;
    char row[ROW_SIZE];
    uint64_t off;

    fresh_path("bad");
    CHECK(storage_open(&st, path, ROW_SIZE, err, sizeof(err)) == 0);
    make_row(row, 0);
    CHECK(storage_append(&st, row, &off) == 0);
    CHECK(storage_read(&st, off + ROW_SIZE, row) < 0);
    CHECK(storage_read(&st, off + 1, row) < 0);
    CHECK(storage_read(&st, 0, row) < 0);
    CHECK(storage_close(&st) == 0);
}

/* TC-STORE-008 EG: 스키마가 바뀌어 행 크기가 다르면 열지 않는다 */
static void test_row_size_mismatch(void)
{
    struct storage st;

    fresh_path("mismatch");
    CHECK(storage_open(&st, path, ROW_SIZE, err, sizeof(err)) == 0);
    CHECK(storage_close(&st) == 0);
    CHECK(storage_open(&st, path, ROW_SIZE + 8, err, sizeof(err)) < 0);
    CHECK(strstr(err, "does not match schema") != NULL);
}

/* TC-STORE-009 EG: 테이블 파일이 아님 (헤더 깨짐) */
static void test_bad_header(void)
{
    struct storage st;

    fresh_path("garbage");
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, "hello world, not a table file!!!!", 33) == 33);
    close(fd);
    CHECK(storage_open(&st, path, ROW_SIZE, err, sizeof(err)) < 0);
    CHECK(strstr(err, "bad header") != NULL);
}

/* TC-STORE-010 EG: 쓰다가 죽어 끝에 반쪽 행이 남으면 그 행은 없는 것으로 보고, 다음 행이 그 자리에 쓰인다 */
static void test_partial_trailing_row(void)
{
    struct storage st;
    char row[ROW_SIZE];
    uint64_t off;

    fresh_path("partial");
    CHECK(storage_open(&st, path, ROW_SIZE, err, sizeof(err)) == 0);
    make_row(row, 0);
    CHECK(storage_append(&st, row, &off) == 0);
    CHECK(storage_close(&st) == 0);

    int fd = open(path, O_WRONLY | O_APPEND);
    CHECK(fd >= 0);
    CHECK(write(fd, "half", 4) == 4);
    close(fd);

    CHECK(storage_open(&st, path, ROW_SIZE, err, sizeof(err)) == 0);
    CHECK(storage_row_count(&st) == 1);
    make_row(row, 1);
    CHECK(storage_append(&st, row, &off) == 0);
    CHECK(off == STORAGE_HEADER_SIZE + ROW_SIZE);
    CHECK(storage_read(&st, off, row) == 0);
    CHECK_STR_EQ(row, "row-1");
    CHECK(storage_close(&st) == 0);
}

int main(void)
{
    RUN(test_create);
    RUN(test_append_and_read);
    RUN(test_buffered_until_read);
    RUN(test_reopen);
    RUN(test_scan_many);
    RUN(test_scan_stop);
    RUN(test_read_bad_offset);
    RUN(test_row_size_mismatch);
    RUN(test_bad_header);
    RUN(test_partial_trailing_row);
    TEST_DONE();
    return 0;
}
