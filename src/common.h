#ifndef COMMON_H
#define COMMON_H

/* 모든 모듈이 공유하는 한계값과 기본 타입. 값을 바꾸면 .tbl 파일 형식이 바뀌므로 함부로 고치지 않는다. */

#define MAX_NAME 32       /* 테이블·컬럼 이름 버퍼 크기 (\0 포함, 이름은 31 자까지) */
#define MAX_COLUMNS 16    /* 테이블 하나의 최대 컬럼 수 (id 포함) */
#define MAX_SQL_LEN 65536 /* 문장 하나의 최대 바이트 수, 넘으면 ERR_TOO_LARGE */
#define HEADER_SIZE 1024  /* .tbl 헤더 고정 크기, 레코드는 이 뒤부터 */
#define MAX_VARCHAR 1024  /* VARCHAR(n) 의 n 상한 */

typedef enum {
    COL_INT,     /* int32_t, 리틀 엔디언 4 바이트 */
    COL_VARCHAR, /* n 바이트, 남는 공간은 \0 */
} ColumnType;

#endif
