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

int main(void)
{
    printf("test_concurrency (LOCK_MODE %s)\n", engine_lock_mode_name());
    RUN(test_two_readers_do_not_wait);
    RUN(test_reader_waits_for_writer);
    RUN(test_invalid_statement_takes_no_lock);
    RUN(test_lock_released_after_io_error);
    RUN(test_waiting_writer_blocks_new_readers);
    RUN(test_writes_to_different_tables_do_not_wait);
    TEST_DONE();
    return 0;
}
