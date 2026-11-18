#ifndef ENGINE_H
#define ENGINE_H

/* 데이터 디렉터리 하나 = 데이터베이스 하나. SQL 텍스트를 받아 파싱 → 검증 → 실행하고 Result 를 돌려준다.
 * 출력하지 않는다 (CLI, API 서버가 Result 를 각자 형식으로 낸다). */

#include "common.h"
#include "error.h"
#include "result.h"
#include "statement.h"
#include "table.h"

typedef struct Engine Engine;

/* data_dir 의 *.tbl 을 모두 연다. 테이블 이름은 파일 이름에서 .tbl 을 뗀 소문자.
 * .tbl 이 아닌 파일은 무시한다. 하나라도 열지 못하면 전체 실패: NULL 을 돌려주고
 * *out_error (ERR_IO, ERR_CORRUPT, ERR_INTERNAL) 와 out_message (문제 파일 이름 포함) 를 채운다. */
Engine *engine_open(const char *data_dir, ErrorCode *out_error, char out_message[MAX_MESSAGE]);
void engine_close(Engine *engine);

/* engine_open 이 테이블을 열며 남긴 경고 수와 index 번째 문장 (0 <= index < count).
 * 지금은 헤더 복구 ("table 'users' header recovered: ...") 뿐이다. 엔진은 출력하지 않으므로
 * CLI·서버가 꺼내서 stderr 등에 낸다. 문자열은 engine_close 까지 유효하다. */
int engine_warning_count(const Engine *engine);
const char *engine_warning(const Engine *engine, int index);

/* 이름이 name 인 테이블 (인덱스·재구축 시간 조회, 테스트·벤치마크용). 없으면 NULL */
const Table *engine_find_table(const Engine *engine, const char *name);

/* 열린 테이블 수와 index 번째 테이블 (0 <= index < count). 목록과 스키마 (이름, 컬럼) 는 열린 뒤
 * 바뀌지 않으므로 락 없이 읽어도 된다 */
int engine_table_count(const Engine *engine);
const Table *engine_table_at(const Engine *engine, int index);

/* index 번째 테이블의 현재 행 수. INSERT 와 동시에 불릴 수 있어서 그 테이블의 읽기 락
 * (GLOBAL 이면 엔진 뮤텍스) 을 잡고 읽는다 (/tables 용) */
int32_t engine_record_count(Engine *engine, int index);

/* 이 빌드의 락 방식 이름 ("GLOBAL" 또는 "TABLE_RW") */
const char *engine_lock_mode_name(void);

/* 여러 스레드가 같은 엔진으로 engine_execute 를 동시에 불러도 된다 (테이블을 읽고 쓰는 실행 단계는
 * 엔진 안의 락이 감싼다). engine_open / engine_close 는 서버 시작 전·종료 후 한 스레드에서만 부른다.
 *
 * sql 한 문장을 실행한다 (단일 문장 모드: 두 번째 문장은 ERR_UNSUPPORTED, 빈 입력은 ERR_SYNTAX).
 * 결과는 호출한 쪽이 result_free 한다.
 * params 는 sql 의 ? 자리에 순서대로 들어갈 값 param_count 개 (VAL_INT 또는 VAL_STRING, NULL·0 이면 없음).
 * ? 는 값 자리에만 쓸 수 있고 (이름·LIMIT 자리는 ERR_SYNTAX), 개수가 안 맞으면 ERR_PARAM.
 * 엔진은 값을 복사하므로 params 는 호출이 끝나면 바로 해제해도 된다. */
Result engine_execute(Engine *engine, const char *sql, const Value *params, int param_count);

/* 스크립트의 문장 하나가 끝날 때마다 불린다. stmt_index 는 1 부터 (에러 문장 포함, 빈 문장 제외).
 * result 는 콜백이 돌아온 뒤 엔진이 해제하므로, 남길 값은 콜백 안에서 복사한다. */
typedef void (*ResultHandler)(const Result *result, int stmt_index, void *ctx);

/* script 의 문장들을 순서대로 실행하고 문장마다 on_result 를 부른다 (NULL 이면 부르지 않음).
 * 에러가 난 문장도 번호와 줄을 담아 전달하고 다음 문장을 계속 실행한다. 에러 문장 수를 돌려준다.
 * 파서를 만들 메모리가 없으면 ERR_INTERNAL 결과 하나(번호 0)를 전달하고 1 을 돌려준다. */
int engine_execute_script(Engine *engine, const char *script, ResultHandler on_result, void *ctx);

#endif
