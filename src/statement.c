#include "statement.h"

#include <stdlib.h>
#include <string.h>

Value value_from_int(int32_t number)
{
    return (Value){ .type = VAL_INT, .i = number };
}

Value value_from_string(const char *text, size_t len)
{
    /* +1 로 \0 을 붙여 두면 디버깅·메시지 출력에서 C 문자열로 바로 쓸 수 있다.
     * len 이 0 이어도 1 바이트를 할당하므로 "빈 문자열" 과 "할당 실패(NULL)" 가 구분된다. */
    Value value = { .type = VAL_STRING, .s = { .ptr = malloc(len + 1), .len = len } };
    if (value.s.ptr == NULL) {
        value.s.len = 0;
        return value;
    }
    memcpy(value.s.ptr, text, len);
    value.s.ptr[len] = '\0';
    return value;
}

Value value_placeholder(int param_index)
{
    return (Value){ .type = VAL_PARAM, .param_index = param_index };
}

void value_free(Value *value)
{
    if (value == NULL || value->type != VAL_STRING)
        return;
    free(value->s.ptr);
    /* 해제 후 NULL 로 두어 두 번째 호출이 free(NULL) 이 되게 한다 */
    value->s.ptr = NULL;
    value->s.len = 0;
}

void statement_free(Statement *stmt)
{
    if (stmt == NULL)
        return;

    switch (stmt->type) {
    case STMT_INSERT:
        for (int i = 0; i < stmt->insert.value_count; i++)
            value_free(&stmt->insert.values[i]);
        break;
    case STMT_SELECT:
        if (stmt->select.has_where)
            value_free(&stmt->select.where.value);
        break;
    case STMT_CREATE:
        break; /* 고정 크기 배열뿐이라 해제할 것이 없다 */
    }
}
