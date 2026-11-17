#ifndef ERROR_H
#define ERROR_H

/* 모든 모듈이 쓰는 에러 코드. 에러 판단과 테스트는 이 코드로 하고 메시지 문구로 하지 않는다.
 * 새 코드는 ERR_INTERNAL 앞에 추가한다 (ERR_INTERNAL 이 마지막이라는 가정을 error.c 가 검사한다). */
typedef enum {
    ERR_NONE,
    ERR_SYNTAX,
    ERR_UNKNOWN_TABLE,
    ERR_UNKNOWN_COLUMN,
    ERR_TYPE_MISMATCH,
    ERR_VALUE_COUNT,
    ERR_VALUE_TOO_LONG,
    ERR_OUT_OF_RANGE,
    ERR_AUTO_ID,
    ERR_UNSUPPORTED,
    ERR_PARAM,
    ERR_TOO_LARGE,
    ERR_IO,
    ERR_CORRUPT,
    ERR_INTERNAL,
} ErrorCode;

/* 에러 코드의 이름 ("SYNTAX" 등). 범위 밖 값이면 "UNKNOWN". 반환 문자열은 정적 상수라 해제하지 않는다. */
const char *error_name(ErrorCode code);

#endif
