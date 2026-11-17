#include "executor.h"

#include <stdlib.h>
#include <string.h>

#include "btree.h"
#include "table.h"

/* 스캔은 레코드를 SCAN_BATCH 개씩 pread 한 번으로 읽는다 (행마다 시스템 콜을 하지 않기 위해) */
#define SCAN_BATCH 256
#define INITIAL_ROW_CAPACITY 16
#define NO_LIMIT (-1)
#define ID_COLUMN_NAME "id"

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

/* ---- SELECT: id 인덱스 경로 ---- */

/* id 조건이 허용하는 키 구간 [low, high]. int64 로 둬서 "id > INT32_MAX" 의 v + 1,
 * "id < INT32_MIN" 의 v - 1 이 넘치지 않는다 (그 경우 low > high 인 빈 구간이 된다) */
typedef struct {
    int64_t low;
    int64_t high;
} IdRange;

/* 아직 읽지 않은, offset 이 연속인 레코드 묶음 */
typedef struct {
    uint64_t first_offset;
    int32_t count;
} OffsetRun;

/* WHERE 가 id 조건이고 != 가 아니면 인덱스를 쓴다. != 는 거의 전부가 맞으므로 인덱스 이점이 없다.
 * 인덱스가 없는 테이블 (테스트에서 끈 경우) 은 선형 탐색 */
static bool can_use_id_index(const SelectStmt *select, const Plan *plan)
{
    return plan->has_where && strcmp(select->where.column, ID_COLUMN_NAME) == 0 &&
           select->where.op != OP_NE && table_index(plan->table) != NULL;
}

static IdRange id_range_of(const Condition *where)
{
    int64_t value = where->value.i;
    IdRange range = {INT32_MIN, INT32_MAX};
    switch (where->op) {
    case OP_EQ: range.low = value; range.high = value; break;
    case OP_GT: range.low = value + 1; break;
    case OP_GE: range.low = value; break;
    case OP_LT: range.high = value - 1; break;
    case OP_LE: range.high = value; break;
    case OP_NE: break; /* can_use_id_index 가 걸러서 오지 않는다 */
    }
    return range;
}

/* 묶음을 한 번의 pread 로 읽어 결과에 붙인다. 구간 안의 키만 모았으므로 조건 검사도 모두 통과한다 */
static ErrorCode flush_run(Scan *scan, OffsetRun *run, uint8_t *batch)
{
    if (run->count == 0)
        return ERR_NONE;
    ErrorCode error = table_read_records_at(scan->plan->table, run->first_offset, run->count,
                                            batch);
    if (error != ERR_NONE)
        return error;
    bool is_collected = collect_batch(scan, batch, run->count);
    run->count = 0;
    return is_collected ? ERR_NONE : ERR_INTERNAL;
}

/* offset 이 묶음의 바로 다음 레코드이고 묶음이 꽉 차지 않았으면 이어 붙일 수 있다.
 * id 가 1 부터 빈틈없이 이어지므로 범위 결과는 사실상 한 덩어리지만, 연속인지는 offset 으로 확인한다 */
static bool can_extend_run(const Scan *scan, const OffsetRun *run, uint64_t offset)
{
    return run->count > 0 && run->count < SCAN_BATCH &&
           offset == run->first_offset + (uint64_t)run->count * scan->buffer.record_size;
}

/* 이미 모은 행 + 읽으려고 묶어 둔 행이 LIMIT 에 닿았는지 (닿으면 더 순회하지 않는다) */
static bool is_planned_limit_reached(const Scan *scan, const OffsetRun *run)
{
    return scan->limit != NO_LIMIT && scan->buffer.count + run->count >= scan->limit;
}

/* = : 트리에서 한 번 찾고 레코드 1 개만 읽는다. 없는 id (0, 음수, max 초과) 는 읽기 없이 빈 결과 */
static ErrorCode scan_index_equal(Scan *scan, int32_t id, uint8_t *batch)
{
    uint64_t offset = 0;
    if (is_limit_reached(scan) || !btree_search(table_index(scan->plan->table), id, &offset))
        return ERR_NONE;
    OffsetRun run = {offset, 1};
    return flush_run(scan, &run, batch);
}

/* 범위: low 이상 첫 키부터 (low 가 없으면 맨 왼쪽부터) 리프 연결을 따라가다 high 를 넘으면 멈춘다 */
static ErrorCode scan_index_range(Scan *scan, IdRange range, uint8_t *batch)
{
    const BTree *index = table_index(scan->plan->table);
    BTreeIter iter = range.low <= INT32_MIN ? btree_seek_first(index)
                                            : btree_seek(index, (int32_t)range.low);
    OffsetRun run = {0, 0};
    int32_t key = 0;
    uint64_t offset = 0;
    while (!is_planned_limit_reached(scan, &run) && btree_iter_next(&iter, &key, &offset)) {
        if (key > range.high)
            break;
        if (!can_extend_run(scan, &run, offset)) {
            ErrorCode error = flush_run(scan, &run, batch);
            if (error != ERR_NONE)
                return error;
            run.first_offset = offset;
        }
        run.count++;
    }
    return flush_run(scan, &run, batch);
}

static ErrorCode scan_index(Scan *scan, uint8_t *batch)
{
    const Condition *where = &scan->select->where;
    if (where->op == OP_EQ)
        return scan_index_equal(scan, where->value.i, batch);
    IdRange range = id_range_of(where);
    if (range.low > range.high) /* id > INT32_MAX, id < INT32_MIN: 읽을 것이 없다 */
        return ERR_NONE;
    return scan_index_range(scan, range, batch);
}

/* ---- SELECT: 결과 ---- */

static Result select_result(const Plan *plan, RowBuffer *buffer, bool used_index)
{
    Result result;
    result_init(&result);
    result.column_count = plan->column_count;
    memcpy(result.columns, plan->columns, sizeof(plan->columns));
    result.record_size = (int)buffer->record_size;
    result.row_count = buffer->count;
    result.rows = buffer->rows; /* 소유권을 Result 로 넘긴다 (result_free 가 해제) */
    result.used_index = used_index;
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
    bool use_index = can_use_id_index(select, plan);
    ErrorCode error = use_index ? scan_index(&scan, batch) : scan_table(&scan, batch);
    free(batch);
    if (error == ERR_NONE)
        return select_result(plan, &scan.buffer, use_index);
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
