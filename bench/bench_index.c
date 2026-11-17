/* 인덱스 경로 대 선형 탐색 벤치마크 (Phase 2 S29).
 *
 *   make bench                        N = 1만, 10만, 100만
 *   make bench BENCH_SIZES="10000"    규모 지정
 *
 * 엔진 API 를 직접 호출해서 출력 비용 없이 잰다. 시간은 CLOCK_MONOTONIC.
 * 측정하는 쿼리마다 결과 행 수와 used_index 를 검증하고, 틀리면 실패 (종료 코드 1) 로 끝낸다.
 * 데이터는 늘 /tmp 아래 임시 디렉터리에 만든다: bind mount 경로 (호스트 공유 폴더) 는 파일 I/O 가
 * 호스트를 거쳐 느리고 들쭉날쭉해서 측정값이 디스크가 아니라 공유 계층을 재게 된다. */

#include <fcntl.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "btree.h"
#include "engine.h"
#include "parser.h"
#include "table.h"

#define TEMP_TEMPLATE "/tmp/minidb-bench-XXXXXX"
#define USERS_SQL "CREATE TABLE users (id INT, name VARCHAR(32), age INT)"
#define USERS_TABLE "users"
#define PATH_SIZE 256
#define SQL_SIZE 128
#define MAX_SIZES 8
#define DEFAULT_SIZE_COUNT 3
#define INDEX_REPEAT 1000    /* 인덱스 쿼리는 짧아서 많이 반복해 평균 */
#define SCAN_REPEAT 100      /* 선형 탐색은 100 만 건에서 수십 ms 라 반복을 줄인다 */
#define RANDOM_KEY_COUNT 1000
#define RANGE_LIMIT 100
#define RARE_AGE 99          /* age >= 99 는 약 1 %: 선형 탐색이 LIMIT 을 채우려면 멀리 가야 한다 */
#define MIN_AGE 1
#define AGE_SPAN 99          /* 나이 1 ~ 99 */
#define RANDOM_SEED 20241117u
#define NFTW_OPEN_FDS 16
#define MS_PER_SECOND 1000.0
#define NS_PER_MS 1000000.0

static const int32_t DEFAULT_SIZES[DEFAULT_SIZE_COUNT] = {10000, 100000, 1000000};

typedef struct {
    char dir[PATH_SIZE];
    char table_path[PATH_SIZE];
    Engine *engine;
    int32_t row_count;
    int32_t rare_age_count; /* age >= RARE_AGE 인 행 수 (범위 쿼리 기대값) */
    uint32_t random_state;
} Bench;

/* 규모 하나의 요약 (마지막 규모별 표) */
typedef struct {
    int32_t row_count;
    double insert_ms;
    double rebuild_ms;
    int height;
    double random_index_ms;
    double random_scan_ms;
} SizeSummary;

/* 한 쿼리의 콜드 (페이지 캐시를 비운 첫 실행) / 웜 (반복 평균) 시간 */
typedef struct {
    double cold_ms;
    double warm_ms;
} Timing;

/* ---- 공통 ---- */

static void die(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));

static void die(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fputs("bench_index: ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
    exit(EXIT_FAILURE);
}

static double now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec * MS_PER_SECOND + (double)now.tv_nsec / NS_PER_MS;
}

/* 시드 고정 xorshift32: 실행마다 같은 나이와 같은 무작위 키를 쓴다 */
static uint32_t next_random(Bench *bench)
{
    uint32_t x = bench->random_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    bench->random_state = x;
    return x;
}

static int remove_entry(const char *path, const struct stat *info, int type, struct FTW *walk)
{
    (void)info;
    (void)type;
    (void)walk;
    return remove(path);
}

/* ---- 준비 ---- */

static void create_users_table(const Bench *bench)
{
    Statement stmt;
    ParseError error;
    if (parser_parse_single(USERS_SQL, strlen(USERS_SQL), &stmt, &error) != ERR_NONE)
        die("cannot parse schema: %s", error.message);
    ErrorCode created = table_create(bench->table_path, &stmt.create);
    statement_free(&stmt);
    if (created != ERR_NONE)
        die("cannot create %s: %s", bench->table_path, error_name(created));
}

static void open_engine(Bench *bench)
{
    ErrorCode error = ERR_NONE;
    char message[MAX_MESSAGE];
    bench->engine = engine_open(bench->dir, &error, message);
    if (bench->engine == NULL)
        die("cannot open engine: %s", message);
}

static void bench_setup(Bench *bench, int32_t row_count)
{
    memset(bench, 0, sizeof(*bench));
    snprintf(bench->dir, sizeof(bench->dir), "%s", TEMP_TEMPLATE);
    if (mkdtemp(bench->dir) == NULL)
        die("cannot create temp dir under /tmp");
    if (!table_build_path(bench->dir, USERS_TABLE, bench->table_path, sizeof(bench->table_path)))
        die("table path too long");
    bench->row_count = row_count;
    bench->random_state = RANDOM_SEED;
    create_users_table(bench);
    open_engine(bench);
}

static void bench_teardown(Bench *bench)
{
    engine_close(bench->engine);
    nftw(bench->dir, remove_entry, NFTW_OPEN_FDS, FTW_DEPTH | FTW_PHYS);
}

/* ---- 측정 도우미 ---- */

/* 테이블 파일의 페이지 캐시를 비운다 (콜드 측정용). 더러운 페이지는 버릴 수 없어서 먼저 디스크로 내린다.
 * 루트 권한 없이 되는 방법이라 컨테이너 안에서도 쓸 수 있다 */
static void drop_file_cache(const Bench *bench)
{
    int fd = open(bench->table_path, O_RDONLY);
    if (fd < 0)
        die("cannot open %s", bench->table_path);
    fdatasync(fd);
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    close(fd);
}

/* sql 을 한 번 실행하고 걸린 시간. 행 수나 접근 경로가 기대와 다르면 실패로 끝낸다 (T29-01) */
static double run_checked(Bench *bench, const char *sql, int64_t expected_rows,
                          bool expected_index)
{
    double start = now_ms();
    Result result = engine_execute(bench->engine, sql, NULL, 0);
    double elapsed = now_ms() - start;
    if (result.error != ERR_NONE)
        die("%s: %s", sql, result.message);
    if (result.row_count != expected_rows || result.used_index != expected_index)
        die("%s: got %lld rows (index %d), expected %lld (index %d)", sql,
            (long long)result.row_count, result.used_index, (long long)expected_rows,
            expected_index);
    result_free(&result);
    return elapsed;
}

static Timing measure(Bench *bench, const char *sql, int64_t expected_rows, bool is_index)
{
    Timing timing;
    drop_file_cache(bench);
    timing.cold_ms = run_checked(bench, sql, expected_rows, is_index);
    int repeat = is_index ? INDEX_REPEAT : SCAN_REPEAT;
    double total = 0;
    for (int i = 0; i < repeat; i++)
        total += run_checked(bench, sql, expected_rows, is_index);
    timing.warm_ms = total / repeat;
    return timing;
}

/* ---- 단계 ---- */

/* SQL 경로 (파싱 → 검증 → 실행 → 인덱스 등록) 로 N 건을 넣는다 */
static double insert_rows(Bench *bench)
{
    double start = now_ms();
    for (int32_t i = 1; i <= bench->row_count; i++) {
        int32_t age = MIN_AGE + (int32_t)(next_random(bench) % AGE_SPAN);
        bench->rare_age_count += age >= RARE_AGE;
        char sql[SQL_SIZE];
        snprintf(sql, sizeof(sql), "INSERT INTO users VALUES ('user%d', %d)", i, age);
        Result result = engine_execute(bench->engine, sql, NULL, 0);
        if (result.error != ERR_NONE)
            die("insert %d: %s", i, result.message);
        result_free(&result);
    }
    return now_ms() - start;
}

/* 닫았다 다시 열어 재구축 시간과 트리 높이를 잰다 */
static void reopen(Bench *bench, SizeSummary *summary)
{
    engine_close(bench->engine);
    double start = now_ms();
    open_engine(bench);
    double open_ms = now_ms() - start;
    const Table *table = engine_find_table(bench->engine, USERS_TABLE);
    if (table == NULL || table_record_count(table) != bench->row_count)
        die("reopened table has wrong row count");
    summary->rebuild_ms = table_index_build_ms(table);
    summary->height = btree_height(table_index(table));
    printf("- engine_open %.2f ms (index rebuild %.2f ms), tree height %d (BTREE_ORDER %d)\n\n",
           open_ms, summary->rebuild_ms, summary->height, BTREE_ORDER);
}

static void report_point_queries(Bench *bench)
{
    int32_t keys[] = {1, bench->row_count / 2, bench->row_count};
    printf("| k | id = k cold | id = k warm | name = 'userk' cold | name = 'userk' warm | "
           "warm 배율 |\n|---|---|---|---|---|---|\n");
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        char id_sql[SQL_SIZE];
        char name_sql[SQL_SIZE];
        snprintf(id_sql, sizeof(id_sql), "SELECT * FROM users WHERE id = %d", keys[i]);
        snprintf(name_sql, sizeof(name_sql), "SELECT * FROM users WHERE name = 'user%d'",
                 keys[i]);
        Timing by_id = measure(bench, id_sql, 1, true);
        Timing by_name = measure(bench, name_sql, 1, false);
        printf("| %d | %.4f ms | %.4f ms | %.2f ms | %.2f ms | %.0f× |\n", keys[i], by_id.cold_ms,
               by_id.warm_ms, by_name.cold_ms, by_name.warm_ms, by_name.warm_ms / by_id.warm_ms);
    }
    printf("\n");
}

/* 무작위 k 1,000 개 평균 (웜). 같은 키 목록을 두 경로에 쓴다 */
static void report_random_queries(Bench *bench, SizeSummary *summary)
{
    double index_total = 0;
    double scan_total = 0;
    for (int i = 0; i < RANDOM_KEY_COUNT; i++) {
        int32_t key = 1 + (int32_t)(next_random(bench) % (uint32_t)bench->row_count);
        char sql[SQL_SIZE];
        snprintf(sql, sizeof(sql), "SELECT * FROM users WHERE id = %d", key);
        index_total += run_checked(bench, sql, 1, true);
        snprintf(sql, sizeof(sql), "SELECT * FROM users WHERE name = 'user%d'", key);
        scan_total += run_checked(bench, sql, 1, false);
    }
    summary->random_index_ms = index_total / RANDOM_KEY_COUNT;
    summary->random_scan_ms = scan_total / RANDOM_KEY_COUNT;
    printf("- 무작위 k %d 개 평균: id = k %.4f ms, name = 'userk' %.2f ms (%.0f×)\n",
           RANDOM_KEY_COUNT, summary->random_index_ms, summary->random_scan_ms,
           summary->random_scan_ms / summary->random_index_ms);
}

static void report_range_queries(Bench *bench)
{
    int32_t start = bench->row_count / 2;
    int64_t id_rows = bench->row_count - start + 1;
    int64_t age_rows = bench->rare_age_count;
    char id_sql[SQL_SIZE];
    char age_sql[SQL_SIZE];
    snprintf(id_sql, sizeof(id_sql), "SELECT * FROM users WHERE id >= %d LIMIT %d", start,
             RANGE_LIMIT);
    snprintf(age_sql, sizeof(age_sql), "SELECT * FROM users WHERE age >= %d LIMIT %d", RARE_AGE,
             RANGE_LIMIT);
    Timing by_id = measure(bench, id_sql, id_rows < RANGE_LIMIT ? id_rows : RANGE_LIMIT, true);
    Timing by_age = measure(bench, age_sql, age_rows < RANGE_LIMIT ? age_rows : RANGE_LIMIT, false);
    printf("- 범위 `id >= %d LIMIT %d`: cold %.4f ms, warm %.4f ms\n", start, RANGE_LIMIT,
           by_id.cold_ms, by_id.warm_ms);
    printf("- 범위 `age >= %d LIMIT %d` (선형): cold %.2f ms, warm %.2f ms\n\n", RARE_AGE,
           RANGE_LIMIT, by_age.cold_ms, by_age.warm_ms);
}

static SizeSummary run_size(int32_t row_count)
{
    SizeSummary summary = {.row_count = row_count};
    Bench bench;
    bench_setup(&bench, row_count);
    printf("## N = %d\n\n", row_count);
    summary.insert_ms = insert_rows(&bench);
    printf("- INSERT %d 건: %.0f ms (%.0f 건/s)\n", row_count, summary.insert_ms,
           row_count / (summary.insert_ms / MS_PER_SECOND));
    reopen(&bench, &summary);
    report_point_queries(&bench);
    report_random_queries(&bench, &summary);
    report_range_queries(&bench);
    bench_teardown(&bench);
    return summary;
}

static void report_scale(const SizeSummary *summaries, int count)
{
    printf("## 규모별 요약 (무작위 k 평균, 웜)\n\n");
    printf("| N | INSERT 건/s | 재구축 ms | 높이 | id = k ms | name = 'userk' ms | 배율 |\n");
    printf("|---|---|---|---|---|---|---|\n");
    for (int i = 0; i < count; i++) {
        const SizeSummary *s = &summaries[i];
        printf("| %d | %.0f | %.2f | %d | %.4f | %.2f | %.0f× |\n", s->row_count,
               s->row_count / (s->insert_ms / MS_PER_SECOND), s->rebuild_ms, s->height,
               s->random_index_ms, s->random_scan_ms, s->random_scan_ms / s->random_index_ms);
    }
}

/* 인자가 없으면 기본 규모 3 개. 인자는 양의 정수 N 들 */
static int parse_sizes(int argc, char **argv, int32_t *out_sizes)
{
    if (argc <= 1) {
        memcpy(out_sizes, DEFAULT_SIZES, sizeof(DEFAULT_SIZES));
        return DEFAULT_SIZE_COUNT;
    }
    if (argc - 1 > MAX_SIZES)
        die("at most %d sizes", MAX_SIZES);
    for (int i = 1; i < argc; i++) {
        char *end = NULL;
        long size = strtol(argv[i], &end, 10);
        if (*end != '\0' || size < 2 || size > INT32_MAX - 1)
            die("invalid size '%s' (need an integer >= 2)", argv[i]);
        out_sizes[i - 1] = (int32_t)size;
    }
    return argc - 1;
}

int main(int argc, char **argv)
{
    int32_t sizes[MAX_SIZES];
    int count = parse_sizes(argc, argv, sizes);
    SizeSummary summaries[MAX_SIZES];
    printf("# bench_index (BTREE_ORDER %d, index repeat %d, scan repeat %d)\n\n", BTREE_ORDER,
           INDEX_REPEAT, SCAN_REPEAT);
    for (int i = 0; i < count; i++) {
        summaries[i] = run_size(sizes[i]);
        fflush(stdout);
    }
    report_scale(summaries, count);
    return EXIT_SUCCESS;
}
