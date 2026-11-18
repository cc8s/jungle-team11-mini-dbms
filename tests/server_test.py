#!/usr/bin/env python3
"""서버 통합 테스트 (TC-SRV). 실제로 ./minidb --server 를 띄워 HTTP 로 요청한다.

Python 표준 라이브러리만 쓴다. 레포 루트에서 실행: python3 tests/server_test.py
"""

import http.client
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

BIN = os.environ.get("MINIDB_BIN", "./minidb")  # sanitizer 로 빌드한 서버를 넣어 돌릴 수 있다
failures = []


def check(cond, what):
    if cond:
        print(f"  ok   {what}")
    else:
        print(f"  FAIL {what}")
        failures.append(what)


class Server:
    def __init__(self, data_dir, *args):
        self.proc = subprocess.Popen(
            [BIN, "--server", "-p", "0", "-d", data_dir, *args],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        line = self.proc.stdout.readline()
        if "listening on port" not in line:
            raise RuntimeError(f"server did not start: {line!r}")
        self.port = int(line.split("port")[1].split()[0])

    def request(self, method, path, body=None, timeout=10):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)
        headers = {"Content-Type": "application/json"} if body is not None else {}
        conn.request(method, path, body=body, headers=headers)
        resp = conn.getresponse()
        data = resp.read()
        conn.close()
        return resp.status, json.loads(data) if data else None

    def sql(self, text):
        return self.request("POST", "/query", json.dumps({"sql": text}))

    def stop(self):
        """SIGTERM 으로 끄고 종료 코드를 확인한다. TSan 빌드는 레이스를 보고하면 코드 66 으로 끝난다"""
        self.proc.send_signal(signal.SIGTERM)
        out, _ = self.proc.communicate(timeout=15)
        code = self.proc.returncode
        if code != 0:
            print(out)
        check(code == 0, f"서버 종료 코드 0 (받은 값 {code})")
        return code, out


def test_basic(data_dir):
    print("TC-SRV-001 기본 요청과 오류 상태 코드")
    s = Server(data_dir, "-w", "4", "-q", "16")
    try:
        check(s.request("GET", "/health") == (200, {"ok": True}), "GET /health → 200")
        st, j = s.sql("INSERT INTO users VALUES ('kim', 'k@x', 20)")
        check(st == 200 and j["inserted_id"] == 1, "INSERT → 200, inserted_id 1")
        st, j = s.sql("SELECT * FROM users WHERE id = 1")
        check(st == 200 and j["rows"] == [[1, "kim", "k@x", 20]] and j["plan"] == "index lookup",
              "SELECT id = 1 → rows, index lookup")
        st, j = s.sql("SELECT * FORM users")
        check(st == 400 and "near 'FORM'" in j["error"], "구문 오류 → 400")
        st, j = s.request("POST", "/query", "not json")
        check(st == 400 and not j["ok"], "JSON 아님 → 400")
        st, _ = s.request("GET", "/query")
        check(st == 405, "GET /query → 405")
        st, _ = s.request("GET", "/nope")
        check(st == 404, "없는 경로 → 404")
        st, _ = s.request("POST", "/query", "x" * (1024 * 1024 + 1))
        check(st == 413, "본문 1MB 초과 → 413")
    finally:
        code, out = s.stop()
    check(code == 0 and "stopped" in out, "SIGTERM → 정상 종료 (코드 0)")


def test_concurrent_clients(data_dir):
    print("TC-SRV-002 동시 클라이언트 200개: INSERT 와 SELECT 를 섞어도 id 중복·누락 없음")
    s = Server(data_dir, "-w", "8", "-q", "256")
    results = []
    lock = threading.Lock()

    def client(i):
        if i % 2 == 0:
            st, j = s.sql(f"INSERT INTO users (name, age) VALUES ('c{i}', {i})")
        else:
            st, j = s.sql("SELECT id FROM users WHERE id BETWEEN 1 AND 50")
        with lock:
            results.append((i, st, j))

    try:
        threads = [threading.Thread(target=client, args=(i,)) for i in range(200)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        statuses = [st for _, st, _ in results]
        check(statuses.count(200) == 200, f"200개 모두 200 (받은 상태: {sorted(set(statuses))})")
        ids = sorted(j["inserted_id"] for i, st, j in results if i % 2 == 0 and st == 200)
        check(ids == list(range(2, 102)), "INSERT 100개의 id 가 2..101 로 빠짐·중복 없음")
        st, j = s.sql("SELECT id FROM users")
        check(st == 200 and j["row_count"] == 101, "전체 행 수 101 (기존 1 + 100)")
    finally:
        s.stop()


def test_busy_503(data_dir):
    print("TC-SRV-003 큐가 가득 차면 503 (워커 1, 큐 1) — 학습 노트 2 Q12")
    s = Server(data_dir, "-w", "1", "-q", "1")
    idle = []
    try:
        # 아무것도 보내지 않는 연결 둘: 하나는 워커를 잡고(읽기 대기), 하나는 큐 한 칸을 채운다
        for _ in range(2):
            idle.append(socket.create_connection(("127.0.0.1", s.port)))
            time.sleep(0.2)
        start = time.monotonic()
        st, j = s.request("GET", "/health", timeout=3)
        took = time.monotonic() - start
        check(st == 503 and "busy" in j["error"], "세 번째 요청 → 503")
        check(took < 1.0, f"503 은 기다리지 않고 바로 온다 ({took * 1000:.0f}ms)")
    finally:
        for sock in idle:
            sock.close()
        time.sleep(0.3)
        code, out = s.stop()
    check("rejected busy 1" in out, "종료 통계에 거절 1건")


def test_restart_keeps_data(data_dir):
    print("TC-SRV-004 다시 띄우면 데이터와 id 가 이어진다 (SIGTERM 때 flush)")
    s = Server(data_dir)
    try:
        st, j = s.sql("SELECT id FROM users")
        check(st == 200 and j["row_count"] == 101, "재시작 후 행 101")
        st, j = s.sql("INSERT INTO users (name) VALUES ('after')")
        check(j.get("inserted_id") == 102, "다음 id 102")
    finally:
        s.stop()


def main():
    if not os.path.exists(BIN):
        print("build first: make")
        return 2
    with tempfile.TemporaryDirectory() as d:
        test_basic(d)
        test_concurrent_clients(d)
        test_busy_503(d)
        test_restart_keeps_data(d)
    print(f"{'FAILED' if failures else 'ok'}: {len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
