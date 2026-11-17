#include "test.h"
#include "test_util.h"

#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#include "parser.h"
#include "table.h"
#include "table_internal.h"

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
    /* Phase 2 부터 열 때 위치 k 의 id 가 k 인지 검사하므로 (S25) 레코드 9 개에 id 를 채운다 */
    for (int32_t id = 1; id <= 9; id++) {
        uint8_t id_bytes[sizeof(int32_t)] = {(uint8_t)id, 0, 0, 0}; /* 리틀 엔디언, id < 256 */
        patch_file(file.path, HEADER_SIZE + (off_t)(id - 1) * USERS_RECORD_SIZE, id_bytes,
                   sizeof(id_bytes));
    }

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

/* ---- S12: 레코드 직렬화, 쓰기, 읽기 ---- */

#define PEOPLE_SQL "CREATE TABLE people (id INT, name VARCHAR(4), note VARCHAR(6), age INT)"
#define PEOPLE_RECORD_SIZE 18 /* 4 + 4 + 6 + 4 */
#define PEOPLE_VALUE_COUNT 3  /* id 를 뺀 값 */
#define NAME_COLUMN 1
#define NOTE_COLUMN 2
#define AGE_COLUMN 3

/* 임시 디렉터리에 people 테이블을 만들고 연다 */
typedef struct {
    TableFile file;
    Table *table;
} OpenTable;

static OpenTable open_people(void)
{
    OpenTable open_table;
    open_table.file = table_file_make();
    CreateStmt create = parse_create(PEOPLE_SQL);
    CHECK(table_create(open_table.file.path, &create) == ERR_NONE);
    ErrorCode error;
    open_table.table = table_open(open_table.file.path, &error);
    CHECK(open_table.table != NULL);
    return open_table;
}

static void reopen(OpenTable *open_table)
{
    table_close(open_table->table);
    ErrorCode error;
    open_table->table = table_open(open_table->file.path, &error);
    CHECK(open_table->table != NULL);
}

static void close_people(OpenTable *open_table)
{
    table_close(open_table->table);
    table_file_remove(&open_table->file);
}

static void encode_person(const Table *table, int32_t id, const char *name, const char *note,
                          int32_t age, uint8_t *out_record)
{
    Value values[PEOPLE_VALUE_COUNT] = {
        value_from_string(name, strlen(name)),
        value_from_string(note, strlen(note)),
        value_from_int(age),
    };
    CHECK(table_encode_record(table, id, values, PEOPLE_VALUE_COUNT, out_record) == ERR_NONE);
    for (int i = 0; i < PEOPLE_VALUE_COUNT; i++)
        value_free(&values[i]);
}

static int32_t append_person(Table *table, const char *name, const char *note, int32_t age)
{
    uint8_t record[PEOPLE_RECORD_SIZE];
    encode_person(table, table_next_id(table), name, note, age, record);
    int32_t id = 0;
    CHECK(table_append_record(table, record, &id) == ERR_NONE);
    return id;
}

static void check_string_field(const Table *table, const uint8_t *record, int column,
                               const char *expected)
{
    size_t len = 0;
    const char *text = table_record_string(table, record, column, &len);
    CHECK(len == strlen(expected));
    CHECK(memcmp(text, expected, len) == 0);
}

static void check_person(const Table *table, const uint8_t *record, int32_t id,
                         const char *name, const char *note, int32_t age)
{
    CHECK(table_record_int(table, record, 0) == id);
    check_string_field(table, record, NAME_COLUMN, name);
    check_string_field(table, record, NOTE_COLUMN, note);
    CHECK(table_record_int(table, record, AGE_COLUMN) == age);
}

/* T12-01 */ static void test_record_round_trip(void)
{
    OpenTable people = open_people();
    CHECK(table_record_size(people.table) == PEOPLE_RECORD_SIZE);
    uint8_t record[PEOPLE_RECORD_SIZE];
    encode_person(people.table, 7, "kim", "hello", 25, record);
    check_person(people.table, record, 7, "kim", "hello", 25);
    close_people(&people);
}

/* T12-02, T12-05 */ static void test_append_three_records(void)
{
    OpenTable people = open_people();
    CHECK(append_person(people.table, "kim", "a", 25) == 1);
    CHECK(append_person(people.table, "lee", "b", 30) == 2);
    CHECK(append_person(people.table, "park", "c", 35) == 3);
    CHECK(table_record_count(people.table) == 3);
    CHECK(table_next_id(people.table) == 4);

    uint8_t bytes[HEADER_SIZE * 2];
    CHECK(read_file(people.file.path, bytes, sizeof(bytes)) ==
          HEADER_SIZE + 3 * PEOPLE_RECORD_SIZE);
    TableHeader header;
    table_header_decode(bytes, &header); /* 헤더도 파일에 갱신되었다 */
    CHECK(header.next_id == 4);
    CHECK(header.row_count == 3);
    close_people(&people);
}

/* T12-03 */ static void test_records_survive_reopen(void)
{
    OpenTable people = open_people();
    append_person(people.table, "kim", "a", 25);
    append_person(people.table, "lee", "b", 30);
    reopen(&people);
    CHECK(table_record_count(people.table) == 2);
    CHECK(table_next_id(people.table) == 3);
    uint8_t record[PEOPLE_RECORD_SIZE];
    CHECK(table_read_record(people.table, 2, record) == ERR_NONE);
    check_person(people.table, record, 2, "lee", "b", 30);
    CHECK(append_person(people.table, "park", "c", 35) == 3);
    close_people(&people);
}

/* T12-04 */ static void test_read_many_records_at_once(void)
{
    OpenTable people = open_people();
    append_person(people.table, "kim", "a", 25);
    append_person(people.table, "lee", "b", 30);
    append_person(people.table, "park", "c", 35);
    uint8_t records[3 * PEOPLE_RECORD_SIZE];
    CHECK(table_read_records(people.table, 2, 2, records) == ERR_NONE);
    check_person(people.table, records, 2, "lee", "b", 30);
    check_person(people.table, records + PEOPLE_RECORD_SIZE, 3, "park", "c", 35);
    CHECK(table_read_records(people.table, 1, 3, records) == ERR_NONE);
    check_person(people.table, records, 1, "kim", "a", 25);
    close_people(&people);
}

/* E12-01, E12-02 */ static void test_varchar_full_and_empty(void)
{
    OpenTable people = open_people();
    uint8_t record[PEOPLE_RECORD_SIZE];
    encode_person(people.table, 1, "abcd", "", 0, record);
    const uint8_t name_bytes[] = { 'a', 'b', 'c', 'd' };
    CHECK(memcmp(record + 4, name_bytes, sizeof(name_bytes)) == 0); /* \0 없이 꽉 참 */
    for (int i = 0; i < 6; i++)
        CHECK(record[8 + i] == 0); /* 빈 문자열은 모두 \0 */
    check_person(people.table, record, 1, "abcd", "", 0);
    close_people(&people);
}

/* E12-03 */ static void test_varchar_full_of_multibyte(void)
{
    OpenTable people = open_people();
    uint8_t record[PEOPLE_RECORD_SIZE];
    encode_person(people.table, 1, "a", "가나", 0, record); /* UTF-8 3 바이트 × 2 = 6 */
    check_string_field(people.table, record, NOTE_COLUMN, "가나");
    close_people(&people);
}

/* E12-04 */ static void test_negative_int_round_trip(void)
{
    OpenTable people = open_people();
    append_person(people.table, "a", "", -1);
    append_person(people.table, "b", "", INT32_MIN);
    append_person(people.table, "c", "", INT32_MAX);
    reopen(&people);
    uint8_t records[3 * PEOPLE_RECORD_SIZE];
    CHECK(table_read_records(people.table, 1, 3, records) == ERR_NONE);
    CHECK(table_record_int(people.table, records, AGE_COLUMN) == -1);
    CHECK(table_record_int(people.table, records + PEOPLE_RECORD_SIZE, AGE_COLUMN) == INT32_MIN);
    CHECK(table_record_int(people.table, records + 2 * PEOPLE_RECORD_SIZE, AGE_COLUMN) ==
          INT32_MAX);
    close_people(&people);
}

/* E12-05 */ static void test_read_missing_id(void)
{
    OpenTable people = open_people();
    append_person(people.table, "kim", "a", 25);
    uint8_t records[2 * PEOPLE_RECORD_SIZE];
    CHECK(table_read_record(people.table, 0, records) == ERR_INTERNAL);
    CHECK(table_read_record(people.table, 2, records) == ERR_INTERNAL);
    CHECK(table_read_record(people.table, -1, records) == ERR_INTERNAL);
    CHECK(table_read_records(people.table, 1, 2, records) == ERR_INTERNAL);
    CHECK(table_read_records(people.table, 0, 1, records) == ERR_INTERNAL);
    CHECK(table_read_records(people.table, 1, -1, records) == ERR_INTERNAL);
    CHECK(table_read_records(people.table, INT32_MAX, INT32_MAX, records) == ERR_INTERNAL);
    close_people(&people);
}

/* E12-06 */ static void test_append_when_next_id_is_max(void)
{
    TableFile file = table_file_make();
    CreateStmt create = parse_create(PEOPLE_SQL);
    TableHeader header;
    table_header_from_create(&create, &header);
    write_table_file(file.path, &header, 0);
    ErrorCode error;
    Table *table = table_open(file.path, &error);
    CHECK(table != NULL);
    /* 이 id 를 쓰면 다음 id 가 int32 를 넘는다. 일관된 헤더 (row_count = INT32_MAX - 1) 는 파일이
     * 수십 GB 라 만들 수 없고, 어긋난 헤더는 열 때 복구되므로 (S25) 열린 뒤 메모리에서 바꾼다 */
    table->header.next_id = INT32_MAX;

    uint8_t record[PEOPLE_RECORD_SIZE];
    encode_person(table, INT32_MAX, "kim", "a", 25, record);
    int32_t id = 0;
    CHECK(table_append_record(table, record, &id) == ERR_OUT_OF_RANGE);
    CHECK(table_next_id(table) == INT32_MAX);
    CHECK(table_record_count(table) == 0);
    uint8_t bytes[HEADER_SIZE * 2];
    CHECK(read_file(file.path, bytes, sizeof(bytes)) == HEADER_SIZE); /* 파일 변경 없음 */
    table_close(table);
    table_file_remove(&file);
}

/* E12-07 */ static void test_read_zero_records(void)
{
    OpenTable people = open_people();
    uint8_t untouched = 0xAB;
    CHECK(table_read_records(people.table, 1, 0, &untouched) == ERR_NONE); /* 빈 테이블도 */
    CHECK(untouched == 0xAB);
    close_people(&people);
}

static void test_append_rejects_wrong_id(void)
{
    OpenTable people = open_people();
    uint8_t record[PEOPLE_RECORD_SIZE];
    encode_person(people.table, 5, "kim", "a", 25, record); /* next_id 는 1 */
    int32_t id = 0;
    CHECK(table_append_record(people.table, record, &id) == ERR_INTERNAL);
    CHECK(table_record_count(people.table) == 0);
    close_people(&people);
}

static void test_encode_rejects_unbound_values(void)
{
    OpenTable people = open_people();
    uint8_t record[PEOPLE_RECORD_SIZE];
    Value values[PEOPLE_VALUE_COUNT] = {
        value_from_string("toolong", strlen("toolong")),
        value_from_string("", 0),
        value_from_int(1),
    };
    CHECK(table_encode_record(people.table, 1, values, PEOPLE_VALUE_COUNT, record) ==
          ERR_INTERNAL); /* 길이 검사는 validator 몫이라 여기 오면 버그 */
    CHECK(table_encode_record(people.table, 1, values, 2, record) == ERR_INTERNAL);
    value_free(&values[0]);
    values[0] = value_placeholder(0);
    CHECK(table_encode_record(people.table, 1, values, PEOPLE_VALUE_COUNT, record) ==
          ERR_INTERNAL);
    values[0] = value_from_int(3); /* VARCHAR 칸에 INT */
    CHECK(table_encode_record(people.table, 1, values, PEOPLE_VALUE_COUNT, record) ==
          ERR_INTERNAL);
    for (int i = 0; i < PEOPLE_VALUE_COUNT; i++)
        value_free(&values[i]);
    close_people(&people);
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
    RUN(test_record_round_trip);
    RUN(test_append_three_records);
    RUN(test_records_survive_reopen);
    RUN(test_read_many_records_at_once);
    RUN(test_varchar_full_and_empty);
    RUN(test_varchar_full_of_multibyte);
    RUN(test_negative_int_round_trip);
    RUN(test_read_missing_id);
    RUN(test_append_when_next_id_is_max);
    RUN(test_read_zero_records);
    RUN(test_append_rejects_wrong_id);
    RUN(test_encode_rejects_unbound_values);
    TEST_DONE();
    return 0;
}
