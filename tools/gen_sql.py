#!/usr/bin/env python3
"""대량 INSERT SQL 생성기.

    python3 tools/gen_sql.py N [--table users] [--seed 42] > data/bulk.sql

`INSERT INTO <table> VALUES ('user<i>', <age>);` 를 i = 1..N 까지 한 줄씩 쓴다.
- 이름은 user1 .. userN 으로 모두 달라서 `WHERE name = 'userK'` 결과가 정확히 1 건이다
  (같은 파일을 두 번 실행하면 중복된다: 이름에는 유일성 제약이 없다).
- 나이는 시드를 고정한 난수 (1 ~ 99) 라서 같은 인자면 항상 같은 파일이 나온다.
- 테이블은 (id INT, name VARCHAR(n >= 7 + 자릿수), age INT) 스키마를 가정한다 (schema/users.sql).
"""

import argparse
import random
import sys

DEFAULT_TABLE = "users"
DEFAULT_SEED = 42
MIN_AGE = 1
MAX_AGE = 99
WRITE_CHUNK_LINES = 10000  # 줄을 모아서 쓰면 100 만 줄도 몇 초 안에 끝난다


def parse_args(argv):
    parser = argparse.ArgumentParser(description="generate bulk INSERT statements")
    parser.add_argument("count", type=int, help="number of INSERT statements (>= 0)")
    parser.add_argument("--table", default=DEFAULT_TABLE, help="table name (default: users)")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED, help="random seed for ages")
    args = parser.parse_args(argv)
    if args.count < 0:
        parser.error("count must be >= 0")
    return args


def generate_lines(count, table, seed):
    rng = random.Random(seed)
    for i in range(1, count + 1):
        age = rng.randint(MIN_AGE, MAX_AGE)
        yield f"INSERT INTO {table} VALUES ('user{i}', {age});\n"


def write_lines(lines, out):
    chunk = []
    for line in lines:
        chunk.append(line)
        if len(chunk) == WRITE_CHUNK_LINES:
            out.write("".join(chunk))
            chunk.clear()
    out.write("".join(chunk))


def main(argv):
    args = parse_args(argv)
    write_lines(generate_lines(args.count, args.table, args.seed), sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
