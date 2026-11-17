#include "executor.h"

#include <stdlib.h>

#include "table.h"

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

Result executor_run(const Statement *stmt, const Plan *plan)
{
    if (stmt->type == STMT_INSERT)
        return execute_insert(&stmt->insert, plan, stmt->line);
    /* TODO(S15): SELECT 스캔 */
    return result_error(ERR_UNSUPPORTED, stmt->line, "SELECT is not implemented yet");
}
