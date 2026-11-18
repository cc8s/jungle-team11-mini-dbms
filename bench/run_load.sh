#!/bin/bash
# Phase 3 부하 측정 (S44). 컨테이너 안에서 저장소 루트에서 실행한다.
#
#   bench/run_load.sh                 # 30 초씩 전체 시나리오
#   DURATION=5s bench/run_load.sh     # 빠른 확인
#
# 1. 두 락 방식 (GLOBAL, TABLE_RW) 으로 minidb 를 빌드해 임시 디렉터리에 둔다
# 2. 10 만 건 원본 데이터를 한 번 만들고, 측정마다 복사해서 같은 초기 상태로 시작한다
# 3. 서버를 띄우고 wrk 를 돌린 뒤 데이터를 확인한다 (T44-01)
#    - row_count 증가분 = 200 을 받은 INSERT 수 + 마감 순간 처리 중이던 INSERT (0 ~ 연결 수).
#      wrk 는 시간이 끝나면 응답을 기다리지 않고 연결을 끊는다. 서버는 이미 받은 요청을 끝까지 실행하므로
#      그 INSERT 는 저장되지만 wrk 는 세지 못한다
#    - 서버를 멈춘 뒤 다시 열 때 헤더 복구 경고가 없어야 한다 (열기는 위치 k 의 id 가 k 인지 검사한다)
# 결과는 마크다운 표 한 줄씩 출력한다. 데이터가 어긋나면 1 로 끝난다.

set -euo pipefail

DURATION=${DURATION:-30s}
PORT=${PORT:-18080}
ROWS=${ROWS:-100000}
WRK_THREADS=${WRK_THREADS:-4}
CONNECTIONS=${CONNECTIONS:-64}
THROUGHPUT_QUEUE=${THROUGHPUT_QUEUE:-256} # 연결 수보다 커서 처리량 측정에 503 이 섞이지 않는다
WORKER_COUNTS=${WORKER_COUNTS:-"1 2 4 8 16"}
LOCK_MODES="GLOBAL TABLE_RW"
URL="http://127.0.0.1:${PORT}"
HEALTH_TRIES=100
MAX_ATTEMPTS=3     # 측정이 늘어지면 (아래) 다시 잰다
STRETCH_LIMIT=1.2  # wrk 가 잰 시간이 DURATION 의 1.2 배를 넘으면 VM 이 멈췄던 것으로 본다

WORK=$(mktemp -d /tmp/minidb-load-XXXXXX)
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

build_binaries() {
    for mode in $LOCK_MODES; do
        make -s LOCK_MODE="$mode" minidb >/dev/null
        cp minidb "$WORK/minidb-$mode"
    done
    make -s minidb >/dev/null # 기본 (TABLE_RW) 빌드로 되돌린다
}

prepare_seed() {
    mkdir "$WORK/seed"
    "$WORK/minidb-TABLE_RW" -d "$WORK/seed" init schema/users.sql >/dev/null
    python3 tools/gen_sql.py "$ROWS" >"$WORK/bulk.sql"
    "$WORK/minidb-TABLE_RW" -d "$WORK/seed" -q "$WORK/bulk.sql" >/dev/null
}

row_count() {
    curl -s "$URL/tables" | grep -o '"row_count":[0-9]*' | cut -d: -f2
}

wait_for_health() {
    for _ in $(seq "$HEALTH_TRIES"); do
        if curl -s "$URL/health" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    echo "server did not start" >&2
    return 1
}

# start_server <mode> <workers> [queue]
start_server() {
    rm -rf "$WORK/data"
    cp -r "$WORK/seed" "$WORK/data"
    "$WORK/minidb-$1" -d "$WORK/data" serve "$PORT" "$2" ${3:-} >"$WORK/server.log" 2>&1 &
    SERVER_PID=$!
    wait_for_health
}

stop_server() {
    kill -INT "$SERVER_PID"
    wait "$SERVER_PID"
    SERVER_PID=""
}

field() { # field <name> <file>
    grep -o "\b$1=[0-9.]*" "$2" | head -1 | cut -d= -f2
}

# 멈춘 서버의 데이터를 CLI 로 다시 열어 본다. 헤더 복구 경고가 없으면 ok
reopen_is_clean() {
    echo "SELECT id FROM users WHERE id = 1;" >"$WORK/check.sql"
    ! "$WORK/minidb-$1" -d "$WORK/data" -q "$WORK/check.sql" 2>&1 | grep -q "recovered"
}

# check_data <mode> <inserted> <delta> <connections>
check_data() {
    local in_flight=$(($3 - $2))
    if [ "$in_flight" -lt 0 ] || [ "$in_flight" -gt "$4" ]; then
        echo "MISMATCH (row_count +$3)"
    elif ! reopen_is_clean "$1"; then
        echo "RECOVERED ON REOPEN"
    else
        echo "ok (+$in_flight in flight)"
    fi
}

# wrk 가 실제로 잰 시간이 DURATION 보다 크게 길면 true. 노트북 절전 등으로 Docker VM 이 잠깐 멈추면
# 그 시간까지 측정에 들어가 req/s 가 터무니없이 낮게 나온다 (요청 수는 정상)
is_stretched() {
    awk -v d="$(field duration_s "$WORK/wrk.txt")" -v want="${DURATION%s}" -v k="$STRETCH_LIMIT" \
        'BEGIN { exit !(d > want * k) }'
}

# measure <mode> <workers> <queue> <write_percent> <read_kind> <connections>
# 결과는 $WORK/wrk.txt, row_count 증가분은 DELTA
measure() {
    start_server "$1" "$2" "$3"
    local before after
    before=$(row_count)
    wrk -t"$WRK_THREADS" -c"$6" -d"$DURATION" -s bench/post_query.lua "$URL/query" \
        -- "$4" "$ROWS" "$5" >"$WORK/wrk.txt"
    after=$(row_count)
    curl -sf "$URL/health" >/dev/null # 부하 뒤에도 살아 있다
    stop_server
    DELTA=$((after - before))
}

# run_case <label> <mode> <workers> <queue> <write_percent> <read_kind> <connections>
run_case() {
    local label=$1 mode=$2 workers=$3 queue=$4 write=$5 read_kind=$6 conns=$7
    local attempt
    for attempt in $(seq "$MAX_ATTEMPTS"); do
        measure "$mode" "$workers" "$queue" "$write" "$read_kind" "$conns"
        is_stretched || break
        echo "  (retry: $label $mode $workers measured $(field duration_s "$WORK/wrk.txt")s)" >&2
    done

    local requests busy other inserted check
    requests=$(field requests "$WORK/wrk.txt")
    busy=$(field status_503 "$WORK/wrk.txt")
    other=$(field status_other "$WORK/wrk.txt")
    inserted=$(field ok_inserts "$WORK/wrk.txt")
    check=$(check_data "$mode" "$inserted" "$DELTA" "$conns")
    is_stretched && check="$check, STRETCHED $(field duration_s "$WORK/wrk.txt")s"
    [[ "$check" == ok* ]] || STATUS=1
    printf "| %s | %s | %s / %s | %s | %s | %s | %s | %s | %.1f%% | %s | %s | %s |\n" \
        "$label" "$mode" "$workers" "$queue" "$conns" \
        "$(field ok_rps "$WORK/wrk.txt")" "$(field rps "$WORK/wrk.txt")" \
        "$(field p50_ms "$WORK/wrk.txt")" "$(field p99_ms "$WORK/wrk.txt")" \
        "$(awk -v b="$busy" -v r="$requests" 'BEGIN { print (r > 0 ? 100 * b / r : 0) }')" \
        "$other" "$inserted" "$check"
}

STATUS=0
build_binaries
prepare_seed
echo "# cores=$(nproc) duration=$DURATION wrk_threads=$WRK_THREADS rows=$ROWS"
echo "| 시나리오 | 락 | 워커 / 큐 | 연결 | 200 req/s | 전체 req/s | p50 ms | p99 ms | 503 | 기타 | INSERT 200 | 데이터 검증 |"
echo "|---|---|---|---|---|---|---|---|---|---|---|---|"
Q=$THROUGHPUT_QUEUE
for workers in $WORKER_COUNTS; do # T44-02, T44-03, E44-03: id 조회 90% + INSERT 10%
    for mode in $LOCK_MODES; do
        run_case "id 읽기 90%" "$mode" "$workers" "$Q" 10 id "$CONNECTIONS"
    done
done
for mode in $LOCK_MODES; do # T44-03 보충: 락 안에 오래 머무는 읽기 (선형 스캔)
    run_case "스캔 읽기 100%" "$mode" 4 "$Q" 0 scan "$CONNECTIONS"
done
for mode in $LOCK_MODES; do # E44-02
    run_case "쓰기 100%" "$mode" 4 "$Q" 100 id "$CONNECTIONS"
done
run_case "과부하" TABLE_RW 2 2 10 id "$CONNECTIONS" # E44-01: 연결 64 > 워커 2 + 큐 2
exit "$STATUS"
