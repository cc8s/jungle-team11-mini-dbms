#ifndef CLI_H
#define CLI_H

/* 명령줄 도구. main 은 cli_run(argc, argv, stdout, stderr) 만 부르고,
 * 테스트는 출력 스트림을 바꿔 끼워 같은 함수를 직접 부른다.
 *   minidb [-d <data_dir>] [-q] <file.sql>     SQL 파일 실행
 *   minidb [-d <data_dir>] init <schema.sql>  CREATE TABLE 한 문장으로 테이블 생성
 *   minidb [-d <data_dir>] serve <port> <threads> [queue_cap]  HTTP API 서버 */

#include <stdio.h>

#define CLI_EXIT_OK 0        /* 모든 문장 성공 */
#define CLI_EXIT_SQL_ERROR 1 /* 실행은 했지만 SQL 에러가 하나 이상 */
#define CLI_EXIT_USAGE 2     /* 사용법 오류, 파일·디렉터리 열기 실패 */

int cli_run(int argc, char **argv, FILE *out, FILE *err);

#endif
