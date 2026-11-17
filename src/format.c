#include "format.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#define MS_PER_SECOND 1000.0
#define INT_TEXT_SIZE 12          /* "-2147483648" + \0 */
#define UTF8_CONTINUATION_MASK 0xC0
#define UTF8_CONTINUATION_BITS 0x80

size_t format_display_width(const char *text, size_t len)
{
    /* 이어지는 바이트(10xxxxxx)를 빼고 세면 글자 수가 된다 */
    size_t width = 0;
    for (size_t i = 0; i < len; i++) {
        if (((unsigned char)text[i] & UTF8_CONTINUATION_MASK) != UTF8_CONTINUATION_BITS)
            width++;
    }
    return width;
}

/* 칸 하나의 표시 문자열. INT 는 int_buffer 에 숫자로 쓰고, VARCHAR 는 레코드 안을 가리킨다 */
static const char *cell_text(const Result *result, int64_t row, int col,
                             char int_buffer[INT_TEXT_SIZE], size_t *out_len)
{
    if (result->columns[col].type == COL_INT) {
        int32_t value = result_get_int(result, row, col);
        int len = snprintf(int_buffer, INT_TEXT_SIZE, "%" PRId32, value);
        *out_len = (size_t)len;
        return int_buffer;
    }
    return result_get_str(result, row, col, out_len);
}

/* 열 너비 = 헤더와 모든 값 중 가장 긴 것 (글자 수) */
static void compute_widths(const Result *result, size_t widths[MAX_COLUMNS])
{
    char int_buffer[INT_TEXT_SIZE];
    for (int col = 0; col < result->column_count; col++) {
        const char *name = result->columns[col].name;
        widths[col] = format_display_width(name, strlen(name));
        for (int64_t row = 0; row < result->row_count; row++) {
            size_t len = 0;
            const char *text = cell_text(result, row, col, int_buffer, &len);
            size_t width = format_display_width(text, len);
            if (width > widths[col])
                widths[col] = width;
        }
    }
}

static void print_repeat(FILE *out, char c, size_t count)
{
    for (size_t i = 0; i < count; i++)
        fputc(c, out);
}

static void print_border(FILE *out, int column_count, const size_t widths[MAX_COLUMNS])
{
    for (int col = 0; col < column_count; col++) {
        fputc('+', out);
        print_repeat(out, '-', widths[col] + 2);
    }
    fputs("+\n", out);
}

/* "| text   " 처럼 칸 하나를 쓴다. 정수는 오른쪽, 문자열은 왼쪽 정렬 */
static void print_cell(FILE *out, const char *text, size_t len, size_t width, bool is_right)
{
    size_t pad = width - format_display_width(text, len);
    fputs("| ", out);
    if (is_right)
        print_repeat(out, ' ', pad);
    fwrite(text, 1, len, out); /* VARCHAR 는 \0 으로 끝나지 않을 수 있어 길이만큼만 쓴다 */
    if (!is_right)
        print_repeat(out, ' ', pad);
    fputc(' ', out);
}

static void print_header(FILE *out, const Result *result, const size_t widths[MAX_COLUMNS])
{
    for (int col = 0; col < result->column_count; col++) {
        const char *name = result->columns[col].name;
        print_cell(out, name, strlen(name), widths[col], false);
    }
    fputs("|\n", out);
}

static void print_row(FILE *out, const Result *result, int64_t row,
                      const size_t widths[MAX_COLUMNS])
{
    char int_buffer[INT_TEXT_SIZE];
    for (int col = 0; col < result->column_count; col++) {
        size_t len = 0;
        const char *text = cell_text(result, row, col, int_buffer, &len);
        print_cell(out, text, len, widths[col], result->columns[col].type == COL_INT);
    }
    fputs("|\n", out);
}

static double seconds(const Result *result)
{
    return result->elapsed_ms / MS_PER_SECOND;
}

static void print_select(FILE *out, const Result *result)
{
    if (result->row_count == 0) {
        fprintf(out, "Empty set (%.3f sec)\n\n", seconds(result));
        return;
    }
    size_t widths[MAX_COLUMNS];
    compute_widths(result, widths);
    print_border(out, result->column_count, widths);
    print_header(out, result, widths);
    print_border(out, result->column_count, widths);
    for (int64_t row = 0; row < result->row_count; row++)
        print_row(out, result, row, widths);
    print_border(out, result->column_count, widths);
    fprintf(out, "%" PRId64 " %s in set (%.3f sec)\n\n", result->row_count,
            result->row_count == 1 ? "row" : "rows", seconds(result));
}

void format_result(FILE *out, FILE *err, const Result *result, int stmt_index)
{
    if (result->error != ERR_NONE) {
        fprintf(err, "ERROR (line %d, statement %d): %s\n", result->line, stmt_index,
                result->message);
        return;
    }
    /* SELECT 결과는 항상 컬럼이 1 개 이상, INSERT 결과는 0 개 */
    if (result->column_count > 0) {
        print_select(out, result);
        return;
    }
    fprintf(out, "Query OK, %" PRId64 " %s affected (%.3f sec)\n\n", result->affected_rows,
            result->affected_rows == 1 ? "row" : "rows", seconds(result));
}
