#ifndef STATEMENT_H
#define STATEMENT_H

/* 파서가 만들고 검증기·실행기가 읽는 문장 구조. 문자열 값만 힙을 쓰고 나머지는 고정 크기 배열이다. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"

typedef enum { VAL_INT, VAL_STRING, VAL_PARAM } ValueType;

typedef struct {
    ValueType type;
    union {
        int32_t i;
        struct {
            char *ptr; /* 이스케이프 해제된 값의 malloc 복사본, 끝에 \0 (len 에는 미포함) */
            size_t len;
        } s;
        int param_index; /* ? 자리 번호, 0 부터 */
    };
} Value;

typedef enum { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE } CompareOp;

typedef struct {
    char column[MAX_NAME];
    CompareOp op;
    Value value;
} Condition;

typedef struct {
    char table[MAX_NAME];
    int value_count;
    Value values[MAX_COLUMNS];
} InsertStmt;

typedef struct {
    char table[MAX_NAME];
    bool select_all;
    int column_count;
    char columns[MAX_COLUMNS][MAX_NAME];
    bool has_where;
    Condition where;
    bool has_limit;
    int64_t limit;
} SelectStmt;

/* init 명령 전용 */
typedef struct {
    char name[MAX_NAME];
    ColumnType type;
    int size; /* 바이트 수: INT 는 4, VARCHAR(n) 은 n */
} ColumnDef;

typedef struct {
    char table[MAX_NAME];
    int column_count;
    ColumnDef columns[MAX_COLUMNS];
} CreateStmt;

typedef enum { STMT_INSERT, STMT_SELECT, STMT_CREATE } StmtType;

typedef struct {
    StmtType type;
    int line;
    int param_count;
    union {
        InsertStmt insert;
        SelectStmt select;
        CreateStmt create;
    };
} Statement;

Value value_from_int(int32_t number);
/* text 의 len 바이트를 복사한다. 메모리가 부족하면 s.ptr 이 NULL 이므로 호출한 쪽이 ERR_INTERNAL 로 처리한다. */
Value value_from_string(const char *text, size_t len);
Value value_placeholder(int param_index);

/* 문자열 값의 메모리를 해제하고 ptr 을 NULL 로 둔다. NULL 이나 이미 해제된 값에 불러도 안전하다. */
void value_free(Value *value);
/* 문장이 가진 모든 값을 해제한다. 구조체 자체는 호출한 쪽 소유. NULL 이나 두 번 호출해도 안전하다. */
void statement_free(Statement *stmt);

#endif
