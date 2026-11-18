-- wrk 요청 스크립트: POST /query 로 읽기·쓰기를 섞어 보낸다.
--
--   wrk -t4 -c64 -d30s -s bench/post_query.lua http://localhost:8080/query \
--       -- <write_percent> <max_id> <read_kind>
--
-- write_percent  INSERT 비율 (0 ~ 100, 기본 10 = 읽기 90% / 쓰기 10%)
-- max_id         읽기 때 고르는 범위 1 ~ max_id (기본 100000, 준비한 데이터 수)
-- read_kind      id   = id 인덱스 조회 (SELECT * ... WHERE id = ?, 기본). 락 안에 있는 시간이 수 µs
--                scan = 이름으로 선형 탐색 (WHERE name = ?, 10 만 건 스캔). 락 안에 있는 시간이 수 ms
--
-- 쓰기는 INSERT. 값은 모두 params 로 보낸다 (바인딩).
-- 끝나면 상태 코드별 개수와 성공한 INSERT 수를 한 줄씩 출력한다. 측정 스크립트 (bench/run_load.sh) 가
-- 이 줄을 읽어 서버의 row_count 증가분과 비교한다 (T44-01).

local DEFAULT_WRITE_PERCENT = 10
local DEFAULT_MAX_ID = 100000
local PERCENT = 100
local SEED_BASE = 20261118
local MAX_AGE = 99

local threads = {}

-- 스레드마다 Lua 상태가 따로 있다. setup 은 메인 상태에서 스레드마다 한 번 불린다
function setup(thread)
    thread:set("thread_no", #threads + 1)
    table.insert(threads, thread)
end

function init(args)
    write_percent = tonumber(args[1]) or DEFAULT_WRITE_PERCENT
    max_id = tonumber(args[2]) or DEFAULT_MAX_ID
    read_kind = args[3] or "id"
    math.randomseed(SEED_BASE + thread_no) -- 매 측정마다 같은 요청 순서
    ok_inserts = 0
    status_200 = 0
    status_503 = 0
    status_other = 0
    wrk.method = "POST"
    wrk.headers["Content-Type"] = "application/json"
end

local function insert_body()
    local name = string.format("w%d_%d", thread_no, math.random(1, 1000000000))
    return string.format(
        '{"sql":"INSERT INTO users VALUES (?, ?)","params":["%s",%d]}',
        name, math.random(1, MAX_AGE))
end

local function select_body()
    if read_kind == "scan" then -- tools/gen_sql.py 의 이름은 user1 .. userN
        return string.format(
            '{"sql":"SELECT * FROM users WHERE name = ?","params":["user%d"]}',
            math.random(1, max_id))
    end
    return string.format(
        '{"sql":"SELECT * FROM users WHERE id = ?","params":[%d]}', math.random(1, max_id))
end

function request()
    local body
    if math.random(1, PERCENT) <= write_percent then
        body = insert_body()
    else
        body = select_body()
    end
    return wrk.format(nil, nil, nil, body)
end

-- 응답 순서와 요청 순서를 짝짓지 않는다 (연결이 여러 개라 어긋날 수 있다).
-- INSERT 성공 응답에만 last_insert_id 가 있으므로 본문으로 센다
function response(status, headers, body)
    if status == 200 then
        status_200 = status_200 + 1
        if body:find('"last_insert_id":', 1, true) then
            ok_inserts = ok_inserts + 1
        end
    elseif status == 503 then
        status_503 = status_503 + 1
    else
        status_other = status_other + 1
    end
end

function done(summary, latency, requests)
    local totals = { ok_inserts = 0, status_200 = 0, status_503 = 0, status_other = 0 }
    for _, thread in ipairs(threads) do
        for key, _ in pairs(totals) do
            totals[key] = totals[key] + thread:get(key)
        end
    end
    local seconds = summary.duration / 1e6
    io.write(string.format(
        "result duration_s=%.1f requests=%d rps=%.1f ok_rps=%.1f p50_ms=%.2f p99_ms=%.2f\n",
        seconds, summary.requests, summary.requests / seconds, totals.status_200 / seconds,
        latency:percentile(50) / 1000, latency:percentile(99) / 1000))
    io.write(string.format("result status_200=%d status_503=%d status_other=%d ok_inserts=%d\n",
        totals.status_200, totals.status_503, totals.status_other, totals.ok_inserts))
    io.write(string.format("result socket_errors connect=%d read=%d write=%d timeout=%d\n",
        summary.errors.connect, summary.errors.read, summary.errors.write,
        summary.errors.timeout))
end
