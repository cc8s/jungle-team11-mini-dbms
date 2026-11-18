#!/bin/sh
# 스레드 안전성 금지 항목 점검 (계획서 4.3, CLAUDE.md). src/ 의 C 소스에서 다음을 찾는다:
#   1. 비재진입 함수 호출 (strtok, strerror, localtime 등 내부 정적 버퍼·상태를 쓰는 것)
#   2. 함수 안 static 변수
#   3. 파일 범위의 바뀌는 전역 변수 (const 가 아닌 static, 또는 static 이 아닌 변수 정의)
# 유일한 예외: cli.c 의 시그널 플래그 serve_stop_requested (핸들러는 인자를 받을 수 없다).
# 찾으면 그 줄을 보여 주고 1 로 끝난다.

SRC_DIR="${1:-src}"
ALLOWED='serve_stop_requested'
status=0

report() {
    if [ -n "$2" ]; then
        echo "banned: $1"
        echo "$2"
        status=1
    fi
}

NON_REENTRANT='strtok|strerror|asctime|ctime|gmtime|localtime|rand|srand|getenv|setlocale'
NON_REENTRANT="$NON_REENTRANT|inet_ntoa|gethostbyname|readdir_r"
calls=$(grep -nE "\\b($NON_REENTRANT)[[:space:]]*\\(" "$SRC_DIR"/*.c "$SRC_DIR"/*.h)
report "non-reentrant call" "$calls"

local_statics=$(grep -nE '^[[:space:]]+static[[:space:]]' "$SRC_DIR"/*.c "$SRC_DIR"/*.h)
report "static variable inside a function" "$local_statics"

# 괄호가 없는 줄 = 함수가 아닌 정의. const 와 허용된 플래그는 뺀다
file_statics=$(grep -nE '^static[[:space:]][^(]*(=|;)' "$SRC_DIR"/*.c "$SRC_DIR"/*.h |
    grep -v 'const' | grep -v "$ALLOWED")
report "mutable file-scope static" "$file_statics"

DEFINITION='^[A-Za-z_][A-Za-z0-9_ *]*[ *][A-Za-z_][A-Za-z0-9_]*(\[[^]]*\])*[[:space:]]*(=[^=].*)?;'
globals=$(grep -nE "$DEFINITION" "$SRC_DIR"/*.c |
    grep -vE ':(typedef|extern|static|struct [A-Za-z_]+;|return)' | grep -v '(')
report "global variable" "$globals"

[ "$status" -eq 0 ] && echo "check-banned: ok"
exit "$status"
