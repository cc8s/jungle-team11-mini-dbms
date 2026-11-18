#!/bin/sh
# SQL 기능 테스트: tests/sql/*.sql 을 빈 데이터 디렉터리에서 ./minidb 로 실행하고
# 출력을 같은 이름의 .out 과 바이트 단위로 비교한다. 하나라도 다르면 실패.
set -u
fail=0
for sql in tests/sql/*.sql; do
    want="${sql%.sql}.out"
    dir=$(mktemp -d)
    ./minidb -s schema -d "$dir" "$sql" > "$dir/actual" 2>&1
    if diff -u "$want" "$dir/actual"; then
        echo "  ok   $sql"
    else
        echo "  FAIL $sql"
        fail=1
    fi
    rm -rf "$dir"
done
exit $fail
