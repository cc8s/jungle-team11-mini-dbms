#include "test.h"

#include <stdint.h>

#include "format.h"

/* users(id INT, name VARCHAR(8), age INT) 레코드 = 4 + 8 + 4 바이트 */
#define USERS_RECORD_SIZE 16
#define NAME_SIZE 8
#define NAME_OFFSET 4
#define AGE_OFFSET 12
#define WIDE_SIZE MAX_VARCHAR

/* 출력 캡처: open_memstream 이 text/len 의 주소를 기억해 두고 키워 가며 쓰므로
 * Capture 는 복사하지 말고 한 자리에 둔 채 포인터로 넘긴다 */
typedef struct {
    char *text;
    size_t len;
    FILE *stream;
} Capture;

static void capture_open(Capture *capture)
{
    capture->text = NULL;
    capture->len = 0;
    capture->stream = open_memstream(&capture->text, &capture->len);
    CHECK(capture->stream != NULL);
}

/* 스트림을 닫아 text 를 확정한다. 호출한 쪽이 free(text) */
static char *capture_close(Capture *capture)
{
    CHECK(fclose(capture->stream) == 0);
    return capture->text;
}

static void put_int(uint8_t *field, int32_t value)
{
    uint32_t bits = (uint32_t)value;
    for (int i = 0; i < 4; i++)
        field[i] = (uint8_t)(bits >> (8 * i));
}

static void set_column(Result *result, int i, const char *name, ColumnType type, int size,
                       int offset)
{
    snprintf(result->columns[i].name, MAX_NAME, "%s", name);
    result->columns[i].type = type;
    result->columns[i].size = size;
    result->columns[i].offset = offset;
}

/* SELECT * FROM users 결과를 직접 만든다 */
typedef struct {
    int32_t id;
    const char *name;
    int32_t age;
} User;

static Result users_result(const User *users, int count)
{
    Result result;
    result_init(&result);
    result.column_count = 3;
    set_column(&result, 0, "id", COL_INT, 4, 0);
    set_column(&result, 1, "name", COL_VARCHAR, NAME_SIZE, NAME_OFFSET);
    set_column(&result, 2, "age", COL_INT, 4, AGE_OFFSET);
    result.record_size = USERS_RECORD_SIZE;
    result.row_count = count;
    result.rows = calloc((size_t)(count > 0 ? count : 1), USERS_RECORD_SIZE);
    CHECK(result.rows != NULL);
    for (int i = 0; i < count; i++) {
        uint8_t *record = result.rows + i * USERS_RECORD_SIZE;
        CHECK(strlen(users[i].name) <= NAME_SIZE); /* 칸을 넘으면 옆 컬럼을 덮는다 */
        put_int(record, users[i].id);
        memcpy(record + NAME_OFFSET, users[i].name, strlen(users[i].name));
        put_int(record + AGE_OFFSET, users[i].age);
    }
    return result;
}

/* result 를 출력한 stdout, stderr 문자열을 기대값과 비교한다 */
static void check_output(const Result *result, int stmt_index, const char *expected_out,
                         const char *expected_err)
{
    Capture out;
    Capture err;
    capture_open(&out);
    capture_open(&err);
    format_result(out.stream, err.stream, result, stmt_index);
    char *out_text = capture_close(&out);
    char *err_text = capture_close(&err);
    if (strcmp(out_text, expected_out) != 0 || strcmp(err_text, expected_err) != 0)
        fprintf(stderr, "--- out ---\n%s--- err ---\n%s", out_text, err_text);
    CHECK_STR_EQ(out_text, expected_out);
    CHECK_STR_EQ(err_text, expected_err);
    free(out_text);
    free(err_text);
}

/* T17-01, T17-04 */ static void test_select_three_rows(void)
{
    const User users[] = { { 1, "kim", 25 }, { 2, "lee", 30 }, { 3, "park", 22 } };
    Result result = users_result(users, 3);
    check_output(&result, 1,
                 "+----+------+-----+\n"
                 "| id | name | age |\n"
                 "+----+------+-----+\n"
                 "|  1 | kim  |  25 |\n"
                 "|  2 | lee  |  30 |\n"
                 "|  3 | park |  22 |\n"
                 "+----+------+-----+\n"
                 "3 rows in set (0.000 sec)\n\n",
                 "");
    result_free(&result);
}

/* T17-04 */ static void test_select_one_row(void)
{
    const User users[] = { { 12, "lee", -7 } };
    Result result = users_result(users, 1);
    result.elapsed_ms = 1500;
    check_output(&result, 1,
                 "+----+------+-----+\n"
                 "| id | name | age |\n"
                 "+----+------+-----+\n"
                 "| 12 | lee  |  -7 |\n"
                 "+----+------+-----+\n"
                 "1 row in set (1.500 sec)\n\n",
                 "");
    result_free(&result);
}

/* T17-02 */ static void test_insert(void)
{
    Result result;
    result_init(&result);
    result.affected_rows = 1;
    result.last_insert_id = 1;
    check_output(&result, 1, "Query OK, 1 row affected (0.000 sec)\n\n", "");
}

/* T17-03 */ static void test_error(void)
{
    Result result = result_error(ERR_SYNTAX, 3, "near 'FORM': expected FROM");
    check_output(&result, 2, "", "ERROR (line 3, statement 2): near 'FORM': expected FROM\n");
}

/* E17-01 */ static void test_empty_set(void)
{
    Result result = users_result(NULL, 0);
    result.elapsed_ms = 2;
    check_output(&result, 1, "Empty set (0.002 sec)\n\n", "");
    result_free(&result);
}

/* E17-02, E17-03 */ static void test_column_width_follows_longest_value(void)
{
    const User users[] = { { 1, "", 100000 }, { 2, "abcdefgh", 5 } };
    Result result = users_result(users, 2);
    check_output(&result, 1,
                 "+----+----------+--------+\n"
                 "| id | name     | age    |\n"
                 "+----+----------+--------+\n"
                 "|  1 |          | 100000 |\n"
                 "|  2 | abcdefgh |      5 |\n"
                 "+----+----------+--------+\n"
                 "2 rows in set (0.000 sec)\n\n",
                 "");
    result_free(&result);
}

/* E17-04 */ static void test_korean_value(void)
{
    const User users[] = { { 1, "김철", 30 }, { 2, "가", 31 } }; /* 6, 3 바이트 = 2, 1 글자 */
    Result result = users_result(users, 2);
    check_output(&result, 1,
                 "+----+------+-----+\n"
                 "| id | name | age |\n"
                 "+----+------+-----+\n"
                 "|  1 | 김철   |  30 |\n"
                 "|  2 | 가    |  31 |\n"
                 "+----+------+-----+\n"
                 "2 rows in set (0.000 sec)\n\n",
                 "");
    result_free(&result);
}

/* E17-05 */ static void test_full_wide_varchar(void)
{
    Result result;
    result_init(&result);
    result.column_count = 1;
    set_column(&result, 0, "memo", COL_VARCHAR, WIDE_SIZE, 0);
    result.record_size = WIDE_SIZE;
    result.row_count = 1;
    result.rows = malloc(WIDE_SIZE);
    CHECK(result.rows != NULL);
    memset(result.rows, 'x', WIDE_SIZE); /* \0 없이 꽉 참 */

    Capture out;
    capture_open(&out);
    format_result(out.stream, stderr, &result, 1);
    char *text = capture_close(&out);
    char row[2 + WIDE_SIZE + 4]; /* "| " + 값 + " |\n" + \0 */
    row[0] = '|';
    row[1] = ' ';
    memset(row + 2, 'x', WIDE_SIZE);
    memcpy(row + 2 + WIDE_SIZE, " |\n", 4);
    CHECK(strstr(text, row) != NULL); /* 잘리지 않고 한 칸에 1024 자 */
    free(text);
    result_free(&result);
}

static void test_utf8_length(void)
{
    CHECK(format_display_width("", 0) == 0);
    CHECK(format_display_width("abc", 3) == 3);
    CHECK(format_display_width("김철수", strlen("김철수")) == 3);
    CHECK(format_display_width("a가b", strlen("a가b")) == 3);
}

int main(void)
{
    RUN(test_select_three_rows);
    RUN(test_select_one_row);
    RUN(test_insert);
    RUN(test_error);
    RUN(test_empty_set);
    RUN(test_column_width_follows_longest_value);
    RUN(test_korean_value);
    RUN(test_full_wide_varchar);
    RUN(test_utf8_length);
    TEST_DONE();
    return 0;
}
