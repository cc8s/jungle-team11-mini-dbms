#ifndef TABLE_H
#define TABLE_H

/* .tbl 파일 하나 = 테이블 하나. 앞 HEADER_SIZE 바이트는 스키마와 카운터를 담은 헤더,
 * 그 뒤는 고정 길이 레코드가 id 순서로 이어진다. 파일 I/O 는 pread / pwrite 만 쓴다. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "btree.h"
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

/* 열린 테이블. 파일 fd 를 닫을 때까지 쥐고 있다 (INSERT 마다 열고 닫지 않는다).
 * id 인덱스 (B+ 트리) 도 테이블이 가진다. */
typedef struct Table Table;

/* 열 때 헤더를 바로잡았으면 그 전후 값 (경고 출력용) */
typedef struct {
    int32_t old_row_count;
    int32_t old_next_id;
    int32_t row_count;
    int32_t next_id;
} TableRecovery;

/* path 의 .tbl 파일을 읽기·쓰기로 열고 헤더를 검사한 뒤, 레코드를 모두 읽어 id 인덱스를 재구축한다.
 * 테이블 이름은 파일 이름에서 .tbl 을 뗀 것. 실패하면 NULL 이고 *out_error 에 이유를 쓴다:
 *   ERR_IO      파일을 열거나 읽거나 헤더를 고쳐 쓸 수 없음 (없음, 권한, 디렉터리 등)
 *   ERR_CORRUPT 헤더보다 작음, magic·version·컬럼·record_size 이상, 레코드가 row_count 보다 모자람,
 *               row_count 안의 위치 k 레코드 id 가 k 가 아님
 *   ERR_INTERNAL 메모리 부족, 테이블 락을 만들 수 없음
 * 헤더 복구 (비정상 종료 대비): row_count 뒤에 완전하고 id 가 이어지는 레코드가 있으면 채택하고,
 * 처음으로 어긋나는 레코드 (반쪽, id 불일치, 0 으로 채워짐) 부터는 무시한다.
 * next_id 는 row_count + 1 로 맞춘다. 값이 바뀌었으면 헤더를 다시 쓰고 table_find_recovery 로 알린다. */
Table *table_open(const char *path, ErrorCode *out_error);
void table_close(Table *table);

/* ---- 테이블 락 (LOCK_MODE TABLE_RW 에서 엔진이 쓴다) ----
 * 읽기 락은 여러 스레드가 동시에, 쓰기 락은 혼자 잡는다. 쓰기 우선이라 쓰기가 기다리기 시작하면
 * 새 읽기는 그 쓰기가 끝날 때까지 기다린다 (읽기가 계속 들어와도 쓰기가 굶지 않는다).
 * 한 스레드가 같은 테이블 락을 겹쳐 잡으면 안 된다 (재귀 불가). 해제는 둘 다 table_unlock */
void table_lock_read(Table *table);
void table_lock_write(Table *table);
void table_unlock(Table *table);

/* 열 때 헤더를 바로잡았으면 true 와 그 내용. 아니면 false */
bool table_find_recovery(const Table *table, TableRecovery *out_recovery);

/* id → 레코드 offset 인덱스. 테이블이 소유하므로 해제하지 않는다 */
const BTree *table_index(const Table *table);

/* table_open 의 인덱스 재구축 (복구 포함) 에 걸린 시간 (벤치마크용) */
double table_index_build_ms(const Table *table);

const char *table_name(const Table *table);
int table_column_count(const Table *table);
/* index 번째 컬럼 정의 (0 <= index < table_column_count) */
const ColumnDef *table_column(const Table *table, int index);
/* index 번째 컬럼이 레코드 안에서 시작하는 바이트 위치 */
int32_t table_column_offset(const Table *table, int index);
/* 이름이 정확히 같은 컬럼을 찾는다 (이름은 파서가 소문자로 정규화한다). 없으면 false */
bool table_find_column(const Table *table, const char *name, int *out_index);
int32_t table_record_count(const Table *table);
/* 다음 INSERT 가 받을 id */
int32_t table_next_id(const Table *table);
/* 레코드 1 개 바이트 수 (컬럼 크기 합) */
int32_t table_record_size(const Table *table);

/* ---- 레코드 ----
 * 레코드는 컬럼 순서대로 이어 붙인 table_record_size 바이트다.
 * INT 는 4 바이트 리틀 엔디언, VARCHAR(n) 은 값을 복사하고 남는 칸을 \0 으로 채운 n 바이트
 * (n 바이트를 꽉 채운 값은 \0 이 없다). */

/* id 와 id 를 뺀 나머지 컬럼 값 value_count 개를 out_record 에 직렬화한다.
 * 값은 validator 와 바인딩을 거친 것이어야 한다: 개수·타입이 안 맞거나, 길이를 넘거나,
 * 자리 표시자가 남아 있으면 호출 측 버그로 보고 ERR_INTERNAL. */
ErrorCode table_encode_record(const Table *table, int32_t id, const Value *values,
                              int value_count, uint8_t *out_record);

/* record 를 다음 id 자리에 쓰고 헤더의 next_id, row_count 를 갱신한 뒤 id 인덱스에 등록한다.
 * record 의 id 칸은 table_next_id 여야 한다 (아니면 ERR_INTERNAL).
 * next_id 가 INT32_MAX 를 넘게 되면 ERR_OUT_OF_RANGE (파일 변경 없음), 쓰기 실패는 ERR_IO (인덱스 변경 없음).
 * 인덱스 등록이 메모리 부족으로 실패하면 ERR_INTERNAL: 레코드는 이미 파일에 있고 *out_id 도 채워지며,
 * 다음 table_open 의 재구축에서 인덱스에 들어간다. */
ErrorCode table_append_record(Table *table, const uint8_t *record, int32_t *out_id);

/* id 번 레코드를 out_record (table_record_size 바이트) 에 읽는다. 없는 id 는 ERR_INTERNAL */
ErrorCode table_read_record(const Table *table, int32_t id, uint8_t *out_record);

/* first_id 부터 count 개를 한 번의 pread 로 out_records 에 이어서 읽는다.
 * 범위가 1 ~ row_count 밖이면 ERR_INTERNAL. count = 0 이면 아무것도 읽지 않고 성공 */
ErrorCode table_read_records(const Table *table, int32_t first_id, int32_t count,
                             uint8_t *out_records);

/* 인덱스가 돌려준 레코드 offset 에서 count 개를 한 번의 pread 로 읽는다 (연속 offset 묶음 읽기).
 * offset 이 레코드 경계가 아니거나 범위가 1 ~ row_count 밖이면 ERR_INTERNAL */
ErrorCode table_read_records_at(const Table *table, uint64_t offset, int32_t count,
                                uint8_t *out_records);

/* record 의 column 번째 INT 값 */
int32_t table_record_int(const Table *table, const uint8_t *record, int column);
/* record 의 column 번째 VARCHAR 값. \0 으로 끝나지 않을 수 있으니 *out_len 만큼만 읽는다.
 * 돌려준 포인터는 record 안을 가리킨다 */
const char *table_record_string(const Table *table, const uint8_t *record, int column,
                                size_t *out_len);

#endif
