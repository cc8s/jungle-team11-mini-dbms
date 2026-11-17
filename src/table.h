#ifndef TABLE_H
#define TABLE_H

/* .tbl 파일 하나 = 테이블 하나. 앞 HEADER_SIZE 바이트는 스키마와 카운터를 담은 헤더,
 * 그 뒤는 고정 길이 레코드가 id 순서로 이어진다. 파일 I/O 는 pread / pwrite 만 쓴다. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"
#include "error.h"
#include "statement.h"

#define TABLE_MAGIC "MDB1"
#define TABLE_MAGIC_LEN 4
#define TABLE_VERSION 1
#define TABLE_FILE_EXT ".tbl"

/* 헤더의 메모리 표현. 파일에는 table_header_encode 가 필드별로 리틀 엔디언으로 쓴다
 * (구조체를 통째로 쓰면 패딩과 엔디언이 컴파일러·CPU 에 따라 달라진다). */
typedef struct {
    char magic[TABLE_MAGIC_LEN]; /* \0 없이 "MDB1" 4 바이트 */
    uint32_t version;
    int32_t next_id;     /* 다음 INSERT 가 받을 id */
    int32_t row_count;   /* 저장된 레코드 수 */
    int32_t record_size; /* 컬럼 크기 합 */
    int32_t column_count;
    ColumnDef columns[MAX_COLUMNS];
} TableHeader;

/* 새 테이블의 헤더를 만든다: next_id = 1, row_count = 0, record_size = 컬럼 크기 합.
 * create 는 파서가 스키마 규칙을 검사한 것이어야 한다. */
void table_header_from_create(const CreateStmt *create, TableHeader *out_header);

/* header 를 HEADER_SIZE 바이트로 직렬화한다. 쓰지 않는 칸과 예비 공간은 0 으로 채운다. */
void table_header_encode(const TableHeader *header, uint8_t out_bytes[HEADER_SIZE]);

/* HEADER_SIZE 바이트를 그대로 읽어 들인다. 값이 올바른지는 검사하지 않는다 (table_open 이 검사). */
void table_header_decode(const uint8_t bytes[HEADER_SIZE], TableHeader *out_header);

/* "<data_dir>/<table_name>.tbl" 을 out_path 에 쓴다. path_size 에 다 들어가지 않으면 false. */
bool table_build_path(const char *data_dir, const char *table_name, char *out_path,
                      size_t path_size);

/* path 에 새 테이블 파일을 만들고 헤더를 쓴다. 이미 있으면 건드리지 않고 ERR_IO.
 * 헤더를 쓰다 실패하면 반쯤 만든 파일을 지운다. */
ErrorCode table_create(const char *path, const CreateStmt *create);

/* 열린 테이블. 파일 fd 를 닫을 때까지 쥐고 있다 (INSERT 마다 열고 닫지 않는다). */
typedef struct Table Table;

/* path 의 .tbl 파일을 읽기·쓰기로 열고 헤더를 검사한다. 테이블 이름은 파일 이름에서 .tbl 을 뗀 것.
 * 실패하면 NULL 이고 *out_error 에 이유를 쓴다:
 *   ERR_IO      파일을 열거나 읽을 수 없음 (없음, 권한, 디렉터리 등)
 *   ERR_CORRUPT 헤더보다 작음, magic·version·컬럼·record_size 이상, 레코드가 row_count 보다 모자람
 *   ERR_INTERNAL 메모리 부족
 * 파일이 예상보다 크면 (쓰다 만 레코드) row_count 까지만 테이블로 보고 정상으로 연다. */
Table *table_open(const char *path, ErrorCode *out_error);
void table_close(Table *table);

const char *table_name(const Table *table);
int table_column_count(const Table *table);
/* index 번째 컬럼 정의 (0 <= index < table_column_count) */
const ColumnDef *table_column(const Table *table, int index);
/* 이름이 정확히 같은 컬럼을 찾는다 (이름은 파서가 소문자로 정규화한다). 없으면 false */
bool table_find_column(const Table *table, const char *name, int *out_index);
int32_t table_record_count(const Table *table);

#endif
