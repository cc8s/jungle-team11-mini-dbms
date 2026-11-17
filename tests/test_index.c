#include "test.h"
#include "test_util.h"

#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#include "btree.h"
#include "cli.h"

/* users 레코드: id 4 + name 32 + age 4 */
#define USERS_SQL "CREATE TABLE users (id INT, name VARCHAR(32), age INT);"
#define USERS_RECORD_SIZE 40
#define USERS_FILE "users.tbl"
#define INSERT_USER "INSERT INTO users VALUES ('kim', 25)"
#define SMALL_ROW_COUNT 10
#define MEDIUM_ROW_COUNT 100
#define BULK_ROW_COUNT 100000 /* 4MB: 재구축이 여러 묶음으로 나뉘어 읽힌다 */
#define CORRUPT_POSITION 5
#define CORRUPT_ID 7

/* ---- 준비 ---- */

static TestDb open_db(void)
{
    TestDb db = test_db_create(USERS_SQL);
    CHECK(db.dir != NULL);
    CHECK(db.engine != NULL);
    return db;
}

static int32_t insert_user(Engine *engine)
{
    Result result = engine_execute(engine, INSERT_USER, NULL, 0);
    CHECK(result.error == ERR_NONE);
    int32_t id = result.last_insert_id;
    result_free(&result);
    return id;
}

/* 엔진으로 count 건을 넣고 엔진을 닫는다. 이후 파일을 직접 조작하거나 table_open 으로 연다 */
static TestDb create_users_file(int count)
{
    TestDb db = open_db();
    for (int i = 0; i < count; i++)
        insert_user(db.engine);
    engine_close(db.engine);
    db.engine = NULL;
    return db;
}

static void users_path(const TestDb *db, char *out_path)
{
    snprintf(out_path, TEST_PATH_MAX, "%s/%s", db->dir, USERS_FILE);
}

/* id 번 레코드의 파일 위치 (엔진 공식을 테스트 안에서 따로 계산해 맞는지 본다) */
static uint64_t expected_offset(int32_t id)
{
    return HEADER_SIZE + (uint64_t)(id - 1) * USERS_RECORD_SIZE;
}

static Table *open_users(const TestDb *db)
{
    char path[TEST_PATH_MAX];
    users_path(db, path);
    ErrorCode error = ERR_INTERNAL;
    Table *table = table_open(path, &error);
    CHECK(table != NULL);
    CHECK(error == ERR_NONE);
    return table;
}

/* ---- 파일 조작 (비정상 종료 흉내) ---- */

static void read_header(const char *path, TableHeader *out_header)
{
    uint8_t bytes[HEADER_SIZE];
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    CHECK(pread(fd, bytes, HEADER_SIZE, 0) == HEADER_SIZE);
    close(fd);
    table_header_decode(bytes, out_header);
}

static void write_bytes_at(const char *path, const void *bytes, size_t len, off_t offset)
{
    int fd = open(path, O_WRONLY);
    CHECK(fd >= 0);
    CHECK(pwrite(fd, bytes, len, offset) == (ssize_t)len);
    close(fd);
}

static void write_counters(const TestDb *db, int32_t row_count, int32_t next_id)
{
    char path[TEST_PATH_MAX];
    users_path(db, path);
    TableHeader header;
    read_header(path, &header);
    header.row_count = row_count;
    header.next_id = next_id;
    uint8_t bytes[HEADER_SIZE];
    table_header_encode(&header, bytes);
    write_bytes_at(path, bytes, HEADER_SIZE, 0);
}

static void encode_id(int32_t id, uint8_t *record)
{
    for (int i = 0; i < (int)sizeof(int32_t); i++)
        record[i] = (uint8_t)(((uint32_t)id >> (8 * i)) & 0xFF); /* 리틀 엔디언 */
}

/* position 자리에 id 칸만 채운 레코드 len 바이트를 쓴다 (len < 레코드 크기면 반쪽 레코드) */
static void write_record_at(const TestDb *db, int32_t position, int32_t id, size_t len)
{
    char path[TEST_PATH_MAX];
    users_path(db, path);
    uint8_t record[USERS_RECORD_SIZE] = {0};
    encode_id(id, record);
    write_bytes_at(path, record, len, (off_t)expected_offset(position));
}

/* 1..count 번 레코드 (id 칸만 채움) 를 한 번에 쓴다 */
static void write_bulk_records(const TestDb *db, int32_t count)
{
    char path[TEST_PATH_MAX];
    users_path(db, path);
    size_t len = (size_t)count * USERS_RECORD_SIZE;
    uint8_t *records = calloc(1, len);
    CHECK(records != NULL);
    for (int32_t id = 1; id <= count; id++)
        encode_id(id, records + (size_t)(id - 1) * USERS_RECORD_SIZE);
    write_bytes_at(path, records, len, HEADER_SIZE);
    free(records);
}

static void check_index(const Table *table, int32_t count)
{
    const BTree *index = table_index(table);
    CHECK(index != NULL);
    CHECK(btree_size(index) == count);
    CHECK(btree_check(index));
    for (int32_t id = 1; id <= count; id++) {
        uint64_t offset = 0;
        CHECK(btree_search(index, id, &offset));
        CHECK(offset == expected_offset(id));
    }
}

static void check_counters(const Table *table, int32_t row_count, int32_t next_id)
{
    CHECK(table_record_count(table) == row_count);
    CHECK(table_next_id(table) == next_id);
}

static void check_no_recovery(const Table *table)
{
    TableRecovery recovery;
    CHECK(!table_find_recovery(table, &recovery));
}

/* ---- 정상 ---- */

/* T25-01 */
static void test_rebuild_index_on_open(void)
{
    TestDb db = create_users_file(MEDIUM_ROW_COUNT);
    Table *table = open_users(&db);
    check_index(table, MEDIUM_ROW_COUNT);
    check_no_recovery(table);
    CHECK(table_index_build_ms(table) >= 0);
    table_close(table);
    test_db_destroy(&db);
}

/* T25-01 (묶음 경계): 레코드를 파일에 직접 써서 여러 번에 나눠 읽는 경로를 밟는다 */
static void test_rebuild_bulk_in_batches(void)
{
    TestDb db = create_users_file(0);
    write_bulk_records(&db, BULK_ROW_COUNT);
    write_counters(&db, BULK_ROW_COUNT, BULK_ROW_COUNT + 1);
    Table *table = open_users(&db);
    check_index(table, BULK_ROW_COUNT);
    check_no_recovery(table);
    table_close(table);
    test_db_destroy(&db);
}

/* T25-02 */
static void test_reopen_gives_same_index(void)
{
    TestDb db = create_users_file(MEDIUM_ROW_COUNT);
    Table *table = open_users(&db);
    table_close(table);
    table = open_users(&db);
    check_index(table, MEDIUM_ROW_COUNT);
    table_close(table);
    test_db_destroy(&db);
}

/* T25-03 */
static void test_empty_table_index(void)
{
    TestDb db = create_users_file(0);
    Table *table = open_users(&db);
    check_index(table, 0);
    check_counters(table, 0, 1);
    check_no_recovery(table);
    table_close(table);
    test_db_destroy(&db);
}

/* ---- 헤더 복구 ---- */

/* E25-01 */
static void test_recover_records_after_stale_header(void)
{
    TestDb db = create_users_file(SMALL_ROW_COUNT);
    write_counters(&db, SMALL_ROW_COUNT - 1, SMALL_ROW_COUNT); /* 마지막 헤더 갱신 전에 죽음 */
    Table *table = open_users(&db);
    check_counters(table, SMALL_ROW_COUNT, SMALL_ROW_COUNT + 1);
    check_index(table, SMALL_ROW_COUNT);

    TableRecovery recovery;
    CHECK(table_find_recovery(table, &recovery));
    CHECK(recovery.old_row_count == SMALL_ROW_COUNT - 1);
    CHECK(recovery.old_next_id == SMALL_ROW_COUNT);
    CHECK(recovery.row_count == SMALL_ROW_COUNT);
    CHECK(recovery.next_id == SMALL_ROW_COUNT + 1);
    table_close(table);

    char path[TEST_PATH_MAX];
    users_path(&db, path);
    TableHeader header;
    read_header(path, &header); /* 파일의 헤더도 바로잡혔다 */
    CHECK(header.row_count == SMALL_ROW_COUNT);
    CHECK(header.next_id == SMALL_ROW_COUNT + 1);
    test_db_destroy(&db);
}

/* E25-02 */
static void test_ignore_partial_tail_record(void)
{
    TestDb db = create_users_file(SMALL_ROW_COUNT);
    write_record_at(&db, SMALL_ROW_COUNT + 1, SMALL_ROW_COUNT + 1, USERS_RECORD_SIZE / 2);
    Table *table = open_users(&db);
    check_counters(table, SMALL_ROW_COUNT, SMALL_ROW_COUNT + 1);
    check_index(table, SMALL_ROW_COUNT);
    check_no_recovery(table);
    table_close(table);
    test_db_destroy(&db);
}

/* E25-03 */
static void test_ignore_zero_filled_tail_record(void)
{
    TestDb db = create_users_file(SMALL_ROW_COUNT);
    write_record_at(&db, SMALL_ROW_COUNT + 1, 0, USERS_RECORD_SIZE); /* 크기는 완전, id = 0 */
    Table *table = open_users(&db);
    check_counters(table, SMALL_ROW_COUNT, SMALL_ROW_COUNT + 1);
    check_index(table, SMALL_ROW_COUNT);
    check_no_recovery(table);
    table_close(table);
    test_db_destroy(&db);
}

/* E25-04 */
static void test_fix_wrong_next_id(void)
{
    TestDb db = create_users_file(SMALL_ROW_COUNT);
    write_counters(&db, SMALL_ROW_COUNT, 5);
    Table *table = open_users(&db);
    check_counters(table, SMALL_ROW_COUNT, SMALL_ROW_COUNT + 1);
    TableRecovery recovery;
    CHECK(table_find_recovery(table, &recovery));
    CHECK(recovery.old_next_id == 5);
    CHECK(recovery.row_count == SMALL_ROW_COUNT);
    table_close(table);
    test_db_destroy(&db);
}

/* E25-05 */
static void test_wrong_id_inside_rows_is_corrupt(void)
{
    TestDb db = create_users_file(SMALL_ROW_COUNT);
    write_record_at(&db, CORRUPT_POSITION, CORRUPT_ID, sizeof(int32_t));
    char path[TEST_PATH_MAX];
    users_path(&db, path);
    ErrorCode error = ERR_NONE;
    CHECK(table_open(path, &error) == NULL);
    CHECK(error == ERR_CORRUPT);
    test_db_destroy(&db);
}

/* E25-06 */
static void test_insert_after_recovery_continues_ids(void)
{
    TestDb db = create_users_file(SMALL_ROW_COUNT);
    write_counters(&db, SMALL_ROW_COUNT - 1, SMALL_ROW_COUNT);
    test_db_reopen(&db);
    CHECK(db.engine != NULL);
    CHECK(engine_warning_count(db.engine) == 1);
    CHECK(strstr(engine_warning(db.engine, 0), "users") != NULL);
    CHECK(insert_user(db.engine) == SMALL_ROW_COUNT + 1);
    test_db_destroy(&db);
}

/* E25-07 */
static void test_reopen_after_recovery_is_clean(void)
{
    TestDb db = create_users_file(SMALL_ROW_COUNT);
    write_counters(&db, SMALL_ROW_COUNT - 1, SMALL_ROW_COUNT);
    test_db_reopen(&db); /* 여기서 복구되고 헤더가 다시 쓰인다 */
    CHECK(engine_warning_count(db.engine) == 1);
    test_db_reopen(&db);
    CHECK(engine_warning_count(db.engine) == 0);
    engine_close(db.engine);
    db.engine = NULL;

    Table *table = open_users(&db);
    check_no_recovery(table);
    check_counters(table, SMALL_ROW_COUNT, SMALL_ROW_COUNT + 1);
    table_close(table);
    test_db_destroy(&db);
}

/* E25-01 (CLI): 복구 경고는 stderr 에 나가고 실행은 계속된다 */
static void test_cli_prints_recovery_warning(void)
{
    TestDb db = create_users_file(SMALL_ROW_COUNT);
    write_counters(&db, SMALL_ROW_COUNT - 1, SMALL_ROW_COUNT);
    char query_path[TEST_PATH_MAX];
    CHECK(test_write_file(db.dir, "query.sql", INSERT_USER ";\n", query_path));

    TestCapture out;
    TestCapture err;
    test_capture_open(&out);
    test_capture_open(&err);
    CHECK(out.stream != NULL && err.stream != NULL);
    char *argv[] = {"minidb", "-d", db.dir, "-q", query_path, NULL};
    int code = cli_run(5, argv, out.stream, err.stream);
    char *out_text = test_capture_close(&out);
    char *err_text = test_capture_close(&err);

    CHECK(code == 0);
    CHECK(strstr(err_text, "warning") != NULL);
    CHECK(strstr(err_text, "users") != NULL);
    CHECK(strstr(out_text, "1 statements, 0 errors") != NULL);
    free(out_text);
    free(err_text);
    test_db_destroy(&db);
}

int main(void)
{
    printf("test_index\n");
    RUN(test_rebuild_index_on_open);
    RUN(test_rebuild_bulk_in_batches);
    RUN(test_reopen_gives_same_index);
    RUN(test_empty_table_index);
    RUN(test_recover_records_after_stale_header);
    RUN(test_ignore_partial_tail_record);
    RUN(test_ignore_zero_filled_tail_record);
    RUN(test_fix_wrong_next_id);
    RUN(test_wrong_id_inside_rows_is_corrupt);
    RUN(test_insert_after_recovery_continues_ids);
    RUN(test_reopen_after_recovery_is_clean);
    RUN(test_cli_prints_recovery_warning);
    TEST_DONE();
    return 0;
}
