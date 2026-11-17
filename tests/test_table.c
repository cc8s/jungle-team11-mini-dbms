#include "test.h"
#include "test_util.h"

#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#include "parser.h"
#include "table.h"

#define USERS_SQL "CREATE TABLE users (id INT, name VARCHAR(32), age INT)"
#define USERS_RECORD_SIZE 40          /* 4 + 32 + 4 */
#define FIRST_COLUMN_OFFSET 24        /* 고정 필드 6 개 × 4B */
#define COLUMN_ENTRY_SIZE 40          /* name 32B + type 4B + size 4B */
#define NEXT_ID_OFFSET 8

/* CREATE 문 텍스트를 CreateStmt 로 바꾼다. CreateStmt 는 동적 메모리가 없어 해제가 필요 없다 */
static CreateStmt parse_create(const char *sql)
{
    Statement stmt;
    ParseError error;
    CHECK(parser_parse_single(sql, strlen(sql), &stmt, &error) == ERR_NONE);
    CHECK(stmt.type == STMT_CREATE);
    return stmt.create;
}

/* 파일 전체를 읽어 bytes 에 담고 크기를 돌려준다 */
static size_t read_file(const char *path, uint8_t *bytes, size_t capacity)
{
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    ssize_t n = pread(fd, bytes, capacity, 0);
    CHECK(n >= 0);
    close(fd);
    return (size_t)n;
}

static void build_table_path(const char *dir, const char *table, char *out_path)
{
    CHECK(table_build_path(dir, table, out_path, TEST_PATH_MAX));
}

static void check_same_header(const TableHeader *a, const TableHeader *b)
{
    CHECK(memcmp(a->magic, b->magic, TABLE_MAGIC_LEN) == 0);
    CHECK(a->version == b->version);
    CHECK(a->next_id == b->next_id);
    CHECK(a->row_count == b->row_count);
    CHECK(a->record_size == b->record_size);
    CHECK(a->column_count == b->column_count);
    for (int i = 0; i < a->column_count; i++) {
        CHECK_STR_EQ(a->columns[i].name, b->columns[i].name);
        CHECK(a->columns[i].type == b->columns[i].type);
        CHECK(a->columns[i].size == b->columns[i].size);
    }
}

/* T10-01 */ static void test_header_round_trip(void)
{
    CreateStmt create = parse_create(USERS_SQL);
    TableHeader header;
    table_header_from_create(&create, &header);
    header.next_id = 7;
    header.row_count = 6;

    uint8_t bytes[HEADER_SIZE];
    table_header_encode(&header, bytes);
    TableHeader decoded;
    table_header_decode(bytes, &decoded);
    check_same_header(&header, &decoded);
}

static void test_build_path(void)
{
    char path[TEST_PATH_MAX];
    CHECK(table_build_path("data", "users", path, sizeof(path)));
    CHECK_STR_EQ(path, "data/users.tbl");
    CHECK(!table_build_path("data", "users", path, strlen("data/users.tbl"))); /* \0 자리 없음 */
}

/* T10-02, T10-03 */ static void test_create_writes_header(void)
{
    char *dir = test_make_temp_dir();
    CHECK(dir != NULL);
    char path[TEST_PATH_MAX];
    build_table_path(dir, "users", path);
    CreateStmt create = parse_create(USERS_SQL);
    CHECK(table_create(path, &create) == ERR_NONE);

    uint8_t bytes[HEADER_SIZE * 2];
    CHECK(read_file(path, bytes, sizeof(bytes)) == HEADER_SIZE);
    TableHeader header;
    table_header_decode(bytes, &header);
    CHECK(memcmp(bytes, "MDB1", TABLE_MAGIC_LEN) == 0);
    CHECK(header.version == TABLE_VERSION);
    CHECK(header.next_id == 1);
    CHECK(header.row_count == 0);
    CHECK(header.record_size == USERS_RECORD_SIZE);
    CHECK(header.column_count == 3);
    CHECK_STR_EQ(header.columns[1].name, "name");
    CHECK(header.columns[1].type == COL_VARCHAR);
    CHECK(header.columns[1].size == 32);
    test_remove_dir(dir);
    free(dir);
}

/* T10-04 */ static void test_header_is_little_endian(void)
{
    CreateStmt create = parse_create("CREATE TABLE t (id INT)");
    TableHeader header;
    table_header_from_create(&create, &header);
    header.next_id = 0x01020304;
    uint8_t bytes[HEADER_SIZE];
    table_header_encode(&header, bytes);
    const uint8_t expected[] = { 0x04, 0x03, 0x02, 0x01 };
    CHECK(memcmp(bytes + NEXT_ID_OFFSET, expected, sizeof(expected)) == 0);
}

/* E10-01 */ static void test_create_does_not_overwrite(void)
{
    char *dir = test_make_temp_dir();
    CHECK(dir != NULL);
    char path[TEST_PATH_MAX];
    build_table_path(dir, "users", path);
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0);
    CHECK(pwrite(fd, "keep", 4, 0) == 4);
    close(fd);

    CreateStmt create = parse_create(USERS_SQL);
    CHECK(table_create(path, &create) == ERR_IO);
    uint8_t bytes[HEADER_SIZE];
    CHECK(read_file(path, bytes, sizeof(bytes)) == 4); /* 기존 내용 그대로 */
    CHECK(memcmp(bytes, "keep", 4) == 0);
    test_remove_dir(dir);
    free(dir);
}

/* E10-02 */ static void test_create_in_missing_dir(void)
{
    char *dir = test_make_temp_dir();
    CHECK(dir != NULL);
    char path[TEST_PATH_MAX];
    snprintf(path, sizeof(path), "%s/missing/users.tbl", dir);
    CreateStmt create = parse_create(USERS_SQL);
    CHECK(table_create(path, &create) == ERR_IO);
    test_remove_dir(dir);
    free(dir);
}

/* E10-03, E10-05 */ static void test_header_fits_max_columns(void)
{
    CreateStmt create = parse_create("CREATE TABLE t (id INT, c1 INT, c2 INT, c3 INT, c4 INT,"
                                     " c5 INT, c6 INT, c7 INT, c8 INT, c9 INT, c10 INT,"
                                     " c11 INT, c12 INT, c13 INT, c14 INT, c15 VARCHAR(9))");
    TableHeader header;
    table_header_from_create(&create, &header);
    uint8_t bytes[HEADER_SIZE];
    memset(bytes, 0xAB, sizeof(bytes)); /* 예비 공간을 encode 가 직접 0 으로 채우는지 본다 */
    table_header_encode(&header, bytes);

    TableHeader decoded;
    table_header_decode(bytes, &decoded);
    CHECK(decoded.column_count == MAX_COLUMNS);
    CHECK_STR_EQ(decoded.columns[MAX_COLUMNS - 1].name, "c15");
    CHECK(decoded.columns[MAX_COLUMNS - 1].size == 9);

    size_t used = FIRST_COLUMN_OFFSET + MAX_COLUMNS * COLUMN_ENTRY_SIZE;
    CHECK(used == 664);
    for (size_t i = used; i < HEADER_SIZE; i++)
        CHECK(bytes[i] == 0);
}

/* E10-04 */ static void test_record_size_with_large_varchars(void)
{
    char sql[TEST_PATH_MAX] = "CREATE TABLE t (id INT";
    for (int i = 1; i < MAX_COLUMNS; i++) {
        char column[MAX_NAME + 32];
        snprintf(column, sizeof(column), ", v%d VARCHAR(1024)", i);
        strcat(sql, column);
    }
    strcat(sql, ")");
    CreateStmt create = parse_create(sql);
    TableHeader header;
    table_header_from_create(&create, &header);
    CHECK(header.record_size == 15364);

    char *dir = test_make_temp_dir();
    CHECK(dir != NULL);
    char path[TEST_PATH_MAX];
    build_table_path(dir, "t", path);
    CHECK(table_create(path, &create) == ERR_NONE);
    test_remove_dir(dir);
    free(dir);
}

int main(void)
{
    RUN(test_header_round_trip);
    RUN(test_build_path);
    RUN(test_create_writes_header);
    RUN(test_header_is_little_endian);
    RUN(test_create_does_not_overwrite);
    RUN(test_create_in_missing_dir);
    RUN(test_header_fits_max_columns);
    RUN(test_record_size_with_large_varchars);
    TEST_DONE();
    return 0;
}
