#ifndef MINI_DBMS_CLI_H
#define MINI_DBMS_CLI_H

/* Phase 1 CLI.
 *
 *   ./minidb [-s schema_dir] [-d data_dir] query.sql   파일의 문장을 차례로 실행
 *   ./minidb [-s schema_dir] [-d data_dir]             터미널이면 REPL, 파이프면 stdin 전체 실행
 *
 * 한 문장이 실패해도 다음 문장을 계속 실행한다. 오류가 하나라도 있으면 종료 코드 1. */

#include <stdio.h>

#include "executor.h"

/* sql 의 문장을 모두 실행하고 결과·오류를 out 에 쓴다. 실패한 문장 수를 돌려준다. */
int cli_run_sql(struct executor *ex, const char *sql, FILE *out);

int cli_main(int argc, char **argv);

#endif
