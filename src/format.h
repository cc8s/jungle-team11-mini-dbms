#ifndef FORMAT_H
#define FORMAT_H

/* Result → CLI 출력 (MySQL 스타일). 엔진은 출력하지 않으므로 사람이 보는 형식은 여기에만 있다. */

#include <stddef.h>
#include <stdio.h>

#include "result.h"

/* 결과 하나를 출력한다. 성공은 out 에, 에러는 err 에 쓴다.
 *   SELECT  표 + "N rows in set (X sec)" (0 행이면 "Empty set (X sec)")
 *   INSERT  "Query OK, 1 row affected (X sec)"
 *   에러    "ERROR (line N, statement M): message"
 * 시간은 result->elapsed_ms 를 초 단위 소수 셋째 자리로 쓴다. */
void format_result(FILE *out, FILE *err, const Result *result, int stmt_index);

/* UTF-8 문자열의 글자 수 (열 정렬 기준). 전각 글자도 1 로 센다 (알려진 한계) */
size_t format_display_width(const char *text, size_t len);

#endif
