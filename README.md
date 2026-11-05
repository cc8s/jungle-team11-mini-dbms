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
make                    # 빌드
make test               # 단위 테스트
./minidb query.sql      # SQL 파일 실행 (Phase 1)
```

## 아키텍처

<!-- SQL → 토크나이저 → 파서 → 실행기 → (B+ 트리 | 선형 탐색) → 파일 저장소 / HTTP → 스레드 풀 → 엔진 -->

## 핵심 로직

### SQL 처리기

<!-- 토큰, Statement 구조, 지원 문법 -->

### 저장 포맷

<!-- schema/<table>.schema, data/<table>.tbl 고정 길이 행, 문자열 최대 길이, 파일 핸들 유지 + 버퍼링 -->

### B+ 트리 인덱스

<!-- 노드 구조, 분할, id → 오프셋, 리프 연결 리스트로 BETWEEN 범위 검색 -->

### 스레드 풀과 동시성

<!-- 작업 큐(mutex + condvar), 테이블별 rwlock, id 증가는 write lock 안에서 -->

## 테스트

| 구분 | 내용 |
|---|---|
| 단위 | |
| 기능 | |
| 동시성 | |
| 엣지 케이스 | |

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
