#ifndef TEST_UTIL_H
#define TEST_UTIL_H

/* 테스트 공용 도우미. 테스트마다 mkdtemp 로 만든 독립 디렉터리를 쓰고 끝나면 통째로 지운다.
 * 고정 경로를 쓰지 않으므로 테스트를 병렬로 돌리거나 실패 후 다시 돌려도 서로 섞이지 않는다. */

/* nftw, mkdtemp 는 Makefile 의 -D_GNU_SOURCE 로 선언이 보인다 */
#include <ftw.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PATH_MAX 4096
#define TEST_TEMP_TEMPLATE "/tmp/minidb-test-XXXXXX"
#define TEST_NFTW_OPEN_FDS 16 /* nftw 가 동시에 열어 둘 디렉터리 fd 수 */

/* 새 임시 디렉터리를 만들고 경로를 돌려준다 (호출한 쪽이 free). 실패하면 NULL. */
static inline char *test_make_temp_dir(void)
{
    char *path = strdup(TEST_TEMP_TEMPLATE);
    if (path == NULL)
        return NULL;
    if (mkdtemp(path) == NULL) {
        free(path);
        return NULL;
    }
    return path;
}

static inline int remove_visited_entry(const char *path, const struct stat *info, int type,
                                       struct FTW *walk)
{
    (void)info;
    (void)type;
    (void)walk;
    return remove(path);
}

/* path 와 그 아래 모든 파일·디렉터리를 지운다. 성공하면 true.
 * FTW_DEPTH: 자식을 먼저 방문해야 비워진 디렉터리를 remove 로 지울 수 있다.
 * FTW_PHYS: 심볼릭 링크를 따라가지 않아 임시 디렉터리 밖을 지우지 않는다.
 * 콜백이 상태를 갖지 않으므로 전역 변수 없이 재진입 가능하다. */
static inline bool test_remove_dir(const char *path)
{
    return nftw(path, remove_visited_entry, TEST_NFTW_OPEN_FDS, FTW_DEPTH | FTW_PHYS) == 0;
}

/* 엔진 테스트용 데이터 디렉터리 묶음.
 * TODO(S14): engine 모듈이 생기면 Engine * 를 추가하고, create_sql 로 테이블을 만든 뒤 엔진을 연다. */
typedef struct {
    char *dir; /* 실패하면 NULL */
} TestDb;

static inline TestDb test_db_create(const char *create_sql)
{
    (void)create_sql; /* S14 전까지는 디렉터리만 준비한다 */
    TestDb db = { .dir = test_make_temp_dir() };
    return db;
}

static inline void test_db_destroy(TestDb *db)
{
    if (db->dir != NULL)
        test_remove_dir(db->dir); /* 정리 실패는 테스트 결과에 영향이 없어 무시한다 */
    free(db->dir);
    db->dir = NULL;
}

#endif
