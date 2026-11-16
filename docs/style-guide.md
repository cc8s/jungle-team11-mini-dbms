# C 스타일 가이드

> 클린 코드 원칙을 **C 언어와 이 프로젝트(미니 DBMS)** 에 맞게 옮긴 규칙이다.
> C 에는 클래스·생성자·예외가 없으므로, 각 원칙을 C 에서 같은 효과를 내는 방식으로 바꿔 적었다.
> 목표는 세 가지다: **읽으면 의도가 보이는 코드, 바꾸기 쉬운 코드, 테스트로 지켜지는 코드.**

| 클린 코드 개념 | 이 프로젝트에서의 대응 |
|---|---|
| 클래스 | 모듈 = `.h` (공개 인터페이스) + `.c` (구현) 한 쌍 |
| 객체 | 불투명 구조체 핸들 (`Engine *`, `Table *`, `BTree *`) |
| 생성자 / 정적 팩토리 메서드 | 의도가 드러나는 생성 함수 (`table_create` vs `table_open`) |
| 예외 | 일관된 에러 반환 규약 + 조기 반환 + `goto cleanup` |
| 인터페이스 / 추상체 | 모듈 경계의 함수 집합, 필요할 때 함수 포인터 |

---

## 1. 이름

### 1.1 의도를 드러내는 이름

이름만 보고 무엇을 하는지 예측할 수 있어야 한다. 의미 없는 약어와 애매한 이름을 쓰지 않는다.

```c
/* 나쁨 */
int  n;
char buf2[32];
int  proc(Tbl *t, int f);

/* 좋음 */
int  matched_row_count;
char column_name[MAX_NAME];
int  table_append_record(Table *table, const uint8_t *record, int32_t *out_id);
```

- 길어도 괜찮다. **길고 서술적인 이름이 짧고 어려운 이름보다 낫다.**
- 허용하는 약어는 아래 목록뿐이다. 그 외는 풀어 쓴다.

| 허용 약어 | 뜻 |
|---|---|
| `id` | 레코드 식별자 |
| `fd` | 파일 디스크립터 |
| `sql` | SQL 문자열 |
| `len` | 바이트 길이 |
| `max` / `min` | 최대 / 최소 |
| `ms` | 밀리초 |
| `i`, `j` | 짧은 반복문의 인덱스 (10 줄 이내) |

### 1.2 명명 규칙

| 대상 | 규칙 | 예 |
|---|---|---|
| 함수 | `모듈_동사_대상` (snake_case) | `table_read_record`, `btree_insert`, `parser_next_statement` |
| 변수, 필드 | snake_case 명사 | `record_size`, `next_id` |
| 타입 (struct, enum, typedef) | PascalCase | `Statement`, `CompareOp`, `ThreadPool` |
| enum 값, 매크로 상수 | 대문자 + 접두사 | `STMT_INSERT`, `OP_EQ`, `ERR_SYNTAX`, `MAX_COLUMNS` |
| bool 변수, bool 반환 함수 | `is_`, `has_`, `can_`, `should_` | `is_leaf`, `has_where`, `can_use_index()` |
| 파일 내부 함수 | `static`, 모듈 접두사 생략 가능 | `static bool is_identifier_start(char c)` |

- **모듈 접두사**는 C 에서 네임스페이스 역할을 한다. 공개 함수는 반드시 붙인다.
- 같은 모듈 안에서는 **같은 개념에 같은 단어**를 쓴다. 한 곳에서 `read` 라고 했으면 다른 곳에서 `fetch`, `load`, `get` 을 섞지 않는다.

### 1.3 프로젝트 공통 동사

| 동사 | 의미 | 예 |
|---|---|---|
| `create` / `destroy` | 새로 만듦 / 메모리 해제 | `btree_create`, `btree_destroy` |
| `open` / `close` | 이미 있는 자원(파일)에 연결 / 연결 해제 | `table_open`, `engine_close` |
| `init` | 호출한 쪽이 준비한 메모리를 초기화 | `result_init(&result)` |
| `free` | 구조체 내부가 가진 메모리 해제 (구조체 자체는 호출한 쪽 소유) | `statement_free`, `result_free` |
| `read` / `write` | 파일 I/O | `table_read_record` |
| `parse` | 텍스트 → 구조 | `parser_parse_select` |
| `find` / `search` | 없을 수 있는 조회 (bool 반환 + out 인자) | `btree_search`, `table_find_column` |
| `get` | 반드시 있는 값 조회 (실패 없음) | `result_get_int` |
| `is` / `has` | 상태 질문 (부작용 없음) | `table_is_empty` |
| `validate` | 규칙 검사 후 에러 보고 | `validator_validate_insert` |

### 1.4 적절한 추상화 수준의 이름

이름은 **그 함수가 놓인 계층의 언어**로 짓는다. 상위 계층의 이름에 하위 구현 세부사항을 드러내지 않는다.

```c
/* 나쁨: 실행기 계층인데 파일 구현이 이름에 드러남 */
pread_records_into_buffer(table, ...);

/* 좋음: 실행기는 "레코드를 읽는다" 만 안다. pread 는 table 모듈 안의 사정 */
table_read_records(table, first_id, count, out_buffer);
```

| 계층 | 쓰는 어휘 |
|---|---|
| engine / executor | 문장, 실행, 결과, 접근 경로, 인덱스 |
| table | 레코드, 헤더, 오프셋 |
| btree | 키, 값, 노드, 리프, 분할 |
| server / http | 요청, 응답, 연결, 상태 코드 |

### 1.5 조건의 의도를 함수로 드러낸다

조건식이 무엇을 묻는지 한 번에 읽히지 않으면 이름 있는 함수로 뺀다.

```c
/* 나쁨 */
if (stmt->select.has_where &&
    strcmp(stmt->select.where.column, "id") == 0 &&
    stmt->select.where.op != OP_NE) { ... }

/* 좋음 */
if (can_use_id_index(&stmt->select)) { ... }
```

- 부정의 부정을 피한다: `if (!is_not_found)` → `if (is_found)`.

### 1.6 생성에 의미 있는 이름을 쓴다 (정적 팩토리 메서드의 C 버전)

C 에는 생성자 오버로딩이 없으므로, **생성 방법마다 다른 이름의 함수**를 둔다. 이름이 "무엇으로, 어떻게 만드는지" 를 설명해야 한다.

```c
/* 나쁨: 어떤 값이 만들어지는지 인자만으로 알 수 없음 */
Value make_value(int type, void *data, size_t len);

/* 좋음 */
Value value_from_int(int32_t number);
Value value_from_string(const char *text, size_t len);
Value value_placeholder(int param_index);

Result result_ok_rows(...);
Result result_ok_insert(int32_t inserted_id);
Result result_error(ErrorCode code, int line, const char *format, ...);

ErrorCode table_create(const char *path, const CreateStmt *schema);  /* 새 파일 생성 */
Table    *table_open(const char *path, ErrorCode *out_error);         /* 기존 파일 열기 */
```

---

## 2. 함수

### 2.1 작게, 한 가지 일만

- 함수 하나는 **한 가지 일**을 하고, 그 일은 이름이 설명하는 일이다.
- 목표 길이 **40 줄 이하**. 넘으면 나눌 수 있는지 먼저 본다.
- 들여쓰기 깊이 **3 단계 이하**. 깊어지면 조기 반환이나 함수 추출로 줄인다.
- 함수 안의 문장들은 **같은 추상화 수준**에 있어야 한다.

```c
/* 같은 수준의 단계로 읽히는 함수 */
Result engine_execute(Engine *engine, const char *sql, const Value *params, int param_count)
{
    Statement statement;
    Result result = parse_single_statement(sql, &statement);
    if (result.error != ERR_NONE)
        return result;

    result = validate_and_bind(engine, &statement, params, param_count);
    if (result.error == ERR_NONE)
        result = execute_statement(engine, &statement);

    statement_free(&statement);
    return result;
}
```

### 2.2 인자

- 인자는 **3 개 이하**를 목표로 한다. 더 많으면 구조체로 묶는다.
- 출력 인자는 이름에 `out_` 을 붙이고 **마지막**에 둔다: `btree_search(tree, key, &out_offset)`.
- 바꾸지 않는 포인터 인자는 `const` 를 붙인다. `const` 가 없으면 "이 함수가 바꾼다" 는 신호다.
- bool 플래그 인자로 함수 동작을 둘로 나누지 않는다. 함수를 둘로 나눈다.

```c
/* 나쁨 */  table_scan(table, true);
/* 좋음 */  table_scan_all(table);   table_scan_live_only(table);
```

### 2.3 명령과 조회를 분리한다 (CQS)

함수는 **상태를 바꾸거나(명령)** **값을 알려주거나(조회)** 둘 중 하나만 한다.

```c
/* 나쁨: 조회처럼 보이는데 next_id 를 증가시킴 */
int32_t table_get_next_id(Table *table);

/* 좋음 */
int32_t table_peek_next_id(const Table *table);     /* 조회: 부작용 없음, const */
int32_t table_allocate_id(Table *table);            /* 명령: 이름이 상태 변경을 말함 */
```

- 조회 함수는 인자를 `const` 로 받는다. `const` 를 붙일 수 없다면 명령이다.
- 예외: 에러 결과를 돌려주는 명령 함수는 허용한다 (2.4 의 규약). "성공했는가" 는 상태 조회가 아니라 명령의 결과다.

### 2.4 에러 처리 (예외 대신 쓰는 규약)

C 에는 예외가 없다. 대신 **모든 모듈이 같은 규약**을 따라서 분기와 중첩이 퍼지지 않게 한다.

| 함수 종류 | 반환 | 예 |
|---|---|---|
| 실패할 수 있는 연산 | `ErrorCode` (성공 `ERR_NONE`), 결과는 `out_` 인자 | `ErrorCode table_read_record(Table *, int32_t id, uint8_t *out_record)` |
| 없을 수 있는 조회 | `bool` (찾음/못 찾음), 결과는 `out_` 인자 | `bool btree_search(const BTree *, int32_t key, uint64_t *out_offset)` |
| 생성 | 포인터 (실패 시 `NULL`) + 필요하면 `ErrorCode *out_error` | `Table *table_open(const char *path, ErrorCode *out_error)` |
| 엔진 최상위 | `Result` (에러 코드 + 메시지 포함) | `engine_execute` |

규칙:
- **에러를 무시하지 않는다.** 반환값을 쓰지 않을 거면 그 이유를 주석으로 남긴다.
- **조기 반환(guard clause)** 으로 정상 경로를 왼쪽에 평평하게 둔다.
- 자원이 여러 개면 **`goto cleanup` 하나**로 해제 경로를 모은다. 해제 코드를 분기마다 복사하지 않는다.
- 에러 메시지는 에러가 처음 발견된 곳에서 한 번만 만든다. 위로 올라가며 덧붙이지 않는다.
- `exit()`, `abort()` 는 `main.c` 와 테스트에서만 쓴다. 라이브러리 코드는 에러를 돌려준다.
- 절대 일어나면 안 되는 상황(프로그래머 실수)은 `assert` 로 검사한다. 사용자 입력 오류에 `assert` 를 쓰지 않는다.

```c
ErrorCode table_create(const char *path, const CreateStmt *schema)
{
    ErrorCode error = ERR_NONE;
    int fd = -1;
    uint8_t *header = NULL;

    if (schema->column_count > MAX_COLUMNS)
        return ERR_UNSUPPORTED;                          /* guard clause */

    fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd < 0) { error = ERR_IO; goto cleanup; }

    header = calloc(1, HEADER_SIZE);
    if (header == NULL) { error = ERR_INTERNAL; goto cleanup; }

    serialize_header(schema, header);
    if (pwrite(fd, header, HEADER_SIZE, 0) != HEADER_SIZE)
        error = ERR_IO;

cleanup:
    free(header);
    if (fd >= 0) close(fd);
    return error;
}
```

### 2.5 부작용을 숨기지 않는다

- 이름이 말하지 않는 일을 하지 않는다. `validate_*` 는 검사만 하고 파일을 쓰지 않는다.
- 전역 상태를 바꾸지 않는다 (전역 변수 금지, 3.4 참고).

---

## 3. 모듈과 구조 (변경하기 쉬운 "클래스")

### 3.1 단일 책임 원칙 (SRP)

모듈 하나는 **바뀌는 이유가 하나**여야 한다.

| 모듈 | 책임 | 바뀌는 이유 |
|---|---|---|
| `tokenizer` | 문자 → 토큰 | 어휘 규칙 변경 |
| `parser` | 토큰 → `Statement` | 문법 변경 |
| `validator` | 스키마 기준 의미 검사 | 검사 규칙 변경 |
| `executor` | `Statement` 실행 | 실행 전략(접근 경로) 변경 |
| `table` | `.tbl` 파일 포맷 | 저장 포맷 변경 |
| `btree` | 인덱스 자료구조 | 인덱스 구현 변경 |
| `format` | `Result` → CLI 표 | 출력 모양 변경 |
| `json`, `http` | 직렬화, 프로토콜 | API 형식 변경 |
| `threadpool` | 작업 분배 | 동시성 모델 변경 |

- 한 파일에 두 책임이 생기면 나눈다. 저장 포맷을 바꿀 때 `parser.c` 를 열 일이 있으면 설계가 잘못된 것이다.

### 3.2 의존 방향

```
main / server ─▶ engine ─▶ parser ─▶ tokenizer
                    ├────▶ validator ─▶ table
                    └────▶ executor  ─▶ table, btree
format, json ─▶ result
```

- 화살표 반대 방향 `#include` 금지. 하위 모듈은 상위 모듈을 모른다 (`table.c` 가 `engine.h` 를 include 하지 않는다).
- 순환 의존이 생기면 공통 부분을 더 아래 모듈(`common.h`, `statement.h`, `result.h`)로 내린다.

### 3.3 구현이 아니라 추상에 의존한다

- **불투명 타입**: 헤더에는 `typedef struct Table Table;` 만 두고, 구조체 내부는 `.c` 에서 정의한다. 다른 모듈은 필드를 직접 만지지 못하고 함수로만 쓴다. 내부를 바꿔도 다른 모듈은 다시 고칠 필요가 없다.
- 공개 헤더에는 **다른 모듈이 꼭 써야 하는 것만** 둔다. 나머지는 `.c` 의 `static`.
- 구현을 바꿔 끼울 필요가 생긴 지점(예: 락 방식, 접근 경로)은 함수 포인터나 컴파일 옵션 한 곳으로 모은다. 같은 `#ifdef` 를 여러 파일에 흩뿌리지 않는다.

```c
/* table.h — 공개 인터페이스만 */
typedef struct Table Table;
Table    *table_open(const char *path, ErrorCode *out_error);
void      table_close(Table *table);
int32_t   table_record_count(const Table *table);
ErrorCode table_read_record(Table *table, int32_t id, uint8_t *out_record);
```

### 3.4 스레드 안전성 (Phase 3 대비, 처음부터 지킨다)

- **전역 변수 금지.** 상태는 `Engine`, `Table`, `ThreadPool` 같은 핸들 안에 둔다.
- 함수 안 `static` 지역 변수(버퍼) 금지.
- `strtok`, `localtime`, `strerror` 같은 비재진입 함수 대신 `_r` 버전을 쓴다.
- 파일 접근은 `pread` / `pwrite` 만 쓴다.
- 락을 잡는 함수와 범위는 `engine.c` / `executor.c` 에만 둔다. 락을 잡은 채 소켓·콘솔 I/O 를 하지 않는다.

### 3.5 메모리 소유권

- 메모리를 할당한 모듈이 해제 함수를 제공한다 (`statement_free`, `result_free`, `btree_destroy`).
- 포인터를 반환하는 함수는 주석에 **누가 해제하는지** 적는다.
- 해제 후 포인터를 다시 쓰지 않는다. 해제 함수는 `NULL` 을 받아도 안전해야 한다.

### 3.6 크기

- 파일 하나 **500 줄 이하**를 목표로 한다. 넘으면 책임이 둘 이상인지 본다.
- 모듈과 함수 수는 **필요한 만큼만**. 쓰이지 않을 확장을 위해 미리 추상화하지 않는다 (설계 규칙 4).

---

## 4. 중복 제거와 의도 표현

- **같은 지식은 한 곳에만.** 레코드 오프셋 공식(`HEADER_SIZE + (id - 1) * record_size`) 은 `table.c` 의 함수 하나에만 있다.
- 매직 넘버를 쓰지 않는다. `1024`, `32`, `16` 은 `HEADER_SIZE`, `MAX_NAME`, `MAX_COLUMNS` 로.
- 에러 코드 → 이름 문자열, 에러 코드 → HTTP 상태 같은 매핑은 **표(배열) 하나**로 만든다. `switch` 를 여러 곳에 복사하지 않는다.
- 비슷한 코드가 세 번 나오면 함수로 뺀다.

### 주석

- 주석은 **왜**를 적는다. **무엇을**은 코드와 이름이 말해야 한다.
- 이름으로 설명할 수 있으면 주석 대신 이름을 고친다.
- 공개 함수에는 헤더에 한두 줄로: 무엇을 하는지, 실패 조건, 메모리 소유권.
- 과제 요구사항상 **핵심 로직(B+ 트리 split, 락 범위, 바인딩)** 에는 원리를 설명하는 주석을 단다.
- 주석 처리한 코드는 남기지 않는다. git 이 기억한다.

```c
/* 나쁨 */  i++;  /* i 를 1 증가 */

/* 좋음 */
/* 리프 split 은 오른쪽 첫 키를 부모에 "복사" 한다.
 * 리프에는 모든 키가 남아 있어야 리프 연결 리스트만으로 범위 검색이 가능하기 때문이다. */
```

---

## 5. 포맷

- 들여쓰기 4 칸 스페이스, 탭 금지. 한 줄 100 자 이하.
- 함수 여는 중괄호는 다음 줄, 제어문 여는 중괄호는 같은 줄.
- 한 줄짜리 `if` 본문도 가능하면 중괄호를 쓴다 (guard clause 의 `return` 한 줄은 예외 허용).
- 포인터 `*` 는 변수 쪽에 붙인다: `char *name`.
- 변수는 처음 쓰는 곳 가까이에서 선언하고 바로 초기화한다.
- 헤더 파일은 include guard (`#ifndef MODULE_H`) 를 쓰고, 자기 자신이 필요한 헤더를 직접 include 한다.
- `#include` 순서: 자기 헤더 → 프로젝트 헤더 → 표준/시스템 헤더.

```c
#include "table.h"

#include "common.h"
#include "error.h"

#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
```

---

## 6. 테스트

### 6.1 TDD 순서

1. **실패하는 테스트를 먼저** 작성한다. 테스트 없이 실제 코드를 쓰지 않는다.
2. 컴파일은 되고 실행은 실패하는 정도까지만 테스트를 쓴다.
3. 그 테스트를 통과할 만큼만 실제 코드를 쓴다.
4. 통과하면 중복을 제거하고 이름을 다듬는다 (리팩터링). 테스트는 계속 통과해야 한다.

### 6.2 테스트 코드도 실제 코드처럼

- 파일: `tests/test_<모듈>.c`. 모듈 하나당 테스트 파일 하나.
- 함수 이름은 **무엇을 검증하는지** 문장처럼: `test_insert_assigns_sequential_ids`, `test_varchar_over_limit_is_rejected`.
- **테스트 함수 하나에 개념 하나.** 개념당 `CHECK` 수는 최소로.
- 구조는 **준비 → 실행 → 검증** (Given / When / Then) 세 덩어리로 나눠 빈 줄로 구분한다.
- 에러는 **에러 코드**로 검사한다. 메시지 문구를 비교하지 않는다.
- 반복되는 준비 코드는 도우미 함수로 뺀다 (`make_temp_engine_with_users_table()`).

```c
static void test_varchar_over_limit_is_rejected(void)
{
    /* Given: name VARCHAR(4) 인 테이블 */
    TestDb db = test_db_create("CREATE TABLE t (id INT, name VARCHAR(4));");

    /* When: 5 바이트 문자열을 넣으면 */
    Result result = engine_execute(db.engine, "INSERT INTO t VALUES ('abcde');", NULL, 0);

    /* Then: 자르지 않고 거절한다 */
    CHECK(result.error == ERR_VALUE_TOO_LONG);

    result_free(&result);
    test_db_destroy(&db);
}
```

### 6.3 F.I.R.S.T

| 규칙 | 이 프로젝트에서 |
|---|---|
| **Fast** | 단위 테스트 전체가 수 초 안에 끝난다. 100 만 건 같은 느린 테스트는 `bench/` 나 별도 타깃으로 분리 |
| **Independent** | 테스트마다 `mkdtemp` 로 임시 디렉터리를 새로 만들고 끝나면 지운다. 다른 테스트의 결과에 의존하지 않는다 |
| **Repeatable** | 컨테이너 어디서나 같은 결과. 현재 시각, 고정 포트, 실제 `data/` 디렉터리에 의존하지 않는다 (서버 테스트는 포트 0) |
| **Self-Validating** | 통과/실패가 종료 코드로 결정된다. 사람이 출력을 눈으로 확인해야 하는 테스트는 테스트가 아니다 |
| **Timely** | 실제 코드 직전에 쓴다 (6.1) |

### 6.4 설계 품질 4 규칙

1. **모든 테스트를 실행한다** — 커밋 전 `make test` (동시성 관련 변경은 `make test-tsan` 도). 테스트하기 어려운 코드는 책임이 섞였다는 신호다.
2. **중복을 제거한다** — 4 절.
3. **의도를 표현한다** — 1 절의 이름 규칙, 2 절의 작은 함수, 6.2 의 읽히는 테스트.
4. **모듈과 함수 수를 최소로** — 위 세 규칙을 지키는 범위에서만 늘린다.

---

## 7. 리뷰 체크리스트

PR 을 올리기 전에 스스로 확인한다.

- [ ] 이름만 읽고 함수가 하는 일을 알 수 있는가
- [ ] 함수가 한 가지 일만 하고, 40 줄·들여쓰기 3 단계 안쪽인가
- [ ] 조회 함수에 부작용이 없고 인자가 `const` 인가
- [ ] 모든 에러 반환을 처리했고, 자원 해제가 `cleanup` 한 곳에 모였는가
- [ ] 하위 모듈이 상위 모듈을 include 하지 않는가
- [ ] 매직 넘버, 중복된 공식, 복사된 `switch` 가 없는가
- [ ] 전역 변수, `static` 버퍼, 비재진입 함수가 없는가
- [ ] 새 동작에 테스트가 있고, `make test` (필요하면 `test-asan`, `test-tsan`) 가 통과하는가
