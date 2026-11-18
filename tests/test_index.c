#include <unistd.h>

#include "cli.h"
#include "executor.h"
#include "row.h"
#include "test.h"

/* schema/users.schema: id INT, name VARCHAR(32), email VARCHAR(64), age INT */
#define SCHEMA_DIR "schema"

static char data_dir[64];
static char err[512];

static void fresh_dir(void)
{
    char path[128];

    if (data_dir[0] == '\0') {
        strcpy(data_dir, "/tmp/minidb-index-XXXXXX");
        CHECK(mkdtemp(data_dir) != NULL);
    }
    snprintf(path, sizeof(path), "%s/users.tbl", data_dir);
    unlink(path);
}

static void open_ex(struct executor *ex, bool use_index)
{
    CHECK(executor_init(ex, SCHEMA_DIR, data_dir) == 0);
    ex->use_index = use_index;
}

static void exec_ok(struct executor *ex, const char *sql, struct result *res)
{
    struct statement stmt;
    size_t consumed;

    CHECK(parser_parse(sql, &consumed, &stmt, err, sizeof(err)) == PARSE_OK);
    int rc = executor_execute(ex, &stmt, res, err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "  %s → %s\n", sql, err);
    }
    CHECK(rc == 0);
}

/* 출력 문자열 (cli_run_sql) — 호출한 쪽이 free */
static char *run_text(struct executor *ex, const char *sql)
{
    char *buf;
    size_t len;
    FILE *out = open_memstream(&buf, &len);

    CHECK(out != NULL);
    cli_run_sql(ex, sql, out);
    fclose(out);
    return buf;
}

static void insert_users(struct executor *ex, int n)
{
    char sql[128];

    for (int i = 1; i <= n; i++) {
        snprintf(sql, sizeof(sql), "INSERT INTO users VALUES ('u%d', 'u%d@x', %d)", i, i, i % 50);
        free(run_text(ex, sql));
    }
}

/* TC-IDX-001 DT: WHERE 모양 × 인덱스 켬/끔 → 탐색 방법과 확인한 행 수 (D003) */
static void test_plan_choice(void)
{
    static const struct {
        const char *sql;
        enum result_plan with_index;
        size_t examined_with_index;
        size_t rows;
    } cases[] = {
        {"SELECT * FROM users WHERE id = 7", RESULT_PLAN_INDEX_LOOKUP, 1, 1},
        {"SELECT * FROM users WHERE id BETWEEN 10 AND 19", RESULT_PLAN_INDEX_RANGE, 10, 10},
        {"SELECT * FROM users WHERE id BETWEEN 5 AND 5", RESULT_PLAN_INDEX_LOOKUP, 1, 1},
        {"SELECT * FROM users WHERE ID = 7", RESULT_PLAN_INDEX_LOOKUP, 1, 1}, /* 대소문자 */
        {"SELECT * FROM users WHERE age = 7", RESULT_PLAN_SCAN, 100, 2},
        {"SELECT * FROM users WHERE name = 'u7'", RESULT_PLAN_SCAN, 100, 1},
        {"SELECT * FROM users WHERE age BETWEEN 0 AND 1", RESULT_PLAN_SCAN, 100, 4},
        {"SELECT * FROM users", RESULT_PLAN_SCAN, 100, 100},
    };
    struct executor on;
    struct executor off;
    struct result res;

    fresh_dir();
    open_ex(&on, true);
    insert_users(&on, 100);
    executor_close(&on);

    open_ex(&on, true);
    open_ex(&off, false);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        exec_ok(&on, cases[i].sql, &res);
        CHECK(res.plan == cases[i].with_index);
        CHECK(res.rows_examined == cases[i].examined_with_index);
        CHECK(res.row_count == cases[i].rows);
        result_free(&res);

        exec_ok(&off, cases[i].sql, &res);
        CHECK(res.plan == RESULT_PLAN_SCAN && res.rows_examined == 100);
        CHECK(res.row_count == cases[i].rows);
        result_free(&res);
    }
    executor_close(&on);
    executor_close(&off);
}

/* TC-IDX-002 BVA: 없는 id, 범위가 데이터 밖 — 트리는 아무 행도 읽지 않는다 */
static void test_lookup_miss(void)
{
    struct executor ex;
    struct result res;

    fresh_dir();
    open_ex(&ex, true);
    insert_users(&ex, 10);
    static const char *misses[] = {
        "SELECT * FROM users WHERE id = 0",
        "SELECT * FROM users WHERE id = 11",
        "SELECT * FROM users WHERE id = -3",
        "SELECT * FROM users WHERE id BETWEEN 11 AND 100",
        "SELECT * FROM users WHERE id BETWEEN 8 AND 2",
    };
    for (size_t i = 0; i < sizeof(misses) / sizeof(misses[0]); i++) {
        exec_ok(&ex, misses[i], &res);
        CHECK(res.row_count == 0 && res.rows_examined == 0);
        result_free(&res);
    }
    executor_close(&ex);
}

/* TC-IDX-003 차등: 같은 무작위 질의를 인덱스 켬/끔으로 실행해 출력이 바이트 단위로 같아야 한다 */
static void test_differential(void)
{
    struct executor on;
    struct executor off;
    char sql[128];
    unsigned seed = 7;
    int compared = 0;

    fresh_dir();
    open_ex(&on, true);
    insert_users(&on, 2000);
    executor_close(&on); /* 다 쓰고 닫은 뒤 두 실행기가 같은 파일을 읽기만 한다 */
    open_ex(&on, true);
    open_ex(&off, false);

    for (int i = 0; i < 400; i++) {
        seed = seed * 1103515245u + 12345u;
        long long a = (long long)((seed >> 8) % 2200) - 100; /* 데이터 밖(-100..0, 2001..)도 섞는다 */
        seed = seed * 1103515245u + 12345u;
        long long b = (long long)((seed >> 8) % 2200) - 100;
        if (i % 2 == 0) {
            snprintf(sql, sizeof(sql), "SELECT id, name FROM users WHERE id = %lld", a);
        } else {
            snprintf(sql, sizeof(sql), "SELECT * FROM users WHERE id BETWEEN %lld AND %lld", a, b);
        }
        char *x = run_text(&on, sql);
        char *y = run_text(&off, sql);
        if (strcmp(x, y) != 0) {
            fprintf(stderr, "  differs: %s\n", sql);
        }
        CHECK_STR_EQ(x, y);
        compared++;
        free(x);
        free(y);
    }
    CHECK(compared == 400);
    executor_close(&on);
    executor_close(&off);
}

/* TC-IDX-004 ST: 다시 열면 파일에서 트리를 재구성한다. next_id 도 이어진다 */
static void test_rebuild_on_open(void)
{
    struct executor ex;
    struct result res;

    fresh_dir();
    open_ex(&ex, true);
    insert_users(&ex, 1000);
    executor_close(&ex);

    open_ex(&ex, true);
    exec_ok(&ex, "SELECT name FROM users WHERE id = 1000", &res);
    CHECK(res.plan == RESULT_PLAN_INDEX_LOOKUP && res.row_count == 1);
    struct table *t = ex.tables[0];
    CHECK(t->is_index_valid && t->index.count == 1000);
    char name[40];
    row_get_string(&t->schema, 1, result_row(&res, 0), name, sizeof(name));
    CHECK_STR_EQ(name, "u1000");
    result_free(&res);
    exec_ok(&ex, "INSERT INTO users (name) VALUES ('next')", &res);
    CHECK(res.inserted_id == 1001);
    executor_close(&ex);
}

/* TC-IDX-005 ST: 방금 넣은 행(아직 쓰기 버퍼에 있음)도 바로 인덱스로 찾는다 */
static void test_lookup_right_after_insert(void)
{
    struct executor ex;
    struct result res;

    fresh_dir();
    open_ex(&ex, true);
    insert_users(&ex, 5);
    exec_ok(&ex, "SELECT name FROM users WHERE id = 5", &res);
    CHECK(res.plan == RESULT_PLAN_INDEX_LOOKUP && res.row_count == 1);
    result_free(&res);
    executor_close(&ex);
}

/* TC-IDX-006 EG: 파일에 같은 id 가 두 번 있으면(손상) 테이블을 열지 않는다 */
static void test_duplicate_id_in_file(void)
{
    struct executor ex;
    struct schema s;
    struct storage st;
    char path[128];
    char row[256] = {0};
    uint64_t off;

    fresh_dir();
    CHECK(schema_load(SCHEMA_DIR, "users", &s, err, sizeof(err)) == 0);
    snprintf(path, sizeof(path), "%s/users.tbl", data_dir);
    CHECK(storage_open(&st, path, s.row_size, err, sizeof(err)) == 0);
    for (int i = 0; i < 3; i++) {
        row_set_int(&s, 0, row, i == 2 ? 1 : i + 1); /* id 1, 2, 1 */
        CHECK(storage_append(&st, row, &off) == 0);
    }
    CHECK(storage_close(&st) == 0);

    open_ex(&ex, true);
    char *out = run_text(&ex, "SELECT * FROM users");
    CHECK(strstr(out, "corrupted: id 1 appears twice") != NULL);
    free(out);
    executor_close(&ex);
}

/* TC-IDX-007 EG: 인덱스가 무효가 되면(트리 삽입 실패를 흉내) 선형 탐색으로 같은 답을 낸다 */
static void test_invalid_index_falls_back(void)
{
    struct executor ex;
    struct result res;

    fresh_dir();
    open_ex(&ex, true);
    insert_users(&ex, 20);
    ex.tables[0]->is_index_valid = false;
    exec_ok(&ex, "SELECT name FROM users WHERE id = 12", &res);
    CHECK(res.plan == RESULT_PLAN_SCAN && res.row_count == 1);
    result_free(&res);
    exec_ok(&ex, "INSERT INTO users (name) VALUES ('after')", &res); /* 무효인 동안은 트리에 넣지 않는다 */
    CHECK(res.inserted_id == 21 && ex.tables[0]->index.count == 20);
    exec_ok(&ex, "SELECT name FROM users WHERE id = 21", &res);
    CHECK(res.plan == RESULT_PLAN_SCAN && res.row_count == 1);
    result_free(&res);
    executor_close(&ex);

    open_ex(&ex, true); /* 다시 열면 트리를 새로 만들어 21 도 들어간다 */
    exec_ok(&ex, "SELECT name FROM users WHERE id = 21", &res);
    CHECK(res.plan == RESULT_PLAN_INDEX_LOOKUP && res.row_count == 1);
    result_free(&res);
    executor_close(&ex);
}

int main(void)
{
    RUN(test_plan_choice);
    RUN(test_lookup_miss);
    RUN(test_differential);
    RUN(test_rebuild_on_open);
    RUN(test_lookup_right_after_insert);
    RUN(test_duplicate_id_in_file);
    RUN(test_invalid_index_falls_back);
    TEST_DONE();
    return 0;
}
