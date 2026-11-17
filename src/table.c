#include "table.h"

#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 헤더 레이아웃 (계획서 3.1). 오프셋 계산은 이 파일에만 둔다 */
#define MAGIC_OFFSET 0
#define VERSION_OFFSET 4
#define NEXT_ID_OFFSET 8
#define ROW_COUNT_OFFSET 12
#define RECORD_SIZE_OFFSET 16
#define COLUMN_COUNT_OFFSET 20
#define COLUMNS_OFFSET 24
#define COLUMN_NAME_SIZE MAX_NAME
#define COLUMN_TYPE_OFFSET COLUMN_NAME_SIZE      /* 컬럼 항목 안에서 */
#define COLUMN_SIZE_OFFSET (COLUMN_NAME_SIZE + 4)
#define COLUMN_ENTRY_SIZE (COLUMN_NAME_SIZE + 8) /* name 32B + type 4B + size 4B */
#define TABLE_FILE_MODE 0644
#define ID_COLUMN_NAME "id"
#define INT_COLUMN_SIZE ((int32_t)sizeof(int32_t))

_Static_assert(COLUMNS_OFFSET + MAX_COLUMNS * COLUMN_ENTRY_SIZE <= HEADER_SIZE,
               "header must fit MAX_COLUMNS columns");

/* ---- 리틀 엔디언 ---- */

/* CPU 엔디언과 상관없이 같은 바이트가 나오도록 시프트로 한 바이트씩 쓴다 */
static void write_u32_le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xFF);
    bytes[1] = (uint8_t)((value >> 8) & 0xFF);
    bytes[2] = (uint8_t)((value >> 16) & 0xFF);
    bytes[3] = (uint8_t)((value >> 24) & 0xFF);
}

static uint32_t read_u32_le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

/* int32_t ↔ uint32_t 는 2 의 보수 비트를 그대로 옮긴다 (음수도 왕복) */
static void write_i32_le(uint8_t *bytes, int32_t value)
{
    write_u32_le(bytes, (uint32_t)value);
}

static int32_t read_i32_le(const uint8_t *bytes)
{
    return (int32_t)read_u32_le(bytes);
}

/* ---- 헤더 ---- */

void table_header_from_create(const CreateStmt *create, TableHeader *out_header)
{
    memset(out_header, 0, sizeof(*out_header));
    memcpy(out_header->magic, TABLE_MAGIC, TABLE_MAGIC_LEN);
    out_header->version = TABLE_VERSION;
    out_header->next_id = 1;
    out_header->row_count = 0;
    out_header->column_count = create->column_count;
    int32_t record_size = 0;
    for (int i = 0; i < create->column_count; i++) {
        out_header->columns[i] = create->columns[i];
        record_size += create->columns[i].size;
    }
    out_header->record_size = record_size;
}

static void encode_column(const ColumnDef *column, uint8_t *entry)
{
    /* 이름은 \0 패딩 32 바이트. 엔트리를 먼저 0 으로 채워 두었으므로 길이만큼만 복사한다 */
    memcpy(entry, column->name, strnlen(column->name, COLUMN_NAME_SIZE - 1));
    write_i32_le(entry + COLUMN_TYPE_OFFSET, (int32_t)column->type);
    write_i32_le(entry + COLUMN_SIZE_OFFSET, column->size);
}

void table_header_encode(const TableHeader *header, uint8_t out_bytes[HEADER_SIZE])
{
    memset(out_bytes, 0, HEADER_SIZE);
    memcpy(out_bytes + MAGIC_OFFSET, header->magic, TABLE_MAGIC_LEN);
    write_u32_le(out_bytes + VERSION_OFFSET, header->version);
    write_i32_le(out_bytes + NEXT_ID_OFFSET, header->next_id);
    write_i32_le(out_bytes + ROW_COUNT_OFFSET, header->row_count);
    write_i32_le(out_bytes + RECORD_SIZE_OFFSET, header->record_size);
    write_i32_le(out_bytes + COLUMN_COUNT_OFFSET, header->column_count);
    for (int i = 0; i < header->column_count && i < MAX_COLUMNS; i++)
        encode_column(&header->columns[i], out_bytes + COLUMNS_OFFSET + i * COLUMN_ENTRY_SIZE);
}

static void decode_column(const uint8_t *entry, ColumnDef *out_column)
{
    memcpy(out_column->name, entry, COLUMN_NAME_SIZE);
    out_column->name[COLUMN_NAME_SIZE - 1] = '\0'; /* 손상된 파일이어도 문자열이 끝나게 한다 */
    out_column->type = (ColumnType)read_i32_le(entry + COLUMN_TYPE_OFFSET);
    out_column->size = read_i32_le(entry + COLUMN_SIZE_OFFSET);
}

void table_header_decode(const uint8_t bytes[HEADER_SIZE], TableHeader *out_header)
{
    memset(out_header, 0, sizeof(*out_header));
    memcpy(out_header->magic, bytes + MAGIC_OFFSET, TABLE_MAGIC_LEN);
    out_header->version = read_u32_le(bytes + VERSION_OFFSET);
    out_header->next_id = read_i32_le(bytes + NEXT_ID_OFFSET);
    out_header->row_count = read_i32_le(bytes + ROW_COUNT_OFFSET);
    out_header->record_size = read_i32_le(bytes + RECORD_SIZE_OFFSET);
    out_header->column_count = read_i32_le(bytes + COLUMN_COUNT_OFFSET);
    /* 컬럼 칸은 항상 MAX_COLUMNS 개 모두 읽는다: column_count 가 망가져 있어도 범위를 넘지 않는다 */
    for (int i = 0; i < MAX_COLUMNS; i++)
        decode_column(bytes + COLUMNS_OFFSET + i * COLUMN_ENTRY_SIZE, &out_header->columns[i]);
}

/* ---- 파일 ---- */

bool table_build_path(const char *data_dir, const char *table_name, char *out_path,
                      size_t path_size)
{
    int len = snprintf(out_path, path_size, "%s/%s%s", data_dir, table_name, TABLE_FILE_EXT);
    return len >= 0 && (size_t)len < path_size;
}

/* pread 도 요청보다 적게 읽을 수 있으므로 다 읽을 때까지 이어서 읽는다. 파일 끝에 먼저 닿으면 false */
static bool read_all_at(int fd, uint8_t *bytes, size_t len, off_t offset)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(fd, bytes + done, len - done, offset + (off_t)done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        done += (size_t)n;
    }
    return true;
}

/* pwrite 는 요청보다 적게 쓸 수 있으므로 다 쓸 때까지 이어서 쓴다 */
static bool write_all_at(int fd, const uint8_t *bytes, size_t len, off_t offset)
{
    size_t written = 0;
    while (written < len) {
        ssize_t n = pwrite(fd, bytes + written, len - written, offset + (off_t)written);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        written += (size_t)n;
    }
    return true;
}

ErrorCode table_create(const char *path, const CreateStmt *create)
{
    TableHeader header;
    uint8_t bytes[HEADER_SIZE];
    table_header_from_create(create, &header);
    table_header_encode(&header, bytes);

    /* O_EXCL: 확인과 생성을 한 번에 해서, 이미 있는 테이블을 덮어쓰는 일이 없게 한다 */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, TABLE_FILE_MODE);
    if (fd < 0)
        return ERR_IO;
    bool is_written = write_all_at(fd, bytes, HEADER_SIZE, 0);
    bool is_closed = close(fd) == 0;
    if (is_written && is_closed)
        return ERR_NONE;
    unlink(path); /* 헤더가 덜 쓰인 파일은 다음 열기에서 손상으로 보이므로 남기지 않는다 */
    return ERR_IO;
}

/* ---- 열기 ---- */

struct Table {
    int fd;
    char name[MAX_NAME];
    TableHeader header;
    int32_t column_offsets[MAX_COLUMNS]; /* 레코드 안에서 각 컬럼이 시작하는 바이트 */
};

static bool is_valid_column(const ColumnDef *column)
{
    if (column->name[0] == '\0')
        return false;
    if (column->type == COL_INT)
        return column->size == INT_COLUMN_SIZE;
    if (column->type == COL_VARCHAR)
        return column->size >= 1 && column->size <= MAX_VARCHAR;
    return false;
}

/* 컬럼 정의를 검사하고 크기 합을 돌려준다. 하나라도 이상하면 false.
 * 합은 int64 로 더한다: 손상된 size 값이 int32 덧셈을 넘치게 만들 수 있다 */
static bool check_columns(const TableHeader *header, int64_t *out_record_size)
{
    if (header->column_count < 1 || header->column_count > MAX_COLUMNS)
        return false;
    const ColumnDef *first = &header->columns[0];
    if (strcmp(first->name, ID_COLUMN_NAME) != 0 || first->type != COL_INT)
        return false;
    int64_t sum = 0;
    for (int i = 0; i < header->column_count; i++) {
        if (!is_valid_column(&header->columns[i]))
            return false;
        sum += header->columns[i].size;
    }
    *out_record_size = sum;
    return true;
}

/* 계획서 3.7 의 열기 규칙. file_size 는 fstat 으로 잰 실제 크기 */
static bool is_valid_header(const TableHeader *header, off_t file_size)
{
    int64_t record_size = 0;
    if (memcmp(header->magic, TABLE_MAGIC, TABLE_MAGIC_LEN) != 0)
        return false;
    if (header->version != TABLE_VERSION)
        return false;
    if (!check_columns(header, &record_size) || record_size != header->record_size)
        return false;
    if (header->row_count < 0)
        return false;
    /* 모자라면 손상, 남으면 쓰다 만 레코드로 보고 row_count 까지만 쓴다 */
    int64_t expected_size = HEADER_SIZE + (int64_t)header->row_count * record_size;
    return (int64_t)file_size >= expected_size;
}

/* "dir/users.tbl" → "users". .tbl 로 끝나지 않거나 이름이 너무 길면 false */
static bool extract_table_name(const char *path, char out_name[MAX_NAME])
{
    const char *slash = strrchr(path, '/');
    const char *base = slash == NULL ? path : slash + 1;
    size_t base_len = strlen(base);
    size_t ext_len = strlen(TABLE_FILE_EXT);
    if (base_len <= ext_len || strcmp(base + base_len - ext_len, TABLE_FILE_EXT) != 0)
        return false;
    size_t name_len = base_len - ext_len;
    if (name_len >= MAX_NAME)
        return false;
    memcpy(out_name, base, name_len);
    out_name[name_len] = '\0';
    return true;
}

/* 열린 fd 에서 헤더를 읽어 검사한다 */
static ErrorCode load_header(int fd, TableHeader *out_header)
{
    struct stat info;
    if (fstat(fd, &info) != 0)
        return ERR_IO;
    if (info.st_size < HEADER_SIZE)
        return ERR_CORRUPT;
    uint8_t bytes[HEADER_SIZE];
    if (!read_all_at(fd, bytes, HEADER_SIZE, 0))
        return ERR_IO;
    table_header_decode(bytes, out_header);
    return is_valid_header(out_header, info.st_size) ? ERR_NONE : ERR_CORRUPT;
}

static void compute_column_offsets(Table *table)
{
    int32_t offset = 0;
    for (int i = 0; i < table->header.column_count; i++) {
        table->column_offsets[i] = offset;
        offset += table->header.columns[i].size;
    }
}

Table *table_open(const char *path, ErrorCode *out_error)
{
    Table *table = calloc(1, sizeof(*table));
    if (table == NULL) {
        *out_error = ERR_INTERNAL;
        return NULL;
    }
    table->fd = -1;
    ErrorCode error = extract_table_name(path, table->name) ? ERR_NONE : ERR_CORRUPT;
    if (error == ERR_NONE) {
        table->fd = open(path, O_RDWR);
        error = table->fd < 0 ? ERR_IO : ERR_NONE;
    }
    if (error == ERR_NONE)
        error = load_header(table->fd, &table->header);
    if (error != ERR_NONE)
        goto cleanup;
    compute_column_offsets(table);
    *out_error = ERR_NONE;
    return table;

cleanup:
    table_close(table);
    *out_error = error;
    return NULL;
}

void table_close(Table *table)
{
    if (table == NULL)
        return;
    if (table->fd >= 0)
        close(table->fd);
    free(table);
}

const char *table_name(const Table *table)
{
    return table->name;
}

int table_column_count(const Table *table)
{
    return table->header.column_count;
}

const ColumnDef *table_column(const Table *table, int index)
{
    return &table->header.columns[index];
}

bool table_find_column(const Table *table, const char *name, int *out_index)
{
    for (int i = 0; i < table->header.column_count; i++) {
        if (strcmp(table->header.columns[i].name, name) == 0) {
            *out_index = i;
            return true;
        }
    }
    return false;
}

int32_t table_record_count(const Table *table)
{
    return table->header.row_count;
}

int32_t table_next_id(const Table *table)
{
    return table->header.next_id;
}

int32_t table_record_size(const Table *table)
{
    return table->header.record_size;
}

/* ---- 레코드 ---- */

/* id 번 레코드의 파일 위치. 이 공식은 여기에만 둔다 (CLAUDE.md 불변식) */
static off_t record_offset(const Table *table, int32_t id)
{
    return (off_t)HEADER_SIZE + (off_t)(id - 1) * table->header.record_size;
}

static ErrorCode encode_field(const ColumnDef *column, const Value *value, uint8_t *field)
{
    if (column->type == COL_INT && value->type == VAL_INT) {
        write_i32_le(field, value->i);
        return ERR_NONE;
    }
    if (column->type == COL_VARCHAR && value->type == VAL_STRING &&
        value->s.len <= (size_t)column->size) {
        memset(field, 0, (size_t)column->size);
        memcpy(field, value->s.ptr, value->s.len);
        return ERR_NONE;
    }
    return ERR_INTERNAL; /* 타입·길이·자리 표시자는 validator 와 바인딩이 이미 걸렀어야 한다 */
}

ErrorCode table_encode_record(const Table *table, int32_t id, const Value *values,
                              int value_count, uint8_t *out_record)
{
    if (value_count != table->header.column_count - 1)
        return ERR_INTERNAL;
    write_i32_le(out_record, id);
    for (int i = 1; i < table->header.column_count; i++) {
        ErrorCode error = encode_field(&table->header.columns[i], &values[i - 1],
                                       out_record + table->column_offsets[i]);
        if (error != ERR_NONE)
            return error;
    }
    return ERR_NONE;
}

/* next_id, row_count 는 헤더에서 붙어 있어 8 바이트를 한 번에 쓴다 */
static bool write_counters(Table *table, int32_t next_id, int32_t row_count)
{
    uint8_t bytes[ROW_COUNT_OFFSET + INT_COLUMN_SIZE - NEXT_ID_OFFSET];
    write_i32_le(bytes, next_id);
    write_i32_le(bytes + (ROW_COUNT_OFFSET - NEXT_ID_OFFSET), row_count);
    return write_all_at(table->fd, bytes, sizeof(bytes), NEXT_ID_OFFSET);
}

/* 순서: 레코드 → 헤더. 중간에 죽으면 파일 끝에 row_count 밖의 레코드가 남을 뿐이고
 * 열 때 그 부분은 무시된다 (S11, Phase 2 에서 복구). 메모리의 헤더는 두 쓰기가 다 끝난 뒤에만 바꾼다 */
ErrorCode table_append_record(Table *table, const uint8_t *record, int32_t *out_id)
{
    int32_t id = table->header.next_id;
    if (id == INT32_MAX || table->header.row_count == INT32_MAX)
        return ERR_OUT_OF_RANGE;
    if (read_i32_le(record) != id)
        return ERR_INTERNAL;
    size_t size = (size_t)table->header.record_size;
    if (!write_all_at(table->fd, record, size, record_offset(table, id)))
        return ERR_IO;
    if (!write_counters(table, id + 1, table->header.row_count + 1))
        return ERR_IO;
    table->header.next_id = id + 1;
    table->header.row_count++;
    *out_id = id;
    return ERR_NONE;
}

ErrorCode table_read_records(const Table *table, int32_t first_id, int32_t count,
                             uint8_t *out_records)
{
    /* int64 로 계산해 first_id + count 가 int32 를 넘어도 범위 검사가 틀리지 않게 한다 */
    int64_t last_id = (int64_t)first_id + count - 1;
    if (first_id < 1 || count < 0 || last_id > table->header.row_count)
        return ERR_INTERNAL;
    if (count == 0)
        return ERR_NONE;
    size_t size = (size_t)count * (size_t)table->header.record_size;
    if (!read_all_at(table->fd, out_records, size, record_offset(table, first_id)))
        return ERR_IO;
    return ERR_NONE;
}

ErrorCode table_read_record(const Table *table, int32_t id, uint8_t *out_record)
{
    return table_read_records(table, id, 1, out_record);
}

int32_t table_record_int(const Table *table, const uint8_t *record, int column)
{
    return read_i32_le(record + table->column_offsets[column]);
}

const char *table_record_string(const Table *table, const uint8_t *record, int column,
                                size_t *out_len)
{
    const char *field = (const char *)(record + table->column_offsets[column]);
    *out_len = strnlen(field, (size_t)table->header.columns[column].size);
    return field;
}
