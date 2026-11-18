# Mini DBMS

> **Krafton Jungle WEEK11-12 · Team 11** — [mini-react](https://github.com/cc8s/jungle-team11-mini-react) · [mini-redis](https://github.com/cc8s/jungle-team11-mini-redis) · [mini-dbms](https://github.com/cc8s/jungle-team11-mini-dbms) · [보드](https://github.com/users/cc8s/projects/9)

C 로 직접 만든 **SQL 처리기 + B+ 트리 인덱스 + 스레드 풀 API 서버**.

| Phase | 결과물 |
|---|---|
| 1 | CLI 로 `INSERT` / `SELECT` 가 동작하는 파일 기반 SQL 처리기 |
| 2 | id 자동 부여 + 메모리 B+ 트리 인덱스, 100만 건 성능 비교 |
| 3 | Phase 1+2 를 엔진으로 쓰는 HTTP API 서버 + 스레드 풀 병렬 처리 |

## 실행 방법

> Linux 기준이다. Windows 에서는 VS Code → `Dev Containers: Reopen in Container` 로 연 뒤 빌드한다.

```bash
make                              # 빌드
make test                         # 단위·통합 테스트 (make test-asan: AddressSanitizer, make test-tsan: ThreadSanitizer)
make test-global                  # 같은 테스트를 LOCK_MODE=GLOBAL 빌드로 (기본은 TABLE_RW)
make check-banned                 # 전역 변수, 함수 안 static, strtok·strerror 같은 비재진입 함수 점검
make LOCK_MODE=GLOBAL             # 전역 뮤텍스로 빌드 (성능 비교용)
make bench                        # 인덱스 대 선형 탐색 벤치마크 (1만·10만·100만 건, 약 1 분)
mkdir -p data                     # 데이터 디렉터리 (git 제외)
./minidb init schema/users.sql    # 스키마로 data/users.tbl 생성
./minidb query.sql                # SQL 파일 실행
./minidb -d other_dir -q query.sql  # 다른 데이터 디렉터리, 에러와 요약만 출력
python3 tools/gen_sql.py 1000000 > data/bulk.sql   # 대량 INSERT SQL (이름 user1..N, 나이 시드 고정)
./minidb -q data/bulk.sql         # 100 만 건 삽입 (측정 결과: docs/benchmark.md)
./minidb serve 8080 8             # HTTP API 서버 (포트, 워커 수, [큐 용량 = 워커 × 16])
curl http://localhost:8080/health # {"ok":true}
curl -X POST http://localhost:8080/query -d '{"sql":"INSERT INTO users VALUES (?, ?)","params":["kim",25]}'
curl -X POST http://localhost:8080/query -d '{"sql":"SELECT * FROM users WHERE id = ?","params":[1]}'
```

종료 코드: 모든 문장 성공 `0`, SQL 에러가 하나라도 있음 `1`, 사용법·파일·디렉터리 오류 `2`.

```
$ ./minidb query.sql
Query OK, 1 row affected (0.000 sec)

+----+------+-----+
| id | name | age |
+----+------+-----+
|  1 | kim  |  25 |
+----+------+-----+
1 row in set (0.000 sec)

ERROR (line 5, statement 3): near 'SELEC': expected a statement   ← stderr
```

## 아키텍처

<!-- SQL → 토크나이저 → 파서 → 실행기 → (B+ 트리 | 선형 탐색) → 파일 저장소 / HTTP → 스레드 풀 → 엔진 -->

## 핵심 로직

### SQL 처리기

```
SQL 텍스트 → tokenizer → parser → validator → executor → table(.tbl)
                 토큰     Statement   Plan      Result
```

| 모듈 | 하는 일 |
|---|---|
| `tokenizer` | 호출할 때마다 토큰 하나. 키워드는 대소문자 무시, 식별자는 소문자로, `'it''s'` 이스케이프, `--` 주석, `?` 자리 표시자 |
| `parser` | 재귀 하강으로 `Statement` 를 만든다. 문법만 본다. 스크립트 모드는 에러 난 문장의 `;` 까지 건너뛰고 다음 문장을 계속 읽는다 |
| `validator` | 테이블·컬럼 존재, 값 개수, 타입(암묵 변환 없음), VARCHAR 바이트 길이를 스키마와 대조하고, 실행에 쓸 컬럼 오프셋을 `Plan` 에 미리 계산한다 |
| `executor` | INSERT 는 `next_id` 로 레코드를 만들어 덧붙이고, SELECT 는 256 건씩 묶어 읽으며 조건과 LIMIT 을 적용한다 |
| `engine` | 데이터 디렉터리의 `*.tbl` 을 열어 두고 위 단계를 잇는다. 출력하지 않고 `Result` 를 돌려준다 |
| `format`, `cli` | `Result` 를 MySQL 스타일 표로 출력, 명령줄 옵션과 종료 코드 |

지원 문법

```sql
CREATE TABLE t (id INT, name VARCHAR(n), ...)   -- init 명령에서만. 첫 컬럼은 id INT
INSERT INTO t VALUES (v, ...)                   -- id 를 뺀 값만, id 는 1 부터 자동 부여
SELECT * | col, ... FROM t [WHERE col op value] [LIMIT n]   -- op: = != < > <= >=
```

- 에러 문장은 `ERROR (line N, statement M): ...` 로 알리고 다음 문장을 계속 실행한다. 실패한 INSERT 는 id 를 소모하지 않는다.
- `AND`/`OR`, `UPDATE`, `DELETE`, `JOIN`, `ORDER BY`, 여러 행 INSERT, 컬럼 목록 INSERT 는 `ERR_UNSUPPORTED` 로 거절한다.
- 문자열 비교는 대소문자를 구분하는 바이트 비교 (`'ab' < 'abc'`).
- 상태는 모두 `Engine`/`Table`/`Parser` 안에 있다 (전역 변수, `static` 버퍼, `strtok` 없음). Phase 3 에서 락은 `executor` 호출만 감싼다.

### 저장 포맷

스키마 원본은 `schema/<table>.sql` (CREATE TABLE 한 문장), 데이터는 `data/<table>.tbl` 하나에 헤더와 레코드가 함께 있다.

```
offset 0     헤더 1024 바이트
               magic "MDB1" | version 1 | next_id | row_count | record_size | col_count
               columns[16] × { name 32B, type 4B, size 4B }  (664 바이트까지, 나머지 0)
offset 1024  레코드 영역: id 번째 레코드 = 1024 + (id - 1) × record_size
```

- 정수는 모두 리틀 엔디언, 구조체를 통째로 쓰지 않고 필드별로 직렬화한다.
- `INT` 는 4 바이트, `VARCHAR(n)` 은 n 바이트 `\0` 패딩 (1 ≤ n ≤ 1024). 길이를 넘는 값은 자르지 않고 `ERR_VALUE_TOO_LONG`.
- 파일 I/O 는 `pread`/`pwrite` 만 쓰고, fd 는 엔진을 닫을 때까지 유지한다.
- INSERT 는 레코드 → 헤더 순서로 쓴다. 중간에 죽어도 파일 끝에 `row_count` 밖의 레코드가 남을 뿐이라 다음 열기에서 무시된다.
- 열 때 헤더보다 작은 파일, magic·version·컬럼 정의·record_size 이상, 레코드 부족은 `ERR_CORRUPT` 로 거절한다.

### B+ 트리 인덱스

테이블마다 메모리 B+ 트리 하나가 `id → 레코드 offset` 을 가진다 (`src/btree.c`, 차수 `BTREE_ORDER` 기본 128).

- **노드**: 내부 노드는 길 안내용 키와 자식 포인터, 리프는 `(id, offset)` 쌍과 오른쪽 형제를 가리키는 `next`. 노드 안 탐색은 이진 탐색.
- **split**: 리프가 넘치면 반으로 나누고 오른쪽 첫 키를 부모로 **복사** (데이터는 리프에 남음). 내부 노드가 넘치면 가운데 키를 부모로 **올림**. 루트가 나뉘면 새 루트가 생겨 높이가 1 늘어난다 (모든 리프 깊이가 늘 같음). 연쇄 split 에 필요한 노드는 삽입 전에 미리 할당해서, 메모리가 모자라면 트리가 바뀌지 않는다.
- **범위 검색**: `btree_seek(lo)` 로 시작 리프를 찾고 리프 연결을 따라간다 (`id >= a`, `id < b`).
- **재구축**: `engine_open` 이 파일 레코드를 1MB 묶음으로 읽어 트리를 다시 만든다 (100 만 건 약 140 ms). 위치 k 의 id 가 k 가 아니면 `ERR_CORRUPT`, 헤더 갱신 전에 죽어서 남은 꼬리 레코드는 채택하고 헤더를 바로잡는다 (경고 출력).
- **INSERT**: 레코드 → 헤더 → 트리 순서. 파일 쓰기가 실패하면 트리에 넣지 않는다.
- **SELECT 접근 경로**: `WHERE id` 조건이고 `!=` 가 아니면 인덱스 (`Result.used_index = true`), 나머지는 선형 탐색. 인덱스 경로는 연속 offset 을 묶어 한 번의 `pread` 로 읽고, `LIMIT` 에서 일찍 멈춘다.

### 스레드 풀과 동시성

<!-- 작업 큐(mutex + condvar) -->

- **락 방식** (`make LOCK_MODE=...`, `/stats` 의 `lock_mode`): `TABLE_RW` (기본) 는 테이블마다 쓰기 우선 RW 락, `GLOBAL` 은 엔진 뮤텍스 하나. 방식에 따른 차이는 `engine.c` 의 `lock_table` / `unlock_table` 에만 있다.
- **락 범위**: 실행 단계 (`executor_run`) 만. SELECT 는 읽기 락 안에서 인덱스·스캔·레코드 복사, INSERT 는 쓰기 락 안에서 id 발급 → 레코드 `pwrite` → 헤더 `pwrite` → `btree_insert`. 파싱·검증·JSON 변환은 락 밖이고, 검증에 실패한 문장은 락을 잡지 않는다.
- **쓰기 우선**: `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP`. 기다리는 쓰기가 있으면 새 읽기를 막아 SELECT 가 끊이지 않아도 INSERT 가 굶지 않는다.
- **교착 없음**: 문장 하나는 테이블 하나만 쓰므로 한 번에 락 하나만 잡는다.

### Prepared Statement (`?` + `params`)

- `?` 는 값 자리 (INSERT 값, `WHERE` 오른쪽) 에만 쓸 수 있다. 테이블·컬럼 이름이나 `LIMIT` 자리는 `ERR_SYNTAX`.
- 엔진은 SQL 틀만 파싱하고, 검증 단계 (`validator_bind_params`) 에서 `?` 자리의 `Value` 를 params 값으로 바꾼다. 값은 토크나이저·파서를 거치지 않으므로 따옴표나 `;` 가 들어 있어도 데이터일 뿐이다 (이스케이프 불필요).
- 바꾼 값은 보통 값처럼 검사한다: 개수 불일치·`\0` 이 든 문자열은 `ERR_PARAM`, 타입이 다르면 `ERR_TYPE_MISMATCH`, 길이 초과는 `ERR_VALUE_TOO_LONG`.
- API 는 요청당 한 문장 (`;` 뒤에 문장이 또 있으면 `ERR_UNSUPPORTED`). CLI 스크립트에는 바인딩이 없어서 `?` 는 `ERR_PARAM`.

### SQL 인젝션 방어 시연

이름 검색 입력칸에 공격자가 `kim'; INSERT INTO users VALUES ('hacker', 99); --` 를 넣었다고 하자 (`tests/test_bind.c`).

| 보내는 방식 | 엔진이 보는 것 | 결과 |
|---|---|---|
| 바인딩: `WHERE name = ?` + `params: [그 문자열]` | 이름 하나 (값 자리의 `Value`) | 0 행, 삽입 없음 — 1 차 방어선 |
| 이어 붙이기로 만든 SQL 을 API 로 | `SELECT ... 'kim'` 과 `INSERT ... hacker` 두 문장 | `ERR_UNSUPPORTED`, 아무것도 실행 안 됨 — 2 차 방어선 (요청당 한 문장) |
| 이어 붙이기로 만든 SQL 을 CLI 스크립트로 | 두 문장 (스크립트는 여러 문장 허용) | hacker 행이 삽입된다 — 대조군 |

- 2 차 방어선은 문장 추가만 막는다. 이어 붙이기는 `name = 'x' OR ...` 처럼 한 문장 안에서 조건을 바꾸는 공격 (이 엔진은 `OR` 가 없지만) 을 막지 못하므로 값은 언제나 바인딩으로 보낸다.
- 입력 안전성: 64KB 식별자·문자열, 따옴표 1 만 개, 토크나이저·파서·엔진·JSON 에 시드 고정 무작위 입력 각 1 만 개 (`test_fuzz`, ASan 빌드에서 메모리 오류 0), 서버에 무작위 바이트 요청 1,000 개 (모두 4xx 또는 연결 종료, 이후에도 정상 응답).

## 테스트

`make test` 는 `tests/test_*.c` 를 각각 실행 파일로 만들어 돌린다. 테스트마다 `mkdtemp` 임시 디렉터리를 쓰고, 에러는 메시지 문구가 아니라 `ErrorCode` 로 확인한다.

| 구분 | 내용 |
|---|---|
| 단위 | `test_tokenizer` `test_parser` `test_table` `test_validator` `test_format` — 모듈별 정상·엣지 케이스 (식별자 31/32 자, INT 경계, VARCHAR n/n+1 바이트, 컬럼 16/17 개, 문장 64KB, 손상된 헤더 등) |
| 기능 | `test_engine` `test_cli` — 엔진 열기/INSERT/SELECT/스크립트, CLI 옵션·종료 코드·init |
| 통합 | `test_integration` — init → 100 건 INSERT → 조건별 SELECT, 재실행 후 데이터 유지, 문장 1,000 개 혼합 스크립트, 테이블 2 개 교차 |
| 바인딩 | `test_bind` — `?` 자리에 정수·문자열, 따옴표·`;` 가 든 값, 개수·타입·길이 불일치, 이름·`LIMIT` 자리의 `?`, 두 문장, 인젝션 시연 3 종 |
| 퍼징 | `test_fuzz` — 토크나이저·파서·엔진, JSON 에 무작위·변형 입력 각 1 만 개 (시드 고정). `test_server` 는 무작위 HTTP 요청 1,000 개 |
| 동시성 | `test_threadpool` `test_concurrency` — 큐·워커·종료, 읽기끼리 동시·쓰기 중 읽기 대기·쓰기 우선·에러 경로 락 해제·다른 테이블 동시 쓰기. 스레드 8 × INSERT 1 만 건 후 id 1..80,000 중복·빈 곳 없음, INSERT 2 + SELECT 6 혼합에서 쓰다 만 행 미노출·행 수 단조 증가, 서버 경유 클라이언트 16 × 요청 500. `make test-tsan` (데이터 레이스), `make test-global` (GLOBAL 락 빌드) |
| B+ 트리 | `test_btree` — split 경계, 범위 순회, 오름·내림·무작위 10 만 건. `make test` 가 `BTREE_ORDER` 3, 4 빌드로 한 번 더 돌림 (`make test-order`) |
| 메모리 | `make test-asan` — 전체 테스트를 AddressSanitizer 로 다시 돌려 누수·오버플로 확인 |
| 느린 테스트 | `make test-slow` — 100 만 건 트리를 일반·ASan 빌드에서 검사 (`MINIDB_SLOW_TESTS=1`) |

알려진 한계: 표의 열 너비는 UTF-8 글자 수로 맞추므로 한글처럼 화면 폭이 2 칸인 글자가 있으면 선이 어긋나 보인다.

## 성능 비교 (100만 건, 인덱스 사용 / 미사용)

`make bench` (컨테이너 내부 `/tmp`, 2 코어). 방법·환경·해석은 [docs/benchmark.md](docs/benchmark.md).

| 쿼리 | 경로 | 100 만 건 (웜) |
|---|---|---|
| `WHERE id = k` | B+ 트리 | 약 0.002 ms |
| `WHERE name = 'userk'` | 선형 탐색 | 약 34 ms (약 15,000 배) |
| `WHERE id >= 500000 LIMIT 100` | B+ 트리 | 약 0.004 ms |

| N | id = k (무작위 평균) | name = 'userk' | 트리 높이 |
|---|---|---|---|
| 1 만 | 0.0034 ms | 0.32 ms | 3 |
| 10 만 | 0.0063 ms | 3.36 ms | 3 |
| 100 만 | 0.0093 ms | 33.92 ms | 4 |

인덱스 검색은 N 이 100 배가 되어도 거의 일정하고 (O(log N)), 선형 탐색은 N 에 비례한다 (O(N)). 100 만 건 INSERT 는 약 3.3 s (초당 약 30 만 건).

## 실제 DBMS 와의 차이

## 팀

| 이름 | GitHub | 역할 |
|---|---|---|
| | [@cc8s](https://github.com/cc8s) | |
| | [@bloro-sh](https://github.com/bloro-sh) | |

## Contributing

- `main` 에 직접 push 하지 않는다. 브랜치 → PR → 리뷰 1명 승인 → merge
- 브랜치: `feat/<주제>`, `fix/<주제>`, `docs/<주제>`
- 커밋: `[Feat]` `[Fix]` `[Test]` `[Docs]` `[Refactor]` `[Chore]` + 제목, 빈 줄, 한 줄 설명
- 이슈 1개 ↔ PR 1개. PR 본문에 `Closes #이슈번호`
