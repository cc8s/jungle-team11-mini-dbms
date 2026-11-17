#include "test.h"

#include "result.h"
#include "statement.h"

#define LONG_MESSAGE_LEN 300
#define USERS_RECORD_SIZE 8 /* id INT(4) + name VARCHAR(4) */
#define USERS_NAME_SIZE 4

/* id INT, name VARCHAR(4) 두 컬럼짜리 Result 를 rows 바이트와 함께 만든다. rows 소유권은 Result 로 넘어간다. */
static Result make_users_result(uint8_t *rows, int64_t row_count)
{
    Result result;
    result_init(&result);
    result.column_count = 2;
    result.columns[0] = (ResultColumn){ .name = "id", .type = COL_INT, .size = 4, .offset = 0 };
    result.columns[1] = (ResultColumn){
        .name = "name", .type = COL_VARCHAR, .size = USERS_NAME_SIZE, .offset = 4 };
    result.record_size = USERS_RECORD_SIZE;
    result.row_count = row_count;
    result.rows = rows;
    return result;
}

/* T02-01 */ static void test_value_from_string_copies_text(void)
{
    const char source[] = "kim";
    Value value = value_from_string(source, 3);

    CHECK(value.type == VAL_STRING);
    CHECK(value.s.ptr != NULL);
    CHECK(value.s.ptr != source);
    CHECK(value.s.len == 3);
    CHECK(memcmp(value.s.ptr, "kim", 3) == 0);

    value_free(&value);
    CHECK(value.s.ptr == NULL);
}

static void test_value_from_int_and_placeholder(void)
{
    Value number = value_from_int(-42);
    CHECK(number.type == VAL_INT);
    CHECK(number.i == -42);

    Value placeholder = value_placeholder(2);
    CHECK(placeholder.type == VAL_PARAM);
    CHECK(placeholder.param_index == 2);

    value_free(&number);      /* 해제할 것이 없어도 안전해야 한다 */
    value_free(&placeholder);
}

/* T02-02 */ static void test_result_error_fills_fields(void)
{
    Result result = result_error(ERR_SYNTAX, 5, "near '%s'", "FORM");

    CHECK(result.error == ERR_SYNTAX);
    CHECK(result.line == 5);
    CHECK_STR_EQ(result.message, "near 'FORM'");
    CHECK(result.rows == NULL);
    result_free(&result);
}

/* T02-03 */ static void test_result_get_reads_cells(void)
{
    uint8_t *rows = calloc(2, USERS_RECORD_SIZE);
    CHECK(rows != NULL);
    /* 1 행: id = 1, name = "ab" (패딩) / 2 행: id = 0x01020304 (리틀 엔디언), name = "kim" */
    rows[0] = 0x01;
    memcpy(rows + 4, "ab", 2);
    const uint8_t second_id[] = { 0x04, 0x03, 0x02, 0x01 };
    memcpy(rows + USERS_RECORD_SIZE, second_id, sizeof(second_id));
    memcpy(rows + USERS_RECORD_SIZE + 4, "kim", 3);
    Result result = make_users_result(rows, 2);

    size_t len = 0;
    CHECK(result_get_int(&result, 0, 0) == 1);
    CHECK(memcmp(result_get_str(&result, 0, 1, &len), "ab", 2) == 0);
    CHECK(len == 2);
    CHECK(result_get_int(&result, 1, 0) == 0x01020304);
    CHECK(memcmp(result_get_str(&result, 1, 1, &len), "kim", 3) == 0);
    CHECK(len == 3);
    result_free(&result);
}

static void test_result_get_int_reads_negative(void)
{
    uint8_t *rows = calloc(1, USERS_RECORD_SIZE);
    CHECK(rows != NULL);
    memset(rows, 0xFF, 4); /* -1 */
    Result result = make_users_result(rows, 1);

    CHECK(result_get_int(&result, 0, 0) == -1);
    result_free(&result);
}

/* E02-01 */ static void test_free_null_is_noop(void)
{
    statement_free(NULL);
    result_free(NULL);
    value_free(NULL);
    CHECK(1);
}

/* E02-02 */ static void test_statement_free_twice_is_safe(void)
{
    Statement insert = { .type = STMT_INSERT, .line = 1 };
    insert.insert.value_count = 2;
    insert.insert.values[0] = value_from_string("kim", 3);
    insert.insert.values[1] = value_from_int(25);

    statement_free(&insert);
    CHECK(insert.insert.values[0].s.ptr == NULL);
    statement_free(&insert); /* 두 번째 호출: ASan 이 이중 해제를 잡는다 */

    Statement select = { .type = STMT_SELECT, .line = 2 };
    select.select.has_where = true;
    select.select.where.value = value_from_string("lee", 3);

    statement_free(&select);
    CHECK(select.select.where.value.s.ptr == NULL);
    statement_free(&select);
}

/* E02-03 */ static void test_value_from_empty_string(void)
{
    Value value = value_from_string("", 0);

    CHECK(value.type == VAL_STRING);
    CHECK(value.s.ptr != NULL);
    CHECK(value.s.len == 0);
    value_free(&value);
}

/* E02-04 */ static void test_result_error_truncates_long_message(void)
{
    char long_text[LONG_MESSAGE_LEN + 1];
    memset(long_text, 'x', LONG_MESSAGE_LEN);
    long_text[LONG_MESSAGE_LEN] = '\0';

    Result result = result_error(ERR_SYNTAX, 7, "%s", long_text);

    CHECK(strlen(result.message) == MAX_MESSAGE - 1);
    CHECK(result.line == 7); /* 메시지 뒤 필드가 덮어써지지 않았다 */
    result_free(&result);
}

/* E02-05 */ static void test_result_get_str_full_varchar_without_nul(void)
{
    /* 레코드 크기만큼만 할당해 칸 뒤를 1 바이트라도 읽으면 ASan 이 잡게 한다 */
    uint8_t *rows = malloc(USERS_RECORD_SIZE);
    CHECK(rows != NULL);
    memset(rows, 0, 4);
    memcpy(rows + 4, "abcd", USERS_NAME_SIZE);
    Result result = make_users_result(rows, 1);

    size_t len = 0;
    const char *name = result_get_str(&result, 0, 1, &len);
    CHECK(len == USERS_NAME_SIZE);
    CHECK(memcmp(name, "abcd", USERS_NAME_SIZE) == 0);
    result_free(&result);
    CHECK(result.rows == NULL);
}

int main(void)
{
    RUN(test_value_from_string_copies_text);
    RUN(test_value_from_int_and_placeholder);
    RUN(test_result_error_fills_fields);
    RUN(test_result_get_reads_cells);
    RUN(test_result_get_int_reads_negative);
    RUN(test_free_null_is_noop);
    RUN(test_statement_free_twice_is_safe);
    RUN(test_value_from_empty_string);
    RUN(test_result_error_truncates_long_message);
    RUN(test_result_get_str_full_varchar_without_nul);
    TEST_DONE();
    return 0;
}
