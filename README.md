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
make test                         # 단위·통합 테스트 (make test-asan: AddressSanitizer)
mkdir -p data                     # 데이터 디렉터리 (git 제외)
./minidb init schema/users.sql    # 스키마로 data/users.tbl 생성
./minidb query.sql                # SQL 파일 실행
./minidb -d other_dir -q query.sql  # 다른 데이터 디렉터리, 에러와 요약만 출력
python3 tools/gen_sql.py 1000000 > data/bulk.sql   # 대량 INSERT SQL (이름 user1..N, 나이 시드 고정)
./minidb -q data/bulk.sql         # 100 만 건 삽입 (측정 결과: docs/benchmark.md)
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

<!-- 노드 구조, 분할, id → 오프셋, 리프 연결 리스트로 BETWEEN 범위 검색 -->

### 스레드 풀과 동시성

<!-- 작업 큐(mutex + condvar), 테이블별 rwlock, id 증가는 write lock 안에서 -->

## 테스트

`make test` 는 `tests/test_*.c` 를 각각 실행 파일로 만들어 돌린다. 테스트마다 `mkdtemp` 임시 디렉터리를 쓰고, 에러는 메시지 문구가 아니라 `ErrorCode` 로 확인한다.

| 구분 | 내용 |
|---|---|
| 단위 | `test_tokenizer` `test_parser` `test_table` `test_validator` `test_format` — 모듈별 정상·엣지 케이스 (식별자 31/32 자, INT 경계, VARCHAR n/n+1 바이트, 컬럼 16/17 개, 문장 64KB, 손상된 헤더 등) |
| 기능 | `test_engine` `test_cli` — 엔진 열기/INSERT/SELECT/스크립트, CLI 옵션·종료 코드·init |
| 통합 | `test_integration` — init → 100 건 INSERT → 조건별 SELECT, 재실행 후 데이터 유지, 문장 1,000 개 혼합 스크립트, 테이블 2 개 교차 |
| 동시성 | Phase 3 |
| B+ 트리 | `test_btree` — split 경계, 범위 순회, 오름·내림·무작위 10 만 건. `make test` 가 `BTREE_ORDER` 3, 4 빌드로 한 번 더 돌림 (`make test-order`) |
| 메모리 | `make test-asan` — 전체 테스트를 AddressSanitizer 로 다시 돌려 누수·오버플로 확인 |
| 느린 테스트 | `make test-slow` — 100 만 건 트리를 일반·ASan 빌드에서 검사 (`MINIDB_SLOW_TESTS=1`) |

알려진 한계: 표의 열 너비는 UTF-8 글자 수로 맞추므로 한글처럼 화면 폭이 2 칸인 글자가 있으면 선이 어긋나 보인다.

## 성능 비교 (100만 건, 인덱스 사용 / 미사용)

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
