#include "test.h"
#include "test_util.h"

#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#include "btree.h"
#include "btree_internal.h"
#include "cli.h"
#include "table_internal.h"

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

/* ---- S26 INSERT 연동 ---- */

#define ORDERS_SQL "CREATE TABLE orders (id INT, item VARCHAR(4));"
#define ORDERS_RECORD_SIZE 8 /* id 4 + item 4 */
#define INSERT_ORDER "INSERT INTO orders VALUES ('pen')"
#define INSERT_TOO_LONG "INSERT INTO users VALUES ('this name is far longer than 32 bytes', 1)"
#define THOUSAND_ROWS 1000
#define ALTERNATE_ROUNDS 50

static const Table *find_users(const Engine *engine)
{
    const Table *table = engine_find_table(engine, "users");
    CHECK(table != NULL);
    return table;
}

/* T26-01 */
static void test_insert_is_indexed_immediately(void)
{
    TestDb db = open_db();
    int32_t id = insert_user(db.engine);
    uint64_t offset = 0;
    CHECK(btree_search(table_index(find_users(db.engine)), id, &offset));
    CHECK(offset == expected_offset(id));
    test_db_destroy(&db);
}

/* T26-02 */
static void test_insert_thousand_rows(void)
{
    TestDb db = open_db();
    for (int i = 0; i < THOUSAND_ROWS; i++)
        insert_user(db.engine);
    check_index(find_users(db.engine), THOUSAND_ROWS);
    test_db_destroy(&db);
}

/* T26-03 */
static void test_inserted_index_matches_rebuilt(void)
{
    TestDb db = open_db();
    for (int i = 0; i < MEDIUM_ROW_COUNT; i++)
        insert_user(db.engine);
    check_index(find_users(db.engine), MEDIUM_ROW_COUNT); /* INSERT 로 쌓은 인덱스 */
    test_db_reopen(&db);
    CHECK(db.engine != NULL);
    check_index(find_users(db.engine), MEDIUM_ROW_COUNT); /* 파일에서 재구축한 인덱스 */
    CHECK(engine_warning_count(db.engine) == 0);
    test_db_destroy(&db);
}

/* E26-01 */
static void test_failed_validation_keeps_index(void)
{
    TestDb db = open_db();
    insert_user(db.engine);
    Result result = engine_execute(db.engine, INSERT_TOO_LONG, NULL, 0);
    CHECK(result.error == ERR_VALUE_TOO_LONG);
    result_free(&result);
    check_index(find_users(db.engine), 1);
    test_db_destroy(&db);
}

/* 레코드 하나를 table 에 직접 append 한다 (users: name, age) */
static ErrorCode append_user(Table *table, int32_t *out_id)
{
    uint8_t record[USERS_RECORD_SIZE];
    Value values[] = {value_from_string("kim", strlen("kim")), value_from_int(25)};
    CHECK(table_encode_record(table, table_next_id(table), values, 2, record) == ERR_NONE);
    value_free(&values[0]); /* 문자열 값은 복사본을 가진다 */
    return table_append_record(table, record, out_id);
}

/* E26-02 */
static void test_write_failure_keeps_index(void)
{
    TestDb db = create_users_file(SMALL_ROW_COUNT);
    Table *table = open_users(&db);
    char path[TEST_PATH_MAX];
    users_path(&db, path);

    /* 쓰기 실패 주입: 테이블의 fd 를 같은 파일의 읽기 전용 fd 로 바꿔 pwrite 가 실패하게 한다 */
    int writable_fd = table->fd;
    table->fd = open(path, O_RDONLY);
    CHECK(table->fd >= 0);
    int32_t id = 0;
    CHECK(append_user(table, &id) == ERR_IO);
    check_index(table, SMALL_ROW_COUNT);
    check_counters(table, SMALL_ROW_COUNT, SMALL_ROW_COUNT + 1);

    close(table->fd);
    table->fd = writable_fd;
    table_close(table);
    test_db_destroy(&db);
}

/* S26 구현 항목: 트리 삽입이 메모리 부족으로 실패해도 레코드는 파일에 있고, 다음 열기 재구축에서 들어온다 */
static void test_tree_failure_recovered_by_rebuild(void)
{
    TestDb db = create_users_file(BTREE_MAX_KEYS); /* 루트 리프가 꽉 참: 다음 삽입은 노드 할당 필요 */
    Table *table = open_users(&db);
    table->index->alloc_budget = 0;
    int32_t id = 0;
    CHECK(append_user(table, &id) == ERR_INTERNAL);
    CHECK(table_record_count(table) == BTREE_MAX_KEYS + 1); /* 파일과 헤더는 이미 갱신됨 */
    CHECK(btree_size(table_index(table)) == BTREE_MAX_KEYS);
    table_close(table);

    table = open_users(&db);
    check_index(table, BTREE_MAX_KEYS + 1);
    check_no_recovery(table); /* 헤더는 맞게 쓰였으므로 복구할 것도 없다 */
    table_close(table);
    test_db_destroy(&db);
}

/* E26-03 */
static void test_two_tables_keep_own_index(void)
{
    TestDb db = test_db_create(USERS_SQL ORDERS_SQL);
    CHECK(db.engine != NULL);
    for (int i = 0; i < ALTERNATE_ROUNDS; i++) {
        insert_user(db.engine);
        Result result = engine_execute(db.engine, INSERT_ORDER, NULL, 0);
        CHECK(result.error == ERR_NONE);
        result_free(&result);
    }
    check_index(find_users(db.engine), ALTERNATE_ROUNDS);
    const BTree *orders = table_index(engine_find_table(db.engine, "orders"));
    CHECK(btree_size(orders) == ALTERNATE_ROUNDS);
    for (int32_t id = 1; id <= ALTERNATE_ROUNDS; id++) {
        uint64_t offset = 0;
        CHECK(btree_search(orders, id, &offset));
        CHECK(offset == HEADER_SIZE + (uint64_t)(id - 1) * ORDERS_RECORD_SIZE);
    }
    test_db_destroy(&db);
}

/* ---- S27 SELECT 접근 경로 ---- */

#define ACCESS_ROW_COUNT 100
#define MULTI_LEAF_ROW_COUNT 1000
#define SQL_BUFFER_SIZE 256
#define ID_COLUMN 0
#define NAME_COLUMN 1

/* name = 'user<i>' 로 count 건을 넣은 DB (엔진 열린 상태) */
static TestDb open_named_users(int count)
{
    TestDb db = open_db();
    for (int i = 1; i <= count; i++) {
        char sql[SQL_BUFFER_SIZE];
        snprintf(sql, sizeof(sql), "INSERT INTO users VALUES ('user%d', %d)", i, i % 50);
        Result result = engine_execute(db.engine, sql, NULL, 0);
        CHECK(result.error == ERR_NONE);
        result_free(&result);
    }
    return db;
}

static Result select_ok(Engine *engine, const char *sql)
{
    Result result = engine_execute(engine, sql, NULL, 0);
    CHECK(result.error == ERR_NONE);
    return result;
}

/* sql 의 결과가 first_id 부터 연속인 row_count 행이고, 인덱스 사용 여부가 기대와 같은지 */
static void check_select_ids(Engine *engine, const char *sql, int64_t row_count,
                             int32_t first_id, bool used_index)
{
    Result result = select_ok(engine, sql);
    CHECK(result.row_count == row_count);
    CHECK(result.used_index == used_index);
    for (int64_t row = 0; row < result.row_count; row++)
        CHECK(result_get_int(&result, row, ID_COLUMN) == first_id + (int32_t)row);
    result_free(&result);
}

/* 테스트용으로 인덱스를 끄고 실행한다: 인덱스가 없는 테이블은 선형 탐색으로 간다 */
static Result select_without_index(Engine *engine, const char *sql)
{
    Table *table = (Table *)find_users(engine); /* 테스트에서만 const 를 푼다 */
    BTree *saved = table->index;
    table->index = NULL;
    Result result = select_ok(engine, sql);
    table->index = saved;
    return result;
}

/* T27-01 */
static void test_select_id_equal_uses_index(void)
{
    TestDb db = open_named_users(ACCESS_ROW_COUNT);
    check_select_ids(db.engine, "SELECT * FROM users WHERE id = 50", 1, 50, true);
    Result result = select_ok(db.engine, "SELECT * FROM users WHERE id = 50");
    size_t len = 0;
    const char *name = result_get_str(&result, 0, NAME_COLUMN, &len);
    CHECK(len == strlen("user50") && memcmp(name, "user50", len) == 0);
    result_free(&result);
    test_db_destroy(&db);
}

/* T27-02 */
static void test_select_id_greater_equal(void)
{
    TestDb db = open_named_users(ACCESS_ROW_COUNT);
    check_select_ids(db.engine, "SELECT * FROM users WHERE id >= 95", 6, 95, true);
    test_db_destroy(&db);
}

/* T27-03 */
static void test_select_id_less_than(void)
{
    TestDb db = open_named_users(ACCESS_ROW_COUNT);
    check_select_ids(db.engine, "SELECT * FROM users WHERE id < 3", 2, 1, true);
    check_select_ids(db.engine, "SELECT * FROM users WHERE id <= 3", 3, 1, true);
    test_db_destroy(&db);
}

/* T27-04 */
static void test_select_other_column_scans(void)
{
    TestDb db = open_named_users(ACCESS_ROW_COUNT);
    check_select_ids(db.engine, "SELECT * FROM users WHERE name = 'user50'", 1, 50, false);
    check_select_ids(db.engine, "SELECT * FROM users", ACCESS_ROW_COUNT, 1, false);
    test_db_destroy(&db);
}

/* T27-05 */
static void test_index_and_scan_give_same_rows(void)
{
    const char *queries[] = {
        "SELECT * FROM users WHERE id = 50",       "SELECT * FROM users WHERE id >= 95",
        "SELECT * FROM users WHERE id > 95",       "SELECT * FROM users WHERE id < 3",
        "SELECT * FROM users WHERE id <= 7",       "SELECT * FROM users WHERE id > 10 LIMIT 5",
        "SELECT name FROM users WHERE id >= 40 LIMIT 3", "SELECT * FROM users WHERE id = 101",
    };
    TestDb db = open_named_users(ACCESS_ROW_COUNT);
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        Result indexed = select_ok(db.engine, queries[i]);
        Result scanned = select_without_index(db.engine, queries[i]);
        CHECK(indexed.used_index && !scanned.used_index);
        CHECK(indexed.row_count == scanned.row_count);
        CHECK(indexed.record_size == scanned.record_size);
        CHECK(indexed.column_count == scanned.column_count);
        size_t bytes = (size_t)indexed.row_count * (size_t)indexed.record_size;
        CHECK(bytes == 0 || memcmp(indexed.rows, scanned.rows, bytes) == 0);
        result_free(&indexed);
        result_free(&scanned);
    }
    test_db_destroy(&db);
}

/* T27-06 */
static void test_select_id_range_with_limit(void)
{
    TestDb db = open_named_users(ACCESS_ROW_COUNT);
    check_select_ids(db.engine, "SELECT * FROM users WHERE id > 10 LIMIT 5", 5, 11, true);
    test_db_destroy(&db);
}

/* E27-01 */
static void test_select_id_not_equal_scans(void)
{
    TestDb db = open_named_users(ACCESS_ROW_COUNT);
    Result result = select_ok(db.engine, "SELECT * FROM users WHERE id != 50");
    CHECK(result.row_count == ACCESS_ROW_COUNT - 1);
    CHECK(!result.used_index);
    result_free(&result);
    test_db_destroy(&db);
}

/* E27-02 */
static void test_select_missing_id_is_empty(void)
{
    TestDb db = open_named_users(ACCESS_ROW_COUNT);
    check_select_ids(db.engine, "SELECT * FROM users WHERE id = 0", 0, 0, true);
    check_select_ids(db.engine, "SELECT * FROM users WHERE id = -5", 0, 0, true);
    check_select_ids(db.engine, "SELECT * FROM users WHERE id = 101", 0, 0, true);
    test_db_destroy(&db);
}

/* E27-03 */
static void test_select_range_outside_ids(void)
{
    TestDb db = open_named_users(ACCESS_ROW_COUNT);
    check_select_ids(db.engine, "SELECT * FROM users WHERE id > 100", 0, 0, true);
    check_select_ids(db.engine, "SELECT * FROM users WHERE id < 1", 0, 0, true);
    test_db_destroy(&db);
}

/* E27-04 */
static void test_select_extreme_bounds(void)
{
    TestDb db = open_named_users(ACCESS_ROW_COUNT);
    check_select_ids(db.engine, "SELECT * FROM users WHERE id >= -2147483648", ACCESS_ROW_COUNT,
                     1, true);
    check_select_ids(db.engine, "SELECT * FROM users WHERE id <= 2147483647", ACCESS_ROW_COUNT, 1,
                     true);
    check_select_ids(db.engine, "SELECT * FROM users WHERE id < -2147483648", 0, 0, true);
    test_db_destroy(&db);
}

/* E27-05 */
static void test_select_greater_than_max(void)
{
    TestDb db = open_named_users(ACCESS_ROW_COUNT);
    check_select_ids(db.engine, "SELECT * FROM users WHERE id > 2147483647", 0, 0, true);
    test_db_destroy(&db);
}

/* E27-06 */
static void test_select_empty_table_by_id(void)
{
    TestDb db = open_db();
    check_select_ids(db.engine, "SELECT * FROM users WHERE id = 1", 0, 0, true);
    check_select_ids(db.engine, "SELECT * FROM users WHERE id >= 1", 0, 0, true);
    test_db_destroy(&db);
}

/* E27-07 */
static void test_select_range_across_leaves(void)
{
    TestDb db = open_named_users(MULTI_LEAF_ROW_COUNT);
    CHECK(btree_height(table_index(find_users(db.engine))) >= 2); /* 리프가 여러 개 */
    check_select_ids(db.engine, "SELECT * FROM users WHERE id >= 2", MULTI_LEAF_ROW_COUNT - 1, 2,
                     true);
    test_db_destroy(&db);
}

/* E27-08 */
static void test_select_id_with_limit_zero(void)
{
    TestDb db = open_named_users(ACCESS_ROW_COUNT);
    check_select_ids(db.engine, "SELECT * FROM users WHERE id = 50 LIMIT 0", 0, 0, true);
    check_select_ids(db.engine, "SELECT * FROM users WHERE id >= 1 LIMIT 0", 0, 0, true);
    test_db_destroy(&db);
}

/* E27-09 */
static void test_select_name_by_id(void)
{
    TestDb db = open_named_users(ACCESS_ROW_COUNT);
    Result result = select_ok(db.engine, "SELECT name FROM users WHERE id = 50");
    CHECK(result.used_index);
    CHECK(result.row_count == 1);
    CHECK(result.column_count == 1);
    size_t len = 0;
    const char *name = result_get_str(&result, 0, 0, &len);
    CHECK(len == strlen("user50") && memcmp(name, "user50", len) == 0);
    result_free(&result);
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
    RUN(test_insert_is_indexed_immediately);
    RUN(test_insert_thousand_rows);
    RUN(test_inserted_index_matches_rebuilt);
    RUN(test_failed_validation_keeps_index);
    RUN(test_write_failure_keeps_index);
    RUN(test_tree_failure_recovered_by_rebuild);
    RUN(test_two_tables_keep_own_index);
    RUN(test_select_id_equal_uses_index);
    RUN(test_select_id_greater_equal);
    RUN(test_select_id_less_than);
    RUN(test_select_other_column_scans);
    RUN(test_index_and_scan_give_same_rows);
    RUN(test_select_id_range_with_limit);
    RUN(test_select_id_not_equal_scans);
    RUN(test_select_missing_id_is_empty);
    RUN(test_select_range_outside_ids);
    RUN(test_select_extreme_bounds);
    RUN(test_select_greater_than_max);
    RUN(test_select_empty_table_by_id);
    RUN(test_select_range_across_leaves);
    RUN(test_select_id_with_limit_zero);
    RUN(test_select_name_by_id);
    TEST_DONE();
    return 0;
}
