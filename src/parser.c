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

/* 파서는 문장 하나의 토큰을 ; (또는 입력 끝) 까지 먼저 배열로 모은 뒤 그 배열을 재귀 하강으로 읽는다.
 * 이렇게 하면 1) 미리 보기가 배열 인덱스로 단순해지고, 2) 문법 에러가 나도 이미 ; 까지 읽었으므로
 * 다음 호출이 자연스럽게 다음 문장부터 시작한다 (S09 에러 복구). */
struct Parser {
    Tokenizer tokenizer;
    Token *tokens; /* 현재 문장의 토큰. 마지막은 항상 TOK_SEMI 또는 TOK_EOF */
    size_t token_count;
    size_t token_capacity;
    size_t pos;

    ErrorCode error;
    int error_line;
    char message[MAX_MESSAGE];
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

/* SELECT ( * | ident (, ident)* ) FROM ident */
static ErrorCode parse_select(Parser *parser, SelectStmt *select)
{
    advance(parser); /* SELECT */
    ErrorCode error = parse_select_columns(parser, select);
    if (error == ERR_NONE)
        error = expect_keyword(parser, KW_FROM, "FROM");
    if (error == ERR_NONE)
        error = expect_identifier(parser, "table name", select->table);
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
        return parse_select(parser, &stmt->select);
    }
    if (is_unsupported_word(first))
        return fail_at(parser, ERR_UNSUPPORTED, first, "this statement is not supported");
    return fail_at(parser, ERR_SYNTAX, first, "expected a statement");
}

ErrorCode parser_parse_statement(Parser *parser, Statement *out_stmt)
{
    memset(out_stmt, 0, sizeof(*out_stmt));
    parser->error = ERR_NONE;
    parser->error_line = 0;
    parser->message[0] = '\0';

    ErrorCode error = collect_statement_tokens(parser);
    if (error == ERR_NONE)
        error = parse_tokens(parser, out_stmt);
    if (error != ERR_NONE) {
        /* 일부만 채워진 문장을 여기서 정리해 두어 호출한 쪽은 성공·실패를 가리지 않고 해제하면 된다 */
        statement_free(out_stmt);
        memset(out_stmt, 0, sizeof(*out_stmt));
    }
    return error;
}
