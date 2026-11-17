#include "validator.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ID_COLUMN 0 /* 첫 컬럼은 항상 자동 부여 id */

static ErrorCode fail(char out_message[MAX_MESSAGE], ErrorCode code, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static ErrorCode fail(char out_message[MAX_MESSAGE], ErrorCode code, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(out_message, MAX_MESSAGE, format, args);
    va_end(args);
    return code;
}

static const char *type_name(ColumnType type)
{
    return type == COL_INT ? "INT" : "VARCHAR";
}

static const char *value_kind(const Value *value)
{
    return value->type == VAL_INT ? "an integer" : "a string";
}

Table *validator_find_table(Table *const *tables, int table_count, const char *name)
{
    for (int i = 0; i < table_count; i++) {
        if (strcmp(table_name(tables[i]), name) == 0)
            return tables[i];
    }
    return NULL;
}

static ErrorCode find_table(Table *const *tables, int table_count, const char *name,
                            Plan *plan, char out_message[MAX_MESSAGE])
{
    plan->table = validator_find_table(tables, table_count, name);
    if (plan->table == NULL)
        return fail(out_message, ERR_UNKNOWN_TABLE, "unknown table '%s'", name);
    return ERR_NONE;
}

/* 값 하나가 컬럼에 들어갈 수 있는지. 타입은 정확히 같아야 한다 (암묵 변환 없음) */
static ErrorCode check_value(const ColumnDef *column, const Value *value, bool check_length,
                             char out_message[MAX_MESSAGE])
{
    if (value->type == VAL_PARAM)
        return fail(out_message, ERR_PARAM, "placeholder '?' needs a bound parameter");
    bool is_int = value->type == VAL_INT;
    if (is_int != (column->type == COL_INT))
        return fail(out_message, ERR_TYPE_MISMATCH, "column '%s' is %s but got %s",
                    column->name, type_name(column->type), value_kind(value));
    /* 바이트 기준. 자르지 않고 거절한다 (한글 3 글자 = 9 바이트) */
    if (check_length && !is_int && value->s.len > (size_t)column->size)
        return fail(out_message, ERR_VALUE_TOO_LONG,
                    "value for '%s' is %zu bytes but VARCHAR(%d) holds at most %d",
                    column->name, value->s.len, column->size, column->size);
    return ERR_NONE;
}

/* ---- INSERT ---- */

static ErrorCode check_insert(const InsertStmt *insert, Plan *plan, char out_message[MAX_MESSAGE])
{
    int column_count = table_column_count(plan->table);
    int expected = column_count - 1; /* id 는 자동 부여 */
    if (insert->value_count == column_count)
        return fail(out_message, ERR_AUTO_ID,
                    "id is assigned automatically; give %d values without id", expected);
    if (insert->value_count != expected)
        return fail(out_message, ERR_VALUE_COUNT, "table '%s' needs %d values but got %d",
                    table_name(plan->table), expected, insert->value_count);
    for (int i = 0; i < insert->value_count; i++) {
        const ColumnDef *column = table_column(plan->table, i + ID_COLUMN + 1);
        ErrorCode error = check_value(column, &insert->values[i], true, out_message);
        if (error != ERR_NONE)
            return error;
    }
    return ERR_NONE;
}

/* ---- SELECT ---- */

static void add_result_column(Plan *plan, int table_column_index)
{
    const ColumnDef *column = table_column(plan->table, table_column_index);
    ResultColumn *result_column = &plan->columns[plan->column_count];
    snprintf(result_column->name, sizeof(result_column->name), "%s", column->name);
    result_column->type = column->type;
    result_column->size = column->size;
    result_column->offset = table_column_offset(plan->table, table_column_index);
    plan->column_indexes[plan->column_count] = table_column_index;
    plan->column_count++;
}

static ErrorCode find_column(const Table *table, const char *name, int *out_index,
                             char out_message[MAX_MESSAGE])
{
    if (!table_find_column(table, name, out_index))
        return fail(out_message, ERR_UNKNOWN_COLUMN, "unknown column '%s' in table '%s'", name,
                    table_name(table));
    return ERR_NONE;
}

static ErrorCode plan_select_columns(const SelectStmt *select, Plan *plan,
                                     char out_message[MAX_MESSAGE])
{
    if (select->select_all) {
        for (int i = 0; i < table_column_count(plan->table); i++)
            add_result_column(plan, i);
        return ERR_NONE;
    }
    for (int i = 0; i < select->column_count; i++) {
        int index = 0;
        ErrorCode error = find_column(plan->table, select->columns[i], &index, out_message);
        if (error != ERR_NONE)
            return error;
        add_result_column(plan, index);
    }
    return ERR_NONE;
}

/* WHERE 값은 길이를 보지 않는다: 칸보다 긴 문자열은 어떤 행과도 같지 않을 뿐 에러가 아니다 */
static ErrorCode plan_where(const Condition *where, Plan *plan, char out_message[MAX_MESSAGE])
{
    int index = 0;
    ErrorCode error = find_column(plan->table, where->column, &index, out_message);
    if (error == ERR_NONE)
        error = check_value(table_column(plan->table, index), &where->value, false, out_message);
    if (error != ERR_NONE)
        return error;
    plan->has_where = true;
    plan->where_column = index;
    plan->where_offset = table_column_offset(plan->table, index);
    return ERR_NONE;
}

static ErrorCode check_select(const SelectStmt *select, Plan *plan, char out_message[MAX_MESSAGE])
{
    ErrorCode error = plan_select_columns(select, plan, out_message);
    if (error == ERR_NONE && select->has_where)
        error = plan_where(&select->where, plan, out_message);
    return error;
}

/* ---- 문장 ---- */

static ErrorCode check_statement(const Statement *stmt, Table *const *tables, int table_count,
                                 Plan *plan, char out_message[MAX_MESSAGE])
{
    if (stmt->type == STMT_CREATE)
        return fail(out_message, ERR_UNSUPPORTED,
                    "CREATE TABLE is only allowed with the init command");
    /* CLI 경로에는 바인딩이 없다. API 는 바인딩으로 ? 를 값으로 바꾸고 param_count 를 0 으로 둔 뒤 부른다 */
    if (stmt->param_count > 0)
        return fail(out_message, ERR_PARAM,
                    "placeholders '?' are only allowed with bound parameters");
    if (stmt->type == STMT_INSERT) {
        ErrorCode error = find_table(tables, table_count, stmt->insert.table, plan, out_message);
        return error != ERR_NONE ? error : check_insert(&stmt->insert, plan, out_message);
    }
    ErrorCode error = find_table(tables, table_count, stmt->select.table, plan, out_message);
    return error != ERR_NONE ? error : check_select(&stmt->select, plan, out_message);
}

ErrorCode validator_check(const Statement *stmt, Table *const *tables, int table_count,
                          Plan *out_plan, char out_message[MAX_MESSAGE])
{
    memset(out_plan, 0, sizeof(*out_plan));
    out_message[0] = '\0';
    return check_statement(stmt, tables, table_count, out_plan, out_message);
}
