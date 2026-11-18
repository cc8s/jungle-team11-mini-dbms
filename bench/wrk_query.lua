-- wrk 부하 스크립트: 요청마다 무작위 값으로 POST /query 를 보낸다.
--   MODE=id    SELECT * FROM users WHERE id = <1..ROWS>          (인덱스를 타는 질의)
--   MODE=name  SELECT * FROM users WHERE name = 'user<1..ROWS>'  (늘 선형 탐색)
-- 사용: MODE=id ROWS=1000000 wrk -t4 -c32 -d10s -s bench/wrk_query.lua http://127.0.0.1:8080

local rows = tonumber(os.getenv("ROWS") or "1000000")
local mode = os.getenv("MODE") or "id"
local headers = { ["Content-Type"] = "application/json" }

function init(args)
  math.randomseed(os.time())
end

function request()
  local k = math.random(1, rows)
  local sql
  if mode == "name" then
    sql = "SELECT * FROM users WHERE name = 'user" .. k .. "'"
  else
    sql = "SELECT * FROM users WHERE id = " .. k
  end
  return wrk.format("POST", "/query", headers, '{"sql": "' .. sql .. '"}')
end
