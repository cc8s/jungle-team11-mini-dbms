#include "test.h"

#include "common.h"
#include "error.h"

#define OUT_OF_RANGE_CODE 999

/* T01-01 */ static void test_error_name_of_none(void)
{
    CHECK_STR_EQ(error_name(ERR_NONE), "NONE");
}

/* T01-02 */ static void test_error_name_of_unknown_column(void)
{
    CHECK_STR_EQ(error_name(ERR_UNKNOWN_COLUMN), "UNKNOWN_COLUMN");
}

/* T01-03 */ static void test_every_error_code_has_name(void)
{
    for (int code = ERR_NONE; code <= ERR_INTERNAL; code++) {
        const char *name = error_name((ErrorCode)code);
        CHECK(name != NULL);
        CHECK(strcmp(name, "UNKNOWN") != 0);
    }
}

/* E01-01 */ static void test_error_name_out_of_range_is_unknown(void)
{
    CHECK_STR_EQ(error_name((ErrorCode)OUT_OF_RANGE_CODE), "UNKNOWN");
    CHECK_STR_EQ(error_name((ErrorCode)-1), "UNKNOWN");
}

/* common.h 상수는 이후 Step 의 불변식(헤더 1024B 등)이 기대는 값이라 고정해 둔다 */
static void test_common_constants(void)
{
    CHECK(MAX_NAME == 32);
    CHECK(MAX_COLUMNS == 16);
    CHECK(MAX_SQL_LEN == 65536);
    CHECK(HEADER_SIZE == 1024);
    CHECK(MAX_VARCHAR == 1024);
    CHECK(COL_INT != COL_VARCHAR);
}

int main(void)
{
    RUN(test_error_name_of_none);
    RUN(test_error_name_of_unknown_column);
    RUN(test_every_error_code_has_name);
    RUN(test_error_name_out_of_range_is_unknown);
    RUN(test_common_constants);
    TEST_DONE();
    return 0;
}
