#include "parser.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tokenizer.h"

#define INITIAL_TOKEN_CAPACITY 16
#define MAX_SHOWN_TOKEN_LEN 32            /* 메시지에 보여줄 토큰 원문 최대 길이 */
#define MAX_DETAIL_LEN (MAX_MESSAGE / 2)  /* "near '...': " 뒤에 붙는 설명 */
#define ID_COLUMN_NAME "id"                /* 모든 테이블의 첫 컬럼 (자동 부여) */
#define INT_COLUMN_SIZE ((int)sizeof(int32_t))

/* 파서는 문장 하나의 토큰을 ; (또는 입력 끝) 까지 먼저 배열로 모은 뒤 그 배열을 재귀 하강으로 읽는다.
 * 이렇게 하면 1) 미리 보기가 배열 인덱스로 단순해지고, 2) 문법 에러가 나도 이미 ; 까지 읽었으므로
 * 다음 호출이 자연스럽게 다음 문장부터 시작한다 (S09 에러 복구). */
struct Parser {
    Tokenizer tokenizer;
    Token *tokens; /* 현재 문장의 토큰. 마지막은 항상 TOK_SEMI 또는 TOK_EOF */
    size_t token_count;
    size_t token_capacity;
    size_t pos;
    int statement_number; /* 꺼낸 문장 수 (빈 문장 제외) */

    ErrorCode error;
    int error_line;
    char message[MAX_MESSAGE];
};

typedef struct {
    TokenType token;
    CompareOp op;
} OperatorEntry;

static const OperatorEntry OPERATORS[] = {
    { TOK_EQ, OP_EQ }, { TOK_NE, OP_NE }, { TOK_LT, OP_LT },
    { TOK_GT, OP_GT }, { TOK_LE, OP_LE }, { TOK_GE, OP_GE },
};

/* 토크나이저에는 없는 SQL 단어지만, 일반 문법 에러 대신 "지원하지 않음" 으로 알려줄 것들 */
static const char *const UNSUPPORTED_WORDS[] = {
    "update", "delete", "drop", "alter", "order", "group", "having", "join", "offset", "union",
};

Parser *parser_create(const char *sql, size_t len)
{
    Parser *parser = calloc(1, sizeof(*parser));
    if (parser == NULL)
        return NULL;
    parser->tokens = malloc(INITIAL_TOKEN_CAPACITY * sizeof(Token));
    if (parser->tokens == NULL) {
        free(parser);
        return NULL;
    }
    parser->token_capacity = INITIAL_TOKEN_CAPACITY;
    tokenizer_init(&parser->tokenizer, sql, len);
    return parser;
}

void parser_destroy(Parser *parser)
{
    if (parser == NULL)
        return;
    free(parser->tokens);
    free(parser);
}

int parser_error_line(const Parser *parser)
{
    return parser->error_line;
}

const char *parser_error_message(const Parser *parser)
{
    return parser->message;
}

/* ---- 토큰 모으기 ---- */

static ErrorCode record_tokenizer_error(Parser *parser, ErrorCode code, int line)
{
    parser->error = code;
    parser->error_line = line;
    snprintf(parser->message, sizeof(parser->message), "%s", parser->tokenizer.message);
    return code;
}

static bool append_token(Parser *parser, const Token *token)
{
    if (parser->token_count == parser->token_capacity) {
        size_t new_capacity = parser->token_capacity * 2;
        Token *grown = realloc(parser->tokens, new_capacity * sizeof(Token));
        if (grown == NULL)
            return false;
        parser->tokens = grown;
        parser->token_capacity = new_capacity;
    }
    parser->tokens[parser->token_count++] = *token;
    return true;
}

/* ; 또는 입력 끝까지 토큰을 모은다. 토크나이저 에러가 나도 끝까지 읽고 첫 에러만 돌려준다 */
static ErrorCode collect_statement_tokens(Parser *parser)
{
    ErrorCode first_error = ERR_NONE;
    parser->token_count = 0;
    parser->pos = 0;

    for (;;) {
        Token token;
        ErrorCode error = tokenizer_next(&parser->tokenizer, &token);
        if (error != ERR_NONE) {
            if (first_error == ERR_NONE)
                first_error = record_tokenizer_error(parser, error, token.line);
            continue;
        }
        if (!append_token(parser, &token))
            return record_tokenizer_error(parser, ERR_INTERNAL, token.line);
        if (token.type == TOK_SEMI || token.type == TOK_EOF)
            return first_error;
    }
}

/* ---- 토큰 배열 탐색 ---- */

static const Token *current(const Parser *parser)
{
    return &parser->tokens[parser->pos];
}

/* 현재 토큰을 돌려주고 한 칸 간다. 마지막(; 또는 EOF) 에서는 멈춰 있어 배열 밖으로 나가지 않는다 */
static const Token *advance(Parser *parser)
{
    const Token *token = current(parser);
    if (parser->pos + 1 < parser->token_count)
        parser->pos++;
    return token;
}

static bool is_keyword(const Token *token, Keyword keyword)
{
    return token->type == TOK_KEYWORD && token->keyword == keyword;
}

static bool match(Parser *parser, TokenType type)
{
    if (current(parser)->type != type)
        return false;
    advance(parser);
    return true;
}

static bool is_unsupported_word(const Token *token)
{
    if (token->type != TOK_IDENT)
        return false;
    for (size_t i = 0; i < sizeof(UNSUPPORTED_WORDS) / sizeof(UNSUPPORTED_WORDS[0]); i++) {
        if (strcmp(token->text, UNSUPPORTED_WORDS[i]) == 0)
            return true;
    }
    return false;
}

/* ---- 에러 ---- */

static ErrorCode fail_at(Parser *parser, ErrorCode code, const Token *token, const char *format,
                         ...) __attribute__((format(printf, 4, 5)));

/* 에러 위치의 토큰을 원문 대소문자 그대로 보여준다: near 'FORM': expected FROM */
static ErrorCode fail_at(Parser *parser, ErrorCode code, const Token *token, const char *format,
                         ...)
{
    char detail[MAX_DETAIL_LEN];
    va_list args;
    va_start(args, format);
    vsnprintf(detail, sizeof(detail), format, args);
    va_end(args);

    parser->error = code;
    parser->error_line = token->line;
    if (token->type == TOK_EOF) {
        snprintf(parser->message, sizeof(parser->message), "at end of input: %s", detail);
        return code;
    }
    int shown_len = token->len > MAX_SHOWN_TOKEN_LEN ? MAX_SHOWN_TOKEN_LEN : (int)token->len;
    snprintf(parser->message, sizeof(parser->message), "near '%.*s': %s", shown_len, token->start,
             detail);
    return code;
}

static ErrorCode expect_keyword(Parser *parser, Keyword keyword, const char *keyword_name)
{
    if (!is_keyword(current(parser), keyword))
        return fail_at(parser, ERR_SYNTAX, current(parser), "expected %s", keyword_name);
    advance(parser);
    return ERR_NONE;
}

/* 식별자 하나를 out_name 에 복사한다. 키워드는 식별자로 쓸 수 없다 (계획서 3.7) */
static ErrorCode expect_identifier(Parser *parser, const char *what, char *out_name)
{
    const Token *token = current(parser);
    if (token->type == TOK_KEYWORD)
        return fail_at(parser, ERR_SYNTAX, token, "keyword cannot be used as %s", what);
    if (token->type != TOK_IDENT)
        return fail_at(parser, ERR_SYNTAX, token, "expected %s", what);
    memcpy(out_name, token->text, MAX_NAME);
    advance(parser);
    return ERR_NONE;
}

/* 문장은 ; 또는 입력 끝에서 끝나야 한다. 남은 토큰이 아는 SQL 절이면 지원 안 함으로 알려준다 */
static ErrorCode expect_statement_end(Parser *parser)
{
    const Token *token = current(parser);
    if (token->type == TOK_SEMI || token->type == TOK_EOF)
        return ERR_NONE;
    if (is_unsupported_word(token))
        return fail_at(parser, ERR_UNSUPPORTED, token, "this clause is not supported");
    return fail_at(parser, ERR_SYNTAX, token, "unexpected token at end of statement");
}

/* ---- 값 ---- */

static bool is_literal(const Token *token)
{
    return token->type == TOK_INT || token->type == TOK_STRING || token->type == TOK_PARAM;
}

/* 문자열 토큰 → Value. 이스케이프를 풀 임시 버퍼를 거친 뒤 value_from_string 으로 복사본을 만든다 */
static ErrorCode parse_string_value(Parser *parser, const Token *token, Value *out_value)
{
    char *text = malloc(token->string_len + 1);
    if (text == NULL)
        return fail_at(parser, ERR_INTERNAL, token, "out of memory");
    tokenizer_copy_string(token, text);
    *out_value = value_from_string(text, token->string_len);
    free(text);
    if (out_value->s.ptr == NULL)
        return fail_at(parser, ERR_INTERNAL, token, "out of memory");
    return ERR_NONE;
}

/* INT | STRING | ?. ? 번호는 문장 안에서 0 부터 다시 센다 (바인딩은 문장 단위이므로) */
static ErrorCode parse_value(Parser *parser, Statement *stmt, Value *out_value)
{
    const Token *token = current(parser);
    ErrorCode error = ERR_NONE;
    if (token->type == TOK_INT)
        *out_value = value_from_int(token->int_value);
    else if (token->type == TOK_STRING)
        error = parse_string_value(parser, token, out_value);
    else if (token->type == TOK_PARAM)
        *out_value = value_placeholder(stmt->param_count++);
    else
        return fail_at(parser, ERR_SYNTAX, token, "expected a value");
    if (error == ERR_NONE)
        advance(parser);
    return error;
}

/* ---- SELECT ---- */

/* '*' 또는 ident (, ident)*. 같은 컬럼 반복은 허용한다 */
static ErrorCode parse_select_columns(Parser *parser, SelectStmt *select)
{
    if (match(parser, TOK_STAR)) {
        select->select_all = true;
        return ERR_NONE;
    }
    for (;;) {
        if (select->column_count == MAX_COLUMNS)
            return fail_at(parser, ERR_UNSUPPORTED, current(parser),
                           "at most %d columns can be selected", MAX_COLUMNS);
        ErrorCode error =
            expect_identifier(parser, "column name", select->columns[select->column_count]);
        if (error != ERR_NONE)
            return error;
        select->column_count++;
        if (!match(parser, TOK_COMMA))
            return ERR_NONE;
    }
}

static bool find_operator(TokenType type, CompareOp *out_op)
{
    for (size_t i = 0; i < sizeof(OPERATORS) / sizeof(OPERATORS[0]); i++) {
        if (OPERATORS[i].token == type) {
            *out_op = OPERATORS[i].op;
            return true;
        }
    }
    return false;
}

/* column op value 한 개. 컬럼은 왼쪽, 값은 오른쪽에만 온다 (계획서 3.7) */
static ErrorCode parse_condition(Parser *parser, Statement *stmt, Condition *condition)
{
    /* 왼쪽은 컬럼 이름 자리다. ? 는 값 자리에만 쓸 수 있으므로 문법 에러 (이름을 바인딩하면 쿼리 구조가
     * 바뀐다). 'kim' = name 같은 값은 문법은 맞지만 지원하지 않는 형태라 ERR_UNSUPPORTED */
    if (current(parser)->type == TOK_PARAM)
        return fail_at(parser, ERR_SYNTAX, current(parser),
                       "placeholder '?' is only allowed where a value goes, not a column name");
    if (is_literal(current(parser)))
        return fail_at(parser, ERR_UNSUPPORTED, current(parser),
                       "the column must be on the left side of the comparison");
    ErrorCode error = expect_identifier(parser, "column name", condition->column);
    if (error != ERR_NONE)
        return error;

    if (!find_operator(current(parser)->type, &condition->op))
        return fail_at(parser, ERR_SYNTAX, current(parser), "expected a comparison operator");
    advance(parser);

    if (current(parser)->type == TOK_IDENT)
        return fail_at(parser, ERR_UNSUPPORTED, current(parser),
                       "comparing two columns is not supported");
    error = parse_value(parser, stmt, &condition->value);
    if (error != ERR_NONE)
        return error;

    if (is_keyword(current(parser), KW_AND) || is_keyword(current(parser), KW_OR))
        return fail_at(parser, ERR_UNSUPPORTED, current(parser), "AND / OR are not supported");
    return ERR_NONE;
}

/* LIMIT 은 0 이상 정수만. -1 은 토크나이저가 INT(-1) 로 읽으므로 여기서 거절한다 */
static ErrorCode parse_limit(Parser *parser, SelectStmt *select)
{
    const Token *token = current(parser);
    if (token->type != TOK_INT || token->int_value < 0)
        return fail_at(parser, ERR_SYNTAX, token, "LIMIT needs a non-negative integer");
    select->has_limit = true;
    select->limit = token->int_value;
    advance(parser);
    return ERR_NONE;
}

/* [WHERE condition] [LIMIT n] — 이 순서로 각각 한 번만. 순서가 틀리거나 두 번 나오면
 * 남은 토큰이 되어 expect_statement_end 에서 ERR_SYNTAX 가 난다 */
static ErrorCode parse_select_clauses(Parser *parser, Statement *stmt)
{
    SelectStmt *select = &stmt->select;
    ErrorCode error = ERR_NONE;
    if (is_keyword(current(parser), KW_WHERE)) {
        advance(parser);
        /* 값을 만들기 전에 켜 두어, 실패해도 statement_free 가 이미 만든 문자열 값을 해제하게 한다 */
        select->has_where = true;
        error = parse_condition(parser, stmt, &select->where);
    }
    if (error == ERR_NONE && is_keyword(current(parser), KW_LIMIT)) {
        advance(parser);
        error = parse_limit(parser, select);
    }
    return error;
}

/* SELECT ( * | ident (, ident)* ) FROM ident [WHERE ...] [LIMIT ...] */
static ErrorCode parse_select(Parser *parser, Statement *stmt)
{
    SelectStmt *select = &stmt->select;
    advance(parser); /* SELECT */
    ErrorCode error = parse_select_columns(parser, select);
    if (error == ERR_NONE)
        error = expect_keyword(parser, KW_FROM, "FROM");
    if (error == ERR_NONE)
        error = expect_identifier(parser, "table name", select->table);
    if (error == ERR_NONE)
        error = parse_select_clauses(parser, stmt);
    if (error == ERR_NONE)
        error = expect_statement_end(parser);
    return error;
}

/* ---- INSERT ---- */

/* 값 자리의 식별자는 대개 문자열 따옴표를 빠뜨린 것이라 그렇게 알려준다 */
static ErrorCode parse_insert_value(Parser *parser, Statement *stmt, Value *out_value)
{
    const Token *token = current(parser);
    if (token->type == TOK_IDENT)
        return fail_at(parser, ERR_SYNTAX, token,
                       "expected a value (put strings in single quotes)");
    return parse_value(parser, stmt, out_value);
}

/* ( value (, value)* ). value_count 는 값이 다 만들어진 뒤에만 올려,
 * 실패해도 statement_free 가 만들어진 값까지만 해제하게 한다 */
static ErrorCode parse_insert_values(Parser *parser, Statement *stmt)
{
    InsertStmt *insert = &stmt->insert;
    if (!match(parser, TOK_LPAREN))
        return fail_at(parser, ERR_SYNTAX, current(parser), "expected '(' after VALUES");
    for (;;) {
        if (insert->value_count == MAX_COLUMNS)
            return fail_at(parser, ERR_UNSUPPORTED, current(parser),
                           "at most %d values can be inserted", MAX_COLUMNS);
        ErrorCode error = parse_insert_value(parser, stmt, &insert->values[insert->value_count]);
        if (error != ERR_NONE)
            return error;
        insert->value_count++;
        if (!match(parser, TOK_COMMA))
            break;
    }
    if (!match(parser, TOK_RPAREN))
        return fail_at(parser, ERR_SYNTAX, current(parser), "expected ')' or ','");
    return ERR_NONE;
}

/* INSERT INTO ident VALUES ( value (, value)* ). 컬럼 목록과 여러 행은 지원 안 함 */
static ErrorCode parse_insert(Parser *parser, Statement *stmt)
{
    InsertStmt *insert = &stmt->insert;
    advance(parser); /* INSERT */
    ErrorCode error = expect_keyword(parser, KW_INTO, "INTO");
    if (error == ERR_NONE)
        error = expect_identifier(parser, "table name", insert->table);
    if (error == ERR_NONE && current(parser)->type == TOK_LPAREN)
        error = fail_at(parser, ERR_UNSUPPORTED, current(parser),
                        "a column list in INSERT is not supported");
    if (error == ERR_NONE)
        error = expect_keyword(parser, KW_VALUES, "VALUES");
    if (error == ERR_NONE)
        error = parse_insert_values(parser, stmt);
    if (error == ERR_NONE && current(parser)->type == TOK_COMMA)
        error = fail_at(parser, ERR_UNSUPPORTED, current(parser),
                        "inserting multiple rows is not supported");
    if (error == ERR_NONE)
        error = expect_statement_end(parser);
    return error;
}

/* ---- CREATE TABLE ---- */

/* VARCHAR 뒤의 ( n ). 음수는 토크나이저가 INT(-1) 로 주므로 문법 오류로,
 * 0 과 MAX_VARCHAR 초과는 형식은 맞지만 범위를 벗어난 값이라 ERR_OUT_OF_RANGE 로 나눈다 */
static ErrorCode parse_varchar_size(Parser *parser, ColumnDef *column)
{
    if (!match(parser, TOK_LPAREN))
        return fail_at(parser, ERR_SYNTAX, current(parser),
                       "VARCHAR needs a size like VARCHAR(32)");
    const Token *token = current(parser);
    if (token->type != TOK_INT || token->int_value < 0)
        return fail_at(parser, ERR_SYNTAX, token, "VARCHAR size must be a positive integer");
    if (token->int_value == 0 || token->int_value > MAX_VARCHAR)
        return fail_at(parser, ERR_OUT_OF_RANGE, token, "VARCHAR size must be 1 to %d",
                       MAX_VARCHAR);
    column->size = token->int_value;
    advance(parser);
    if (!match(parser, TOK_RPAREN))
        return fail_at(parser, ERR_SYNTAX, current(parser), "expected ')' after VARCHAR size");
    return ERR_NONE;
}

/* INT | VARCHAR ( n ). FLOAT 같은 이름은 식별자로 읽히므로 지원 안 하는 타입으로 알려준다 */
static ErrorCode parse_column_type(Parser *parser, ColumnDef *column)
{
    const Token *token = current(parser);
    if (is_keyword(token, KW_INT)) {
        advance(parser);
        column->type = COL_INT;
        column->size = INT_COLUMN_SIZE;
        return ERR_NONE;
    }
    if (is_keyword(token, KW_VARCHAR)) {
        advance(parser);
        column->type = COL_VARCHAR;
        return parse_varchar_size(parser, column);
    }
    if (token->type == TOK_IDENT)
        return fail_at(parser, ERR_UNSUPPORTED, token, "only INT and VARCHAR(n) are supported");
    return fail_at(parser, ERR_SYNTAX, token, "expected a column type");
}

static bool has_column(const CreateStmt *create, const char *name)
{
    for (int i = 0; i < create->column_count; i++) {
        if (strcmp(create->columns[i].name, name) == 0)
            return true;
    }
    return false;
}

/* 이름은 토크나이저가 소문자로 정규화해 두었으므로 a 와 A 는 strcmp 로 같은 이름이 된다 */
static ErrorCode check_column_rules(Parser *parser, const CreateStmt *create,
                                    const ColumnDef *column, const Token *name_token)
{
    if (has_column(create, column->name))
        return fail_at(parser, ERR_SYNTAX, name_token, "duplicate column name");
    bool is_first = create->column_count == 0;
    bool is_id = strcmp(column->name, ID_COLUMN_NAME) == 0 && column->type == COL_INT;
    if (is_first && !is_id)
        return fail_at(parser, ERR_UNSUPPORTED, name_token,
                       "the first column must be '%s INT'", ID_COLUMN_NAME);
    return ERR_NONE;
}

/* ident type. 규칙 검사를 통과한 뒤에만 column_count 를 올린다 */
static ErrorCode parse_column_def(Parser *parser, CreateStmt *create)
{
    if (create->column_count == MAX_COLUMNS)
        return fail_at(parser, ERR_UNSUPPORTED, current(parser),
                       "a table can have at most %d columns", MAX_COLUMNS);
    const Token *name_token = current(parser);
    ColumnDef *column = &create->columns[create->column_count];
    ErrorCode error = expect_identifier(parser, "column name", column->name);
    if (error == ERR_NONE)
        error = parse_column_type(parser, column);
    if (error == ERR_NONE)
        error = check_column_rules(parser, create, column, name_token);
    if (error == ERR_NONE)
        create->column_count++;
    return error;
}

/* CREATE TABLE ident ( ident type (, ident type)* ).
 * 스키마 규칙도 여기서 본다: 만들 테이블은 아직 없으니 validator 가 볼 대상이 없다 */
static ErrorCode parse_create(Parser *parser, Statement *stmt)
{
    CreateStmt *create = &stmt->create;
    advance(parser); /* CREATE */
    ErrorCode error = expect_keyword(parser, KW_TABLE, "TABLE");
    if (error == ERR_NONE)
        error = expect_identifier(parser, "table name", create->table);
    if (error == ERR_NONE && !match(parser, TOK_LPAREN))
        error = fail_at(parser, ERR_SYNTAX, current(parser), "expected '(' and column definitions");
    while (error == ERR_NONE) {
        error = parse_column_def(parser, create);
        if (error != ERR_NONE || !match(parser, TOK_COMMA))
            break;
    }
    if (error == ERR_NONE && !match(parser, TOK_RPAREN))
        error = fail_at(parser, ERR_SYNTAX, current(parser), "expected ')' or ','");
    if (error == ERR_NONE)
        error = expect_statement_end(parser);
    return error;
}

/* ---- 문장 ---- */

static ErrorCode parse_tokens(Parser *parser, Statement *stmt)
{
    const Token *first = current(parser);
    stmt->line = first->line;
    if (is_keyword(first, KW_SELECT)) {
        stmt->type = STMT_SELECT;
        return parse_select(parser, stmt);
    }
    if (is_keyword(first, KW_INSERT)) {
        stmt->type = STMT_INSERT; /* 먼저 정해 두어야 실패 시 statement_free 가 값을 해제한다 */
        return parse_insert(parser, stmt);
    }
    if (is_keyword(first, KW_CREATE)) {
        stmt->type = STMT_CREATE;
        return parse_create(parser, stmt);
    }
    if (is_unsupported_word(first))
        return fail_at(parser, ERR_UNSUPPORTED, first, "this statement is not supported");
    return fail_at(parser, ERR_SYNTAX, first, "expected a statement");
}

static void clear_error(Parser *parser)
{
    parser->error = ERR_NONE;
    parser->error_line = 0;
    parser->message[0] = '\0';
}

/* 토큰이 ; 또는 EOF 하나뿐인 문장. 토크나이저 에러가 난 문장은 비어 보여도 에러 문장이다 */
static bool is_empty_statement(const Parser *parser, ErrorCode collect_error)
{
    return collect_error == ERR_NONE && parser->token_count == 1;
}

static bool is_end_of_input(const Parser *parser)
{
    return parser->tokens[parser->token_count - 1].type == TOK_EOF;
}

/* 빈 문장을 건너뛰며 다음 문장의 토큰을 모은다. 입력이 끝났으면 false */
static bool collect_next_statement(Parser *parser, ErrorCode *out_error)
{
    for (;;) {
        clear_error(parser);
        ErrorCode error = collect_statement_tokens(parser);
        if (!is_empty_statement(parser, error)) {
            *out_error = error;
            return true;
        }
        if (is_end_of_input(parser))
            return false;
    }
}

bool parser_next_statement(Parser *parser, Statement *out_stmt, ErrorCode *out_error)
{
    memset(out_stmt, 0, sizeof(*out_stmt));
    ErrorCode error = ERR_NONE;
    if (!collect_next_statement(parser, &error))
        return false;

    parser->statement_number++;
    if (error == ERR_NONE)
        error = parse_tokens(parser, out_stmt);
    if (error != ERR_NONE) {
        /* 일부만 채워진 문장을 여기서 정리해 두어 호출한 쪽은 성공·실패를 가리지 않고 해제하면 된다 */
        statement_free(out_stmt);
        memset(out_stmt, 0, sizeof(*out_stmt));
    }
    *out_error = error;
    return true;
}

/* 첫 문장을 파싱하고, 뒤에 남은 것이 빈 문장뿐인지 본다.
 * 무엇이든 더 있으면 (토크나이저 에러여도) 두 번째 문장으로 보고 거절한다 */
static ErrorCode parse_only_statement(Parser *parser, Statement *out_stmt)
{
    ErrorCode error = ERR_NONE;
    if (!parser_next_statement(parser, out_stmt, &error))
        return fail_at(parser, ERR_SYNTAX, current(parser), "expected a statement");
    if (error != ERR_NONE)
        return error;

    ErrorCode rest_error = ERR_NONE;
    if (!collect_next_statement(parser, &rest_error))
        return ERR_NONE;
    statement_free(out_stmt);
    memset(out_stmt, 0, sizeof(*out_stmt));
    return fail_at(parser, ERR_UNSUPPORTED, current(parser),
                   "only one statement is allowed here");
}

ErrorCode parser_parse_single(const char *sql, size_t len, Statement *out_stmt,
                              ParseError *out_error)
{
    memset(out_stmt, 0, sizeof(*out_stmt));
    memset(out_error, 0, sizeof(*out_error));
    Parser *parser = parser_create(sql, len);
    if (parser == NULL) {
        snprintf(out_error->message, sizeof(out_error->message), "out of memory");
        return ERR_INTERNAL;
    }
    /* Parser 를 해제하기 전에 에러 정보를 호출한 쪽 버퍼로 옮겨 둔다 */
    ErrorCode error = parse_only_statement(parser, out_stmt);
    if (error != ERR_NONE) {
        out_error->line = parser->error_line;
        snprintf(out_error->message, sizeof(out_error->message), "%s", parser->message);
    }
    parser_destroy(parser);
    return error;
}

int parser_statement_number(const Parser *parser)
{
    return parser->statement_number;
}
