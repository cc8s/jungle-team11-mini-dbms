#include "error.h"

#include <stddef.h>

#define ERROR_CODE_COUNT (ERR_INTERNAL + 1)

/* 코드 → 이름 표 하나로 관리한다. switch 와 달리 지정 초기화로 코드와 이름이 한 줄에 붙어 있어
 * 순서가 바뀌어도 어긋나지 않고, 빠진 항목은 NULL 로 남아 T01-03 테스트가 잡아낸다.
 * 읽기 전용 상수라 여러 스레드가 동시에 읽어도 안전하다. */
static const char *const ERROR_NAMES[] = {
    [ERR_NONE] = "NONE",
    [ERR_SYNTAX] = "SYNTAX",
    [ERR_UNKNOWN_TABLE] = "UNKNOWN_TABLE",
    [ERR_UNKNOWN_COLUMN] = "UNKNOWN_COLUMN",
    [ERR_TYPE_MISMATCH] = "TYPE_MISMATCH",
    [ERR_VALUE_COUNT] = "VALUE_COUNT",
    [ERR_VALUE_TOO_LONG] = "VALUE_TOO_LONG",
    [ERR_OUT_OF_RANGE] = "OUT_OF_RANGE",
    [ERR_AUTO_ID] = "AUTO_ID",
    [ERR_UNSUPPORTED] = "UNSUPPORTED",
    [ERR_PARAM] = "PARAM",
    [ERR_TOO_LARGE] = "TOO_LARGE",
    [ERR_IO] = "IO",
    [ERR_CORRUPT] = "CORRUPT",
    [ERR_INTERNAL] = "INTERNAL",
};

_Static_assert(sizeof(ERROR_NAMES) / sizeof(ERROR_NAMES[0]) == ERROR_CODE_COUNT,
               "ERROR_NAMES must have one entry per ErrorCode");

const char *error_name(ErrorCode code)
{
    /* enum 의 실제 정수 타입은 구현마다 달라 int 로 바꿔 음수까지 막는다 */
    int index = (int)code;
    if (index < 0 || index >= ERROR_CODE_COUNT)
        return "UNKNOWN";
    return ERROR_NAMES[index];
}
