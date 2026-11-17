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
#define VERSION_OFFSET 4
#define RECORD_SIZE_OFFSET 16
#define FILE_MODE 0644
#define NO_PERMISSION_MODE 0000

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

/* ---- S11: 열기, 무결성 검사 ---- */

/* 임시 디렉터리 하나와 그 안의 users.tbl 경로 */
typedef struct {
    char *dir;
    char path[TEST_PATH_MAX];
} TableFile;

static TableFile table_file_make(void)
{
    TableFile file;
    file.dir = test_make_temp_dir();
    CHECK(file.dir != NULL);
    build_table_path(file.dir, "users", file.path);
    return file;
}

static void table_file_remove(TableFile *file)
{
    test_remove_dir(file->dir);
    free(file->dir);
}

static TableHeader users_header(void)
{
    CreateStmt create = parse_create(USERS_SQL);
    TableHeader header;
    table_header_from_create(&create, &header);
    return header;
}

/* header 를 쓰고 그 뒤에 0 으로 채운 body_size 바이트를 붙인 파일을 만든다 */
static void write_table_file(const char *path, const TableHeader *header, size_t body_size)
{
    uint8_t bytes[HEADER_SIZE];
    table_header_encode(header, bytes);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, FILE_MODE);
    CHECK(fd >= 0);
    CHECK(pwrite(fd, bytes, HEADER_SIZE, 0) == HEADER_SIZE);
    CHECK(ftruncate(fd, (off_t)(HEADER_SIZE + body_size)) == 0);
    close(fd);
}

static void write_raw_file(const char *path, const void *bytes, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, FILE_MODE);
    CHECK(fd >= 0);
    CHECK(pwrite(fd, bytes, len, 0) == (ssize_t)len);
    close(fd);
}

static void patch_file(const char *path, off_t offset, const void *bytes, size_t len)
{
    int fd = open(path, O_WRONLY);
    CHECK(fd >= 0);
    CHECK(pwrite(fd, bytes, len, offset) == (ssize_t)len);
    close(fd);
}

static ErrorCode open_error(const char *path)
{
    ErrorCode error = ERR_NONE;
    Table *table = table_open(path, &error);
    CHECK(table == NULL);
    return error;
}

/* header 로 만든 파일을 열었을 때의 에러 */
static ErrorCode open_header_error(const TableHeader *header, size_t body_size)
{
    TableFile file = table_file_make();
    write_table_file(file.path, header, body_size);
    ErrorCode error = open_error(file.path);
    table_file_remove(&file);
    return error;
}

/* T11-01 */ static void test_open_created_table(void)
{
    TableFile file = table_file_make();
    CreateStmt create = parse_create(USERS_SQL);
    CHECK(table_create(file.path, &create) == ERR_NONE);

    ErrorCode error = ERR_INTERNAL;
    Table *table = table_open(file.path, &error);
    CHECK(table != NULL);
    CHECK(error == ERR_NONE);
    CHECK_STR_EQ(table_name(table), "users");
    CHECK(table_column_count(table) == 3);
    CHECK(table_record_count(table) == 0);
    for (int i = 0; i < create.column_count; i++) {
        const ColumnDef *column = table_column(table, i);
        CHECK_STR_EQ(column->name, create.columns[i].name);
        CHECK(column->type == create.columns[i].type);
        CHECK(column->size == create.columns[i].size);
    }
    table_close(table);
    table_file_remove(&file);
}

/* T11-02, T11-03 */ static void test_find_column(void)
{
    TableFile file = table_file_make();
    CreateStmt create = parse_create(USERS_SQL);
    CHECK(table_create(file.path, &create) == ERR_NONE);
    ErrorCode error;
    Table *table = table_open(file.path, &error);
    CHECK(table != NULL);

    int index = -1;
    CHECK(table_find_column(table, "age", &index));
    CHECK(index == 2);
    CHECK(table_find_column(table, "id", &index));
    CHECK(index == 0);
    CHECK(!table_find_column(table, "email", &index));
    CHECK(!table_find_column(table, "", &index));
    table_close(table);
    table_file_remove(&file);
}

static void test_close_null_is_safe(void)
{
    table_close(NULL);
}

/* E11-01 */ static void test_open_missing_file(void)
{
    TableFile file = table_file_make();
    CHECK(open_error(file.path) == ERR_IO);
    table_file_remove(&file);
}

/* E11-02, E11-03 */ static void test_open_too_small_file(void)
{
    TableFile file = table_file_make();
    write_raw_file(file.path, "", 0);
    CHECK(open_error(file.path) == ERR_CORRUPT);

    uint8_t bytes[HEADER_SIZE];
    TableHeader header = users_header();
    table_header_encode(&header, bytes);
    write_raw_file(file.path, bytes, HEADER_SIZE - 1); /* 올바른 헤더의 마지막 1 바이트만 없음 */
    CHECK(open_error(file.path) == ERR_CORRUPT);
    table_file_remove(&file);
}

/* E11-04 */ static void test_open_bad_magic(void)
{
    TableFile file = table_file_make();
    TableHeader header = users_header();
    write_table_file(file.path, &header, 0);
    patch_file(file.path, 3, "2", 1); /* "MDB1" → "MDB2" */
    CHECK(open_error(file.path) == ERR_CORRUPT);
    table_file_remove(&file);
}

/* E11-05 */ static void test_open_unsupported_version(void)
{
    TableHeader header = users_header();
    header.version = 99;
    CHECK(open_header_error(&header, 0) == ERR_CORRUPT);
}

/* E11-06 */ static void test_open_wrong_record_size(void)
{
    TableFile file = table_file_make();
    TableHeader header = users_header();
    write_table_file(file.path, &header, 0);
    const uint8_t wrong[] = { USERS_RECORD_SIZE + 1, 0, 0, 0 };
    patch_file(file.path, RECORD_SIZE_OFFSET, wrong, sizeof(wrong));
    CHECK(open_error(file.path) == ERR_CORRUPT);
    table_file_remove(&file);
}

static void test_open_bad_columns(void)
{
    TableHeader header = users_header();
    header.column_count = 0;
    CHECK(open_header_error(&header, 0) == ERR_CORRUPT);
    header.column_count = MAX_COLUMNS + 1;
    CHECK(open_header_error(&header, 0) == ERR_CORRUPT);

    header = users_header();
    snprintf(header.columns[0].name, MAX_NAME, "key"); /* 첫 컬럼이 id 가 아님 */
    CHECK(open_header_error(&header, 0) == ERR_CORRUPT);

    header = users_header();
    header.columns[1].type = (ColumnType)7; /* 모르는 타입 */
    CHECK(open_header_error(&header, 0) == ERR_CORRUPT);

    header = users_header();
    header.columns[1].size = 0; /* VARCHAR(0) */
    header.record_size -= 32;
    CHECK(open_header_error(&header, 0) == ERR_CORRUPT);

    header = users_header();
    header.columns[2].size = 8; /* INT 인데 4 바이트가 아님 */
    header.record_size += 4;
    CHECK(open_header_error(&header, 0) == ERR_CORRUPT);

    header = users_header();
    header.columns[2].name[0] = '\0'; /* 이름 없음 */
    CHECK(open_header_error(&header, 0) == ERR_CORRUPT);
}

static void test_open_negative_row_count(void)
{
    TableHeader header = users_header();
    header.row_count = -1;
    CHECK(open_header_error(&header, 0) == ERR_CORRUPT);
}

/* E11-07 */ static void test_open_missing_records(void)
{
    TableHeader header = users_header();
    header.row_count = 10;
    header.next_id = 11;
    CHECK(open_header_error(&header, 9 * USERS_RECORD_SIZE) == ERR_CORRUPT);
}

/* E11-08 */ static void test_open_ignores_partial_last_record(void)
{
    TableFile file = table_file_make();
    TableHeader header = users_header();
    header.row_count = 9;
    header.next_id = 10;
    write_table_file(file.path, &header, 9 * USERS_RECORD_SIZE + USERS_RECORD_SIZE / 2);

    ErrorCode error = ERR_INTERNAL;
    Table *table = table_open(file.path, &error);
    CHECK(table != NULL);
    CHECK(error == ERR_NONE);
    CHECK(table_record_count(table) == 9);
    table_close(table);
    table_file_remove(&file);
}

/* E11-09 */ static void test_open_text_file(void)
{
    TableFile file = table_file_make();
    char text[HEADER_SIZE * 2];
    memset(text, 'a', sizeof(text));
    memcpy(text, "id,name,age\n1,kim,25\n", strlen("id,name,age\n1,kim,25\n"));
    write_raw_file(file.path, text, sizeof(text));
    CHECK(open_error(file.path) == ERR_CORRUPT);
    write_raw_file(file.path, "hello\n", strlen("hello\n"));
    CHECK(open_error(file.path) == ERR_CORRUPT);
    table_file_remove(&file);
}

/* E11-10 */ static void test_open_without_permission(void)
{
    TableFile file = table_file_make();
    CreateStmt create = parse_create(USERS_SQL);
    CHECK(table_create(file.path, &create) == ERR_NONE);
    CHECK(chmod(file.path, NO_PERMISSION_MODE) == 0);
    /* root 는 권한 비트를 무시하고 열 수 있어 이 확인은 일반 사용자일 때만 의미가 있다 */
    if (geteuid() != 0)
        CHECK(open_error(file.path) == ERR_IO);
    CHECK(chmod(file.path, FILE_MODE) == 0);

    /* 권한과 상관없이 open 자체가 실패하는 경우: 디렉터리를 테이블로 열기 */
    char dir_path[TEST_PATH_MAX];
    build_table_path(file.dir, "folder", dir_path);
    CHECK(mkdir(dir_path, 0755) == 0);
    CHECK(open_error(dir_path) == ERR_IO);
    table_file_remove(&file);
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
    RUN(test_open_created_table);
    RUN(test_find_column);
    RUN(test_close_null_is_safe);
    RUN(test_open_missing_file);
    RUN(test_open_too_small_file);
    RUN(test_open_bad_magic);
    RUN(test_open_unsupported_version);
    RUN(test_open_wrong_record_size);
    RUN(test_open_bad_columns);
    RUN(test_open_negative_row_count);
    RUN(test_open_missing_records);
    RUN(test_open_ignores_partial_last_record);
    RUN(test_open_text_file);
    RUN(test_open_without_permission);
    TEST_DONE();
    return 0;
}
