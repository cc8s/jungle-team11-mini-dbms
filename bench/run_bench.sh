#!/bin/bash
# 100만 건 벤치마크 (#8): 적재, 재시작(인덱스 재구성), 인덱스 켬·끔 질의, 메모리, HTTP 부하.
#   사용: bench/run_bench.sh [행 수] [wrk 초]       (레포 루트에서, 기본 1000000 행 · 10초)
# 결과는 표준 출력에 마크다운으로 찍는다. wrk 가 없으면 HTTP 부하는 건너뛴다.
set -eu

ROWS=${1:-1000000}
WRK_SECONDS=${2:-10}
WORK=$(mktemp -d)
DATA="$WORK/data"
trap 'rm -rf "$WORK"' EXIT

make -s minidb tools/gen_data

# "-- index lookup, examined 1 rows, 0.004 ms" 줄들의 ms 를 모아 중앙값·평균을 낸다
stats() {
    grep -E "^-- (index|full)" | sed -E 's/.* ([0-9.]+) ms$/\1/' | sort -n |
        awk '{a[NR]=$1; s+=$1} END {printf "%.4f | %.4f", a[int((NR+1)/2)], s/NR}'
}
examined() {
    grep -E "^-- (index|full)" | head -1 | sed -E 's/.*examined ([0-9]+) rows.*/\1/'
}

echo "## 환경"
echo
echo "- 행 수: $ROWS, CPU $(nproc)코어, $(uname -sr)"
echo "- 컴파일러: $(gcc --version | head -1), -O2"
echo

# ---- 1. 적재 -----------------------------------------------------------------------------------
./tools/gen_data "$ROWS" > "$WORK/load.sql"
start=$(date +%s.%N)
/usr/bin/time -f "%M" -o "$WORK/load_rss" ./minidb -d "$DATA" "$WORK/load.sql" > /dev/null
end=$(date +%s.%N)
load_s=$(awk -v a="$start" -v b="$end" 'BEGIN {print b - a}')
echo "## 1. 적재 (INSERT $ROWS 개, 한 프로세스)"
echo
echo "| 걸린 시간 | 초당 INSERT | 파일 크기 | 최대 메모리(RSS) |"
echo "|---|---|---|---|"
printf "| %.2f s | %.0f | %s | %s MB |\n" "$load_s" "$(awk -v n="$ROWS" -v s="$load_s" 'BEGIN {print n / s}')" \
    "$(du -h "$DATA/users.tbl" | cut -f1)" "$(( $(cat "$WORK/load_rss") / 1024 ))"
echo

# ---- 2. 재시작: 테이블 열기 = 파일 훑기 + B+ 트리 재구성 -----------------------------------------
echo "## 2. 재시작 시 인덱스 재구성 (D003)"
echo
echo "| 회차 | 테이블 열기 |"
echo "|---|---|"
for i in 1 2 3; do
    ms=$(echo "SELECT id FROM users WHERE id = 1;" | ./minidb -d "$DATA" -t |
        grep "table opened" | sed -E 's/.* in ([0-9.]+) ms.*/\1/')
    echo "| $i | $ms ms |"
done
echo

# ---- 3. 질의: 인덱스 켬 / 끔 ------------------------------------------------------------------
LOOKUPS=200
SCANS=20
awk -v n="$ROWS" -v k="$LOOKUPS" 'BEGIN { srand(7); for (i = 0; i < k; i++) printf "SELECT * FROM users WHERE id = %d;\n", int(rand() * n) + 1 }' > "$WORK/q_id.sql"
awk -v n="$ROWS" -v k="$SCANS" 'BEGIN { srand(8); for (i = 0; i < k; i++) { a = int(rand() * (n - 1000)) + 1; printf "SELECT * FROM users WHERE id BETWEEN %d AND %d;\n", a, a + 999 } }' > "$WORK/q_range.sql"
awk -v n="$ROWS" -v k="$SCANS" 'BEGIN { srand(9); for (i = 0; i < k; i++) printf "SELECT * FROM users WHERE name = '"'"'user%d'"'"';\n", int(rand() * n) + 1 }' > "$WORK/q_name.sql"

echo "## 3. 질의 시간 (테이블 열기 시간 제외, 한 프로세스에서 연속 실행)"
echo
echo "| 질의 | 횟수 | 인덱스 | 탐색 방법 | 확인한 행 | 중앙값 ms | 평균 ms |"
echo "|---|---|---|---|---|---|---|"
for q in id range name; do
    case $q in
        id) label="\`id = n\`"; count=$LOOKUPS ;;
        range) label="\`id BETWEEN a AND a+999\`"; count=$SCANS ;;
        name) label="\`name = 'user n'\`"; count=$SCANS ;;
    esac
    for mode in on off; do
        flag=""
        [ $mode = off ] && flag="-n"
        out=$(./minidb -d "$DATA" -t $flag "$WORK/q_$q.sql")
        plan=$(echo "$out" | grep -E "^-- (index|full)" | head -1 | sed -E 's/^-- ([a-z ]+),.*/\1/')
        echo "| $label | $count | $mode | $plan | $(echo "$out" | examined) | $(echo "$out" | stats) |"
    done
done
echo

# ---- 4. HTTP 부하 (wrk) ------------------------------------------------------------------------
if ! command -v wrk > /dev/null; then
    echo "## 4. HTTP 부하: wrk 가 없어 건너뜀"
    exit 0
fi
echo "## 4. HTTP 부하 (wrk -t4 -c32 -d${WRK_SECONDS}s, 서버 워커 8 · 큐 64, 요청마다 연결을 새로 맺음)"
echo
echo "| 질의 | 인덱스 | 초당 요청 | 평균 지연 | 99% 지연 | 오류 |"
echo "|---|---|---|---|---|---|"
for run in "id on" "id off" "name on"; do
    set -- $run
    mode=$1
    idx=$2
    flag=""
    [ "$idx" = off ] && flag="-n"
    ./minidb --server -p 0 -d "$DATA" -w 8 -q 64 $flag > "$WORK/server.log" 2>&1 &
    pid=$!
    for _ in $(seq 50); do
        grep -q "listening on port" "$WORK/server.log" 2>/dev/null && break
        sleep 0.1
    done
    port=$(grep -oE "port [0-9]+" "$WORK/server.log" | head -1 | cut -d' ' -f2)
    res=$(MODE=$mode ROWS=$ROWS wrk -t4 -c32 -d"${WRK_SECONDS}s" --latency -s bench/wrk_query.lua \
        "http://127.0.0.1:$port")
    kill -TERM $pid
    wait $pid || true
    rps=$(echo "$res" | awk '/Requests\/sec/ {print $2}')
    avg=$(echo "$res" | awk '/^    Latency/ {print $2}')
    p99=$(echo "$res" | awk '/ 99%/ {print $2}')
    errs=$(echo "$res" | awk '/Non-2xx|Socket errors/ {print}' | tr '\n' ' ')
    echo "| \`$mode\` | $idx | $rps | $avg | $p99 | ${errs:-0} |"
done
