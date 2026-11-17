#include "table.h"

#include <errno.h>
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
