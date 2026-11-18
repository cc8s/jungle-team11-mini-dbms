#include "test.h"
#include "test_util.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>

#include "engine_internal.h"
#include "table_internal.h"

/* 시간에 기대지 않는다: "기다리지 않는다" 는 trylock 결과로, "해제 후에 얻는다" 는 해제 직전에 세운
 * 플래그를 락을 얻은 쪽이 읽어서 확인한다. 순서 확인은 원자 카운터의 값으로 한다. */

#define USERS_SQL "CREATE TABLE users (id INT, name VARCHAR(8));"
#define ORDERS_SQL "CREATE TABLE orders (id INT, item VARCHAR(8));"
#define INSERT_USER "INSERT INTO users VALUES ('kim');"
#define INSERT_ORDER "INSERT INTO orders VALUES ('pen');"
/* 쓰기 스레드가 줄을 서는 데는 보통 수십 번이면 충분하다. 쓰기 우선이 아니면 영영 EBUSY 가 안 나오므로
 * 무한히 돌지 않고 실패하도록 시도 횟수에 넉넉한 상한을 둔다 (시간이 아니라 횟수) */
#define WRITER_QUEUE_MAX_TRIES 10000000

static TestDb open_db(const char *create_sql)
{
    TestDb db = test_db_create(create_sql);
    CHECK(db.dir != NULL);
    CHECK(db.engine != NULL);
    return db;
}

/* 엔진이 가진 테이블. 테스트가 락을 직접 잡기 위해 const 를 뗀다 (엔진 밖 코드는 쓰지 않는 방식) */
static Table *engine_table(Engine *engine, const char *name)
{
    Table *table = (Table *)engine_find_table(engine, name);
    CHECK(table != NULL);
    return table;
}

static ErrorCode run_error(Engine *engine, const char *sql)
{
    Result result = engine_execute(engine, sql, NULL, 0);
    ErrorCode error = result.error;
    result_free(&result);
    return error;
}

static pthread_t start_thread(void *(*body)(void *), void *arg)
{
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, body, arg) == 0);
    return thread;
}

/* ---- T40-01 읽기 + 읽기 ---- */

/* 이미 읽기 락이 잡힌 테이블에 다른 스레드가 읽기 락을 시도한다. trylock 이 0 이면 기다리지 않은 것 */
static void *read_while_reader_holds(void *arg)
{
    Table *table = arg;
    int rc = pthread_rwlock_tryrdlock(&table->rwlock);
    if (rc == 0)
        pthread_rwlock_unlock(&table->rwlock);
    table_lock_read(table); /* 위에서 바로 얻었으니 여기서도 막히지 않는다 */
    table_unlock(table);
    return (void *)(intptr_t)rc;
}

/* T40-01 */
static void test_two_readers_do_not_wait(void)
{
    TestDb db = open_db(USERS_SQL);
    Table *table = engine_table(db.engine, "users");

    table_lock_read(table);
    pthread_t reader = start_thread(read_while_reader_holds, table);
    void *rc = NULL;
    CHECK(pthread_join(reader, &rc) == 0);
    CHECK((intptr_t)rc == 0);
    table_unlock(table);
    test_db_destroy(&db);
}

/* ---- T40-02 쓰기 중 읽기 ---- */

typedef struct {
    Table *table;
    atomic_bool is_writer_released; /* 쓰기 쪽이 table_unlock 직전에 true */
    bool saw_release;               /* 읽기 쪽이 락을 얻은 순간의 값 */
} ReadAfterWrite;

static void *read_after_writer(void *arg)
{
    ReadAfterWrite *state = arg;
    table_lock_read(state->table);
    state->saw_release = atomic_load(&state->is_writer_released);
    table_unlock(state->table);
    return NULL;
}

/* T40-02 */
static void test_reader_waits_for_writer(void)
{
    TestDb db = open_db(USERS_SQL);
    ReadAfterWrite state = { .table = engine_table(db.engine, "users") };
    atomic_init(&state.is_writer_released, false);

    table_lock_write(state.table);
    pthread_t reader = start_thread(read_after_writer, &state);
    atomic_store(&state.is_writer_released, true);
    table_unlock(state.table);
    CHECK(pthread_join(reader, NULL) == 0);
    CHECK(state.saw_release); /* 쓰기 락이 잡혀 있는 동안에는 읽기 락을 얻지 못했다 */
    test_db_destroy(&db);
}

/* ---- E40-01 검증 실패는 락을 잡지 않는다 ---- */

/* E40-01 */
static void test_invalid_statement_takes_no_lock(void)
{
    TestDb db = open_db(USERS_SQL);
    CHECK(engine_lock_acquire_count(db.engine) == 0);
    CHECK(run_error(db.engine, "INSERT INTO users VALUES ('toolongname');") ==
          ERR_VALUE_TOO_LONG);
    CHECK(run_error(db.engine, "INSERT INTO users VALUES (1, 'kim');") == ERR_AUTO_ID);
    CHECK(run_error(db.engine, "SELECT age FROM users;") == ERR_UNKNOWN_COLUMN);
    CHECK(run_error(db.engine, "SELECT * FROM missing;") == ERR_UNKNOWN_TABLE);
    CHECK(run_error(db.engine, "SELEC * FROM users;") == ERR_SYNTAX);
    CHECK(engine_lock_acquire_count(db.engine) == 0);

    CHECK(run_error(db.engine, "SELECT * FROM users;") == ERR_NONE); /* 카운터가 실제로 센다 */
    CHECK(engine_lock_acquire_count(db.engine) == 1);
    CHECK(engine_locks_held(db.engine) == 0);
    test_db_destroy(&db);
}

/* ---- E40-02 실행 중 에러에도 락이 풀린다 ---- */

/* 다음 요청은 다른 스레드에서 보낸다. 락이 새어 있으면 (GLOBAL 뮤텍스든 RW 락이든) 여기서 막힌다 */
static void *insert_user(void *arg)
{
    Engine *engine = arg;
    return (void *)(intptr_t)run_error(engine, INSERT_USER);
}

static ErrorCode insert_user_in_thread(Engine *engine)
{
    pthread_t thread = start_thread(insert_user, engine);
    void *error = NULL;
    CHECK(pthread_join(thread, &error) == 0);
    return (ErrorCode)(intptr_t)error;
}

/* 테이블 fd 를 flags 로 연 다른 fd 로 바꿔 끼워 pread 또는 pwrite 가 EBADF 로 실패하게 한다 */
static int swap_table_fd(Table *table, const char *dir, int flags)
{
    char path[TEST_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s%s", dir, table_name(table), TABLE_FILE_EXT);
    int original = table->fd;
    table->fd = open(path, flags);
    CHECK(table->fd >= 0);
    return original;
}

static void restore_table_fd(Table *table, int original)
{
    close(table->fd);
    table->fd = original;
}

/* E40-02 */
static void test_lock_released_after_io_error(void)
{
    TestDb db = open_db(USERS_SQL);
    Table *table = engine_table(db.engine, "users");

    int original = swap_table_fd(table, db.dir, O_RDONLY); /* 쓰기 실패 → INSERT 의 ERR_IO */
    CHECK(run_error(db.engine, INSERT_USER) == ERR_IO);
    CHECK(engine_locks_held(db.engine) == 0);
    restore_table_fd(table, original);
    CHECK(insert_user_in_thread(db.engine) == ERR_NONE);

    original = swap_table_fd(table, db.dir, O_WRONLY); /* 읽기 실패 → SELECT 의 ERR_IO */
    CHECK(run_error(db.engine, "SELECT * FROM users;") == ERR_IO);
    CHECK(engine_locks_held(db.engine) == 0);
    restore_table_fd(table, original);
    CHECK(insert_user_in_thread(db.engine) == ERR_NONE);

    CHECK(engine_lock_acquire_count(db.engine) == 4);
    CHECK(engine_locks_held(db.engine) == 0);
    test_db_destroy(&db);
}

/* ---- E40-03 쓰기 우선 ---- */

typedef struct {
    Table *table;
    atomic_int next_turn; /* 락을 얻은 순서대로 0, 1, ... */
    int writer_turn;
    int late_reader_turn;
} WriterFirst;

static void *write_and_record_turn(void *arg)
{
    WriterFirst *state = arg;
    table_lock_write(state->table);
    state->writer_turn = atomic_fetch_add(&state->next_turn, 1);
    table_unlock(state->table);
    return NULL;
}

static void *read_and_record_turn(void *arg)
{
    WriterFirst *state = arg;
    table_lock_read(state->table);
    state->late_reader_turn = atomic_fetch_add(&state->next_turn, 1);
    table_unlock(state->table);
    return NULL;
}

/* 쓰기 스레드가 줄을 설 때까지 기다린다. 줄을 서기 전에는 새 읽기 시도가 성공하고 (곧바로 놓는다),
 * 쓰기가 기다리기 시작하면 쓰기 우선 정책 때문에 새 읽기가 EBUSY 를 받는다 */
static int wait_until_writer_queued(Table *table)
{
    for (int tries = 0; tries < WRITER_QUEUE_MAX_TRIES; tries++) {
        int rc = pthread_rwlock_tryrdlock(&table->rwlock);
        if (rc != 0)
            return rc;
        pthread_rwlock_unlock(&table->rwlock);
        sched_yield();
    }
    return 0;
}

/* E40-03 */
static void test_waiting_writer_blocks_new_readers(void)
{
    TestDb db = open_db(USERS_SQL);
    WriterFirst state = { .table = engine_table(db.engine, "users") };
    atomic_init(&state.next_turn, 0);

    table_lock_read(state.table); /* 먼저 들어와 있는 읽기 */
    pthread_t writer = start_thread(write_and_record_turn, &state);
    CHECK(wait_until_writer_queued(state.table) == EBUSY);
    pthread_t late_reader = start_thread(read_and_record_turn, &state); /* 쓰기 뒤에 온 읽기 */
    table_unlock(state.table);
    CHECK(pthread_join(writer, NULL) == 0);
    CHECK(pthread_join(late_reader, NULL) == 0);
    CHECK(state.writer_turn == 0); /* 늦게 온 읽기가 쓰기를 앞지르지 못한다 */
    CHECK(state.late_reader_turn == 1);
    test_db_destroy(&db);
}

/* ---- E40-04 다른 테이블 동시 쓰기 ---- */

static void *insert_order(void *arg)
{
    Engine *engine = arg;
    return (void *)(intptr_t)run_error(engine, INSERT_ORDER);
}

/* E40-04 (TABLE_RW 빌드만. GLOBAL 은 뮤텍스 하나라 다른 테이블끼리도 기다리는 것이 정상) */
static void test_writes_to_different_tables_do_not_wait(void)
{
    if (strcmp(engine_lock_mode_name(), "TABLE_RW") != 0) {
        printf("    (skipped: LOCK_MODE %s)\n", engine_lock_mode_name());
        return;
    }
    TestDb db = open_db(USERS_SQL ORDERS_SQL);
    Table *users = engine_table(db.engine, "users");

    table_lock_write(users); /* users 에 쓰기가 진행 중인 상태 */
    pthread_t thread = start_thread(insert_order, db.engine);
    void *error = NULL;
    CHECK(pthread_join(thread, &error) == 0); /* users 락을 놓기 전에 orders INSERT 가 끝난다 */
    CHECK((ErrorCode)(intptr_t)error == ERR_NONE);
    CHECK(pthread_rwlock_trywrlock(&users->rwlock) == EBUSY); /* users 는 여전히 잡혀 있다 */
    table_unlock(users);

    CHECK(run_error(db.engine, INSERT_USER) == ERR_NONE);
    test_db_destroy(&db);
}

/* ==== S41 동시성 부하 ====
 * 행이 스스로 검증되게 만든다: 스레드 t 의 seq 번째 INSERT 는 name = "t<t>-<seq>", tag = t * 1000000 + seq.
 * id 는 엔진이 쓰기 락 안에서 발급하므로 클라이언트가 미리 알 수 없다. 대신 같은 행의 name 과 tag 가
 * 서로 맞는지 보면, 반쯤 쓰인 레코드 (name 이 비었거나 tag 가 0) 는 걸린다. */

#define LOAD_TABLE_SQL "CREATE TABLE users (id INT, name VARCHAR(16), tag INT);"
#define LOAD_TWO_TABLES_SQL \
    LOAD_TABLE_SQL "CREATE TABLE orders (id INT, name VARCHAR(16), tag INT);"
#define TAG_THREAD_FACTOR 1000000
#define LOAD_SQL_SIZE 128
#define LOAD_NAME_SIZE 32
#define ID_COLUMN 0
#define NAME_COLUMN 1
#define TAG_COLUMN 2

#define T41_THREADS 8
#define T41_INSERTS 10000
#define MIXED_INSERTERS 2
#define MIXED_READERS 6
#define MIXED_INSERTS 2000
#define TWO_TABLE_THREADS 4 /* 테이블마다 */
#define TWO_TABLE_INSERTS 2000

typedef struct {
    Engine *engine;
    const char *table;
    int thread_no;
    int count;
    int32_t *ids; /* seq 번째 INSERT 가 받은 id */
} Inserter;

static void *run_inserter(void *arg)
{
    Inserter *inserter = arg;
    char sql[LOAD_SQL_SIZE];
    for (int seq = 0; seq < inserter->count; seq++) {
        snprintf(sql, sizeof(sql), "INSERT INTO %s VALUES ('t%d-%d', %d);", inserter->table,
                 inserter->thread_no, seq, inserter->thread_no * TAG_THREAD_FACTOR + seq);
        Result result = engine_execute(inserter->engine, sql, NULL, 0);
        CHECK(result.error == ERR_NONE);
        inserter->ids[seq] = result.last_insert_id;
        result_free(&result);
    }
    return NULL;
}

/* 스레드 thread_count 개가 table 에 count 개씩 넣을 준비를 한다 (ids 는 스레드마다 malloc) */
static void prepare_inserters(Inserter *inserters, int thread_count, Engine *engine,
                              const char *table, int count)
{
    for (int i = 0; i < thread_count; i++) {
        inserters[i] = (Inserter){ .engine = engine, .table = table, .thread_no = i,
                                   .count = count, .ids = calloc((size_t)count, sizeof(int32_t)) };
        CHECK(inserters[i].ids != NULL);
    }
}

/* 받은 id 를 모두 모으면 1..thread_count*count 가 정확히 한 번씩 나와야 한다 (중복·빈 곳 없음) */
static void check_ids_complete(Inserter *inserters, int thread_count)
{
    int total = thread_count * inserters[0].count;
    bool *seen = calloc((size_t)total + 1, sizeof(bool));
    CHECK(seen != NULL);
    for (int i = 0; i < thread_count; i++) {
        for (int seq = 0; seq < inserters[i].count; seq++) {
            int32_t id = inserters[i].ids[seq];
            CHECK(id >= 1 && id <= total);
            CHECK(!seen[id]);
            seen[id] = true;
        }
    }
    free(seen); /* 개수가 total 이고 중복이 없으므로 1..total 이 모두 나왔다 */
}

static void free_inserters(Inserter *inserters, int thread_count)
{
    for (int i = 0; i < thread_count; i++)
        free(inserters[i].ids);
}

/* row 행이 온전한지: id 는 위치 + 1 (빈 곳 없이 이어짐), 컬럼이 3 개면 name 이 tag 와 맞는다 */
static void check_row(const Result *result, int64_t row)
{
    CHECK(result_get_int(result, row, ID_COLUMN) == row + 1);
    if (result->column_count == 1)
        return;
    int32_t tag = result_get_int(result, row, TAG_COLUMN);
    size_t len = 0;
    const char *name = result_get_str(result, row, NAME_COLUMN, &len);
    char actual[LOAD_NAME_SIZE] = { 0 };
    memcpy(actual, name, len < sizeof(actual) - 1 ? len : sizeof(actual) - 1);
    char expected[LOAD_NAME_SIZE];
    snprintf(expected, sizeof(expected), "t%d-%d", tag / TAG_THREAD_FACTOR,
             tag % TAG_THREAD_FACTOR);
    CHECK_STR_EQ(actual, expected);
}

/* sql 을 실행해 모든 행을 검사하고 행 수를 돌려준다 */
static int64_t select_and_check(Engine *engine, const char *sql)
{
    Result result = engine_execute(engine, sql, NULL, 0);
    CHECK(result.error == ERR_NONE);
    for (int64_t row = 0; row < result.row_count; row++)
        check_row(&result, row);
    int64_t count = result.row_count;
    result_free(&result);
    return count;
}

/* ---- T41-01, T41-02 ---- */

static void start_inserters(pthread_t *threads, Inserter *inserters, int thread_count)
{
    for (int i = 0; i < thread_count; i++)
        threads[i] = start_thread(run_inserter, &inserters[i]);
}

static void join_all(pthread_t *threads, int thread_count)
{
    for (int i = 0; i < thread_count; i++)
        CHECK(pthread_join(threads[i], NULL) == 0);
}

/* T41-01, T41-02 */
static void test_parallel_inserts_get_unique_ids(void)
{
    TestDb db = open_db(LOAD_TABLE_SQL);
    Inserter inserters[T41_THREADS];
    pthread_t threads[T41_THREADS];
    prepare_inserters(inserters, T41_THREADS, db.engine, "users", T41_INSERTS);
    start_inserters(threads, inserters, T41_THREADS);
    join_all(threads, T41_THREADS);

    int total = T41_THREADS * T41_INSERTS;
    check_ids_complete(inserters, T41_THREADS);
    CHECK(engine_record_count(db.engine, 0) == total);
    CHECK(select_and_check(db.engine, "SELECT * FROM users;") == total);
    free_inserters(inserters, T41_THREADS);

    test_db_reopen(&db); /* T41-02: 헤더와 파일이 맞아서 복구 없이 같은 인덱스가 재구축된다 */
    CHECK(db.engine != NULL);
    CHECK(engine_warning_count(db.engine) == 0);
    CHECK(btree_size(table_index(engine_find_table(db.engine, "users"))) == total);
    CHECK(select_and_check(db.engine, "SELECT * FROM users WHERE id >= 1;") == total);
    test_db_destroy(&db);
}

/* ---- T41-03, E41-01, E41-02 ---- */

typedef struct {
    Engine *engine;
    const char *sql;
    atomic_bool *is_inserting;
    int64_t select_count;
} Reader;

/* INSERT 가 끝날 때까지 SELECT 를 반복한다. 행은 늘 온전하고 (E41-01), 행 수는 줄지 않는다 (E41-02).
 * do-while 이라 INSERT 가 먼저 끝나도 한 번은 읽는다 */
static void *run_reader(void *arg)
{
    Reader *reader = arg;
    int64_t last_count = 0;
    do {
        int64_t count = select_and_check(reader->engine, reader->sql);
        CHECK(count >= last_count);
        last_count = count;
        reader->select_count++;
    } while (atomic_load(reader->is_inserting));
    return NULL;
}

/* 읽기 경로를 섞는다: 인덱스 범위 (id 만 / 전체 컬럼), 선형 스캔 */
static const char *const MIXED_READER_SQL[] = {
    "SELECT id FROM users WHERE id >= 1;",
    "SELECT * FROM users WHERE id >= 1;",
    "SELECT * FROM users;",
};
#define MIXED_READER_SQL_COUNT ((int)(sizeof(MIXED_READER_SQL) / sizeof(MIXED_READER_SQL[0])))

static void start_readers(pthread_t *threads, Reader *readers, Engine *engine,
                          atomic_bool *is_inserting)
{
    for (int i = 0; i < MIXED_READERS; i++) {
        readers[i] = (Reader){ .engine = engine, .is_inserting = is_inserting,
                               .sql = MIXED_READER_SQL[i % MIXED_READER_SQL_COUNT] };
        threads[i] = start_thread(run_reader, &readers[i]);
    }
}

/* T41-03, E41-01, E41-02 */
static void test_mixed_inserts_and_selects(void)
{
    TestDb db = open_db(LOAD_TABLE_SQL);
    atomic_bool is_inserting;
    atomic_init(&is_inserting, true);
    Reader readers[MIXED_READERS];
    pthread_t reader_threads[MIXED_READERS];
    start_readers(reader_threads, readers, db.engine, &is_inserting);

    Inserter inserters[MIXED_INSERTERS];
    pthread_t inserter_threads[MIXED_INSERTERS];
    prepare_inserters(inserters, MIXED_INSERTERS, db.engine, "users", MIXED_INSERTS);
    start_inserters(inserter_threads, inserters, MIXED_INSERTERS);
    join_all(inserter_threads, MIXED_INSERTERS);
    atomic_store(&is_inserting, false);
    join_all(reader_threads, MIXED_READERS);

    check_ids_complete(inserters, MIXED_INSERTERS);
    CHECK(select_and_check(db.engine, "SELECT * FROM users;") == MIXED_INSERTERS * MIXED_INSERTS);
    for (int i = 0; i < MIXED_READERS; i++)
        CHECK(readers[i].select_count >= 1);
    free_inserters(inserters, MIXED_INSERTERS);
    test_db_destroy(&db);
}

/* ---- E41-03 ---- */

/* E41-03 */
static void test_two_tables_get_independent_ids(void)
{
    TestDb db = open_db(LOAD_TWO_TABLES_SQL);
    Inserter users[TWO_TABLE_THREADS];
    Inserter orders[TWO_TABLE_THREADS];
    pthread_t user_threads[TWO_TABLE_THREADS];
    pthread_t order_threads[TWO_TABLE_THREADS];
    prepare_inserters(users, TWO_TABLE_THREADS, db.engine, "users", TWO_TABLE_INSERTS);
    prepare_inserters(orders, TWO_TABLE_THREADS, db.engine, "orders", TWO_TABLE_INSERTS);
    start_inserters(user_threads, users, TWO_TABLE_THREADS);
    start_inserters(order_threads, orders, TWO_TABLE_THREADS);
    join_all(user_threads, TWO_TABLE_THREADS);
    join_all(order_threads, TWO_TABLE_THREADS);

    int per_table = TWO_TABLE_THREADS * TWO_TABLE_INSERTS;
    check_ids_complete(users, TWO_TABLE_THREADS); /* 각 테이블이 1..per_table 을 따로 쓴다 */
    check_ids_complete(orders, TWO_TABLE_THREADS);
    CHECK(select_and_check(db.engine, "SELECT * FROM users;") == per_table);
    CHECK(select_and_check(db.engine, "SELECT * FROM orders;") == per_table);
    free_inserters(users, TWO_TABLE_THREADS);
    free_inserters(orders, TWO_TABLE_THREADS);
    test_db_destroy(&db);
}

int main(void)
{
    printf("test_concurrency (LOCK_MODE %s)\n", engine_lock_mode_name());
    RUN(test_two_readers_do_not_wait);
    RUN(test_reader_waits_for_writer);
    RUN(test_invalid_statement_takes_no_lock);
    RUN(test_lock_released_after_io_error);
    RUN(test_waiting_writer_blocks_new_readers);
    RUN(test_writes_to_different_tables_do_not_wait);
    RUN(test_parallel_inserts_get_unique_ids);
    RUN(test_mixed_inserts_and_selects);
    RUN(test_two_tables_get_independent_ids);
    TEST_DONE();
    return 0;
}
