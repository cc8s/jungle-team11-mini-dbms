#include <signal.h>
#include <stdatomic.h>
#include <unistd.h>

#include "executor.h"
#include "row.h"
#include "test.h"

/* 동시 INSERT/SELECT 스트레스 (D005, D007, 학습 노트 2 Q8·Q9·Q13).
 * TSan 빌드(make tsan)에서도 같은 테스트가 돈다. TSan 은 실제로 겹친 실행에서만 레이스를 잡으므로
 * 여기서 일부러 많이 겹치게 만든다. 건수는 TSan 에서도 CI 시간 안에 끝나게 잡았다. */

#define SCHEMA_DIR     "schema"
#define WRITERS        4
#define READERS        4
#define PER_WRITER     1500
#define TOTAL          (WRITERS * PER_WRITER)
#define STARVE_SECONDS 60

static char data_dir[64];
static struct executor ex;
static atomic_int writers_done;
static atomic_long reads_done;
static atomic_int torn_rows;
static atomic_int failures;

/* 스레드 안에서는 CHECK 를 쓰지 않는다(CHECK 의 통과 횟수 변수는 락이 없다). 실패는 원자 변수로 센다 */
static bool parse_sql(const char *sql, struct statement *stmt)
{
    char err[256];
    size_t consumed;

    if (parser_parse(sql, &consumed, stmt, err, sizeof(err)) != PARSE_OK) {
        atomic_fetch_add(&failures, 1);
        return false;
    }
    return true;
}

static void *writer_main(void *arg)
{
    int w = (int)(intptr_t)arg;
    struct statement stmt;
    struct result res;
    char sql[128];
    char err[256];

    for (int i = 0; i < PER_WRITER; i++) {
        /* email = name + "@x" 라는 규칙을 둔다. 읽는 쪽이 반쯤 쓰인 행을 보면 규칙이 깨진다 */
        snprintf(sql, sizeof(sql), "INSERT INTO users VALUES ('w%d-%d', 'w%d-%d@x', %d)", w, i, w,
                 i, w);
        if (!parse_sql(sql, &stmt)) {
            continue;
        }
        if (executor_execute(&ex, &stmt, &res, err, sizeof(err)) != 0) {
            atomic_fetch_add(&failures, 1);
        }
        result_free(&res);
    }
    atomic_fetch_add(&writers_done, 1);
    return NULL;
}

static void check_rows(const struct result *res)
{
    char name[40];
    char email[80];
    char want[48];

    for (size_t k = 0; k < res->row_count; k++) {
        const void *row = result_row(res, k);
        row_get_string(res->schema, 1, row, name, sizeof(name));
        row_get_string(res->schema, 2, row, email, sizeof(email));
        snprintf(want, sizeof(want), "%s@x", name);
        if (strcmp(email, want) != 0 || row_get_int(res->schema, 0, row) <= 0) {
            atomic_fetch_add(&torn_rows, 1);
        }
    }
}

/* 쓰는 쪽이 다 끝날 때까지 세 가지 SELECT 를 번갈아 계속 던진다.
 * 쓰기 굶주림이 있으면 writers_done 이 영원히 안 차서 이 루프가 안 끝난다 → alarm 이 실패시킨다 */
static void *reader_main(void *arg)
{
    unsigned seed = (unsigned)(intptr_t)arg * 2654435761u;
    struct statement stmt;
    struct result res;
    char sql[128];
    char err[256];

    while (atomic_load(&writers_done) < WRITERS) {
        seed = seed * 1103515245u + 12345u;
        long long id = (long long)((seed >> 8) % TOTAL) + 1;
        switch (seed % 3) {
        case 0:
            snprintf(sql, sizeof(sql), "SELECT * FROM users WHERE id = %lld", id);
            break;
        case 1:
            snprintf(sql, sizeof(sql), "SELECT * FROM users WHERE id BETWEEN %lld AND %lld", id,
                     id + 50);
            break;
        default:
            snprintf(sql, sizeof(sql), "SELECT * FROM users WHERE age = %d", (int)(seed % WRITERS));
            break;
        }
        if (!parse_sql(sql, &stmt)) {
            continue;
        }
        if (executor_execute(&ex, &stmt, &res, err, sizeof(err)) != 0) {
            atomic_fetch_add(&failures, 1);
            continue;
        }
        check_rows(&res);
        result_free(&res);
        atomic_fetch_add(&reads_done, 1);
    }
    return NULL;
}

static void on_alarm(int sig)
{
    (void)sig;
    static const char msg[] = "  FAIL: did not finish in time (writer starvation or deadlock?)\n";
    if (write(STDERR_FILENO, msg, sizeof(msg) - 1) < 0) {
        /* 끝내는 길이라 실패해도 할 수 있는 게 없다 */
    }
    _exit(1);
}

struct verify_ctx {
    const struct schema *schema;
    unsigned char seen_id[TOTAL + 1];
    unsigned char seen_job[WRITERS][PER_WRITER];
    int bad;
};

static int verify_row(const void *row, uint64_t offset, void *arg)
{
    struct verify_ctx *c = arg;
    long long id = row_get_int(c->schema, 0, row);
    char name[40];
    int w;
    int i;

    (void)offset;
    row_get_string(c->schema, 1, row, name, sizeof(name));
    if (id < 1 || id > TOTAL || c->seen_id[id] || sscanf(name, "w%d-%d", &w, &i) != 2 || w < 0 ||
        w >= WRITERS || i < 0 || i >= PER_WRITER || c->seen_job[w][i]) {
        c->bad++;
        return 0;
    }
    c->seen_id[id] = 1;
    c->seen_job[w][i] = 1;
    return 0;
}

/* 학습 노트 2 Q13 의 사후 검증: id 중복 0, 누락 0, 파일 행 수 = 트리 키 수 = 성공한 INSERT 수 */
static void verify_table(void)
{
    static struct verify_ctx c;
    struct table *t = ex.tables[0];

    memset(&c, 0, sizeof(c));
    c.schema = &t->schema;
    CHECK(storage_scan(&t->storage, verify_row, &c) == 0);
    CHECK(c.bad == 0);
    CHECK(storage_row_count(&t->storage) == TOTAL);
    CHECK(t->is_index_valid && t->index.count == TOTAL);
    for (int id = 1; id <= TOTAL; id++) {
        CHECK(c.seen_id[id] == 1);
    }
    for (int w = 0; w < WRITERS; w++) {
        for (int i = 0; i < PER_WRITER; i++) {
            CHECK(c.seen_job[w][i] == 1);
        }
    }
    CHECK(t->storage.next_id == TOTAL + 1);
}

/* TC-CONC-001 동시 INSERT·SELECT: id 중복·누락 0, 행 수 일치, 반쯤 쓰인 행 0, 쓰기 굶주림 없음 */
static void test_concurrent_insert_select(void)
{
    pthread_t writers[WRITERS];
    pthread_t readers[READERS];
    struct statement stmt;
    struct result res;
    char err[256];

    strcpy(data_dir, "/tmp/minidb-conc-XXXXXX");
    CHECK(mkdtemp(data_dir) != NULL);
    CHECK(executor_init(&ex, SCHEMA_DIR, data_dir) == 0);
    CHECK(parse_sql("SELECT id FROM users", &stmt)); /* 테이블을 먼저 열어 둔다 */
    CHECK(executor_execute(&ex, &stmt, &res, err, sizeof(err)) == 0);
    result_free(&res);

    signal(SIGALRM, on_alarm);
    alarm(STARVE_SECONDS);
    for (int i = 0; i < READERS; i++) {
        CHECK(pthread_create(&readers[i], NULL, reader_main, (void *)(intptr_t)(i + 1)) == 0);
    }
    for (int i = 0; i < WRITERS; i++) {
        CHECK(pthread_create(&writers[i], NULL, writer_main, (void *)(intptr_t)i) == 0);
    }
    for (int i = 0; i < WRITERS; i++) {
        pthread_join(writers[i], NULL);
    }
    for (int i = 0; i < READERS; i++) {
        pthread_join(readers[i], NULL);
    }
    alarm(0);

    printf("    %d inserts, %ld selects alongside\n", TOTAL, atomic_load(&reads_done));
    CHECK(atomic_load(&failures) == 0);
    CHECK(atomic_load(&torn_rows) == 0);
    verify_table();
}

/* TC-CONC-002 재시작: 닫고 다시 열어도 같은 검증을 통과한다 (트리 재구성, next_id) */
static void test_reopen_after_concurrency(void)
{
    struct statement stmt;
    struct result res;
    char err[256];

    executor_close(&ex);
    CHECK(executor_init(&ex, SCHEMA_DIR, data_dir) == 0);
    CHECK(parse_sql("SELECT id FROM users WHERE id = 1", &stmt));
    CHECK(executor_execute(&ex, &stmt, &res, err, sizeof(err)) == 0);
    CHECK(res.row_count == 1);
    result_free(&res);
    verify_table();
    executor_close(&ex);
}

int main(void)
{
    RUN(test_concurrent_insert_select);
    RUN(test_reopen_after_concurrency);
    TEST_DONE();
    return 0;
}
