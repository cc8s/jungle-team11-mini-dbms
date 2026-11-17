#include "executor.h"

#include <stdlib.h>
#include <string.h>

#include "table.h"

/* 스캔은 레코드를 SCAN_BATCH 개씩 pread 한 번으로 읽는다 (행마다 시스템 콜을 하지 않기 위해) */
#define SCAN_BATCH 256
#define INITIAL_ROW_CAPACITY 16
#define NO_LIMIT (-1)

/* id 를 받아 레코드를 만들고 덧붙인다. 실패하면 next_id 가 그대로라 id 를 소모하지 않는다 */
static Result execute_insert(const InsertStmt *insert, const Plan *plan, int line)
{
    Table *table = plan->table;
    uint8_t *record = malloc((size_t)table_record_size(table));
    if (record == NULL)
        return result_error(ERR_INTERNAL, line, "out of memory");

    int32_t id = table_next_id(table);
    ErrorCode error = table_encode_record(table, id, insert->values, insert->value_count, record);
    if (error == ERR_NONE)
        error = table_append_record(table, record, &id);
    free(record);

    if (error == ERR_OUT_OF_RANGE)
        return result_error(error, line, "table '%s' has no more ids", table_name(table));
    if (error != ERR_NONE)
        return result_error(error, line, "could not write to table '%s'", table_name(table));
    Result result;
    result_init(&result);
    result.affected_rows = 1;
    result.last_insert_id = id;
    return result;
}

/* ---- SELECT ---- */

/* 맞는 레코드를 원본 바이트 그대로 모으는 버퍼. 모자라면 2 배씩 늘려 복사 횟수를 O(log n) 로 둔다 */
typedef struct {
    uint8_t *rows;
    int64_t count;
    int64_t capacity;
    size_t record_size;
} RowBuffer;

static bool row_buffer_append(RowBuffer *buffer, const uint8_t *record)
{
    if (buffer->count == buffer->capacity) {
        int64_t capacity = buffer->capacity == 0 ? INITIAL_ROW_CAPACITY : buffer->capacity * 2;
        uint8_t *grown = realloc(buffer->rows, (size_t)capacity * buffer->record_size);
        if (grown == NULL)
            return false;
        buffer->rows = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->rows + (size_t)buffer->count * buffer->record_size, record,
           buffer->record_size);
    buffer->count++;
    return true;
}

/* 바이트 단위 비교, 같으면 짧은 쪽이 작다 ('ab' < 'abc'). 대소문자를 구분한다 */
static int compare_bytes(const char *a, size_t a_len, const char *b, size_t b_len)
{
    size_t common = a_len < b_len ? a_len : b_len;
    int cmp = memcmp(a, b, common);
    if (cmp != 0)
        return cmp;
    return (a_len > b_len) - (a_len < b_len);
}

/* 레코드 값과 조건 값을 비교한 부호 (-, 0, +). 타입은 validator 가 맞춰 두었다 */
static int compare_field(const Plan *plan, const Value *value, const uint8_t *record)
{
    if (value->type == VAL_INT) {
        int32_t field = table_record_int(plan->table, record, plan->where_column);
        return (field > value->i) - (field < value->i);
    }
    size_t len = 0;
    const char *field = table_record_string(plan->table, record, plan->where_column, &len);
    return compare_bytes(field, len, value->s.ptr, value->s.len);
}

static bool apply_operator(CompareOp op, int cmp)
{
    switch (op) {
    case OP_EQ: return cmp == 0;
    case OP_NE: return cmp != 0;
    case OP_LT: return cmp < 0;
    case OP_GT: return cmp > 0;
    case OP_LE: return cmp <= 0;
    case OP_GE: return cmp >= 0;
    }
    return false;
}

/* 스캔 하나의 상태. 함수마다 인자를 늘어놓지 않도록 묶는다 */
typedef struct {
    const SelectStmt *select;
    const Plan *plan;
    int64_t limit; /* NO_LIMIT 이면 끝까지 */
    RowBuffer buffer;
} Scan;

static bool matches(const Scan *scan, const uint8_t *record)
{
    if (!scan->plan->has_where)
        return true;
    const Condition *where = &scan->select->where;
    return apply_operator(where->op, compare_field(scan->plan, &where->value, record));
}

static bool is_limit_reached(const Scan *scan)
{
    return scan->limit != NO_LIMIT && scan->buffer.count >= scan->limit;
}

/* 읽어 온 묶음에서 맞는 레코드를 모은다. 메모리가 모자라면 false */
static bool collect_batch(Scan *scan, const uint8_t *batch, int32_t batch_count)
{
    for (int32_t i = 0; i < batch_count && !is_limit_reached(scan); i++) {
        const uint8_t *record = batch + (size_t)i * scan->buffer.record_size;
        if (matches(scan, record) && !row_buffer_append(&scan->buffer, record))
            return false;
    }
    return true;
}

/* id 1 부터 row_count 까지 묶음 단위로 읽으며 맞는 레코드를 모은다. LIMIT 에 닿으면 멈춘다 */
static ErrorCode scan_table(Scan *scan, uint8_t *batch)
{
    int32_t record_count = table_record_count(scan->plan->table);
    for (int32_t first = 1; first <= record_count && !is_limit_reached(scan);
         first += SCAN_BATCH) {
        int32_t left = record_count - first + 1;
        int32_t count = left < SCAN_BATCH ? left : SCAN_BATCH;
        ErrorCode error = table_read_records(scan->plan->table, first, count, batch);
        if (error != ERR_NONE)
            return error;
        if (!collect_batch(scan, batch, count))
            return ERR_INTERNAL;
    }
    return ERR_NONE;
}

static Result select_result(const Plan *plan, RowBuffer *buffer)
{
    Result result;
    result_init(&result);
    result.column_count = plan->column_count;
    memcpy(result.columns, plan->columns, sizeof(plan->columns));
    result.record_size = (int)buffer->record_size;
    result.row_count = buffer->count;
    result.rows = buffer->rows; /* 소유권을 Result 로 넘긴다 (result_free 가 해제) */
    result.used_index = false;  /* Phase 2 에서 id 조건이면 인덱스 경로 */
    return result;
}

static Result execute_select(const SelectStmt *select, const Plan *plan, int line)
{
    Scan scan = {
        .select = select,
        .plan = plan,
        .limit = select->has_limit ? select->limit : NO_LIMIT,
        .buffer = { .record_size = (size_t)table_record_size(plan->table) },
    };
    uint8_t *batch = malloc((size_t)SCAN_BATCH * scan.buffer.record_size);
    if (batch == NULL)
        return result_error(ERR_INTERNAL, line, "out of memory");
    ErrorCode error = scan_table(&scan, batch);
    free(batch);
    if (error == ERR_NONE)
        return select_result(plan, &scan.buffer);
    free(scan.buffer.rows);
    if (error == ERR_IO)
        return result_error(error, line, "could not read table '%s'", table_name(plan->table));
    return result_error(error, line, "out of memory while scanning '%s'",
                        table_name(plan->table));
}

Result executor_run(const Statement *stmt, const Plan *plan)
{
    if (stmt->type == STMT_INSERT)
        return execute_insert(&stmt->insert, plan, stmt->line);
    return execute_select(&stmt->select, plan, stmt->line);
}
