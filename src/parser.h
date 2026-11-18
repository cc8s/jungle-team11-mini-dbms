#ifndef MINI_DBMS_PARSER_H
#define MINI_DBMS_PARSER_H

/* 토큰 → Statement. 지원 문법:
 *
 *   INSERT INTO t [(c1, c2, ...)] VALUES (v1, v2, ...)
 *   SELECT * | c1, c2, ... FROM t [WHERE c = v | WHERE c BETWEEN a AND b]
 *
 * 문장 끝은 ';' 또는 입력 끝. 키워드는 대소문자를 가리지 않는다.
 * 테이블·컬럼이 실제로 있는지, 타입이 맞는지는 실행기가 스키마를 보고 확인한다. */

#include <stdbool.h>
#include <stddef.h>

#include "tokenizer.h"

#define SQL_MAX_COLUMNS 16
#define SQL_NAME_MAX    64

enum sql_value_type {
    SQL_VALUE_INT,
    SQL_VALUE_STRING
};

struct sql_value {
    enum sql_value_type type;
    long long int_value;
    char str_value[TOKEN_TEXT_MAX];
};

enum statement_type {
    STATEMENT_INSERT,
    STATEMENT_SELECT
};

enum where_type {
    WHERE_NONE,
    WHERE_EQUAL,  /* c = v */
    WHERE_BETWEEN /* c BETWEEN low AND high (양 끝 포함) */
};

struct where_clause {
    enum where_type type;
    char column[SQL_NAME_MAX];
    struct sql_value value; /* WHERE_EQUAL */
    long long low;          /* WHERE_BETWEEN */
    long long high;
};

struct statement {
    enum statement_type type;
    char table[SQL_NAME_MAX];

    /* INSERT: 생략하면 column_count == 0 (스키마 순서대로 id 를 뺀 컬럼에 넣는다).
     * SELECT: is_select_all 이면 비어 있다. */
    bool is_select_all;
    int column_count;
    char columns[SQL_MAX_COLUMNS][SQL_NAME_MAX];

    int value_count; /* INSERT 만 */
    struct sql_value values[SQL_MAX_COLUMNS];

    struct where_clause where; /* SELECT 만 */
};

enum parse_result {
    PARSE_OK,
    PARSE_EMPTY, /* 공백과 ';' 뿐이라 문장이 없음 */
    PARSE_ERROR
};

/* sql 의 첫 문장 하나를 해석한다. *consumed 에 다음 문장이 시작하는 위치를 돌려준다.
 * 여러 문장은 sql += consumed 로 이어 부른다. 실패하면 err 에 이유를 쓴다. */
enum parse_result parser_parse(const char *sql, size_t *consumed, struct statement *stmt, char *err,
                               size_t err_size);

#endif
