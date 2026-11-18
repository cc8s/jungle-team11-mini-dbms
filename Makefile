CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -g -O2 -pthread -D_GNU_SOURCE -Isrc -Ithird_party $(EXTRA_CFLAGS)
LDFLAGS := -pthread $(EXTRA_LDFLAGS)

# 목적 파일·테스트 실행 파일 위치. sanitizer 빌드는 따로 둬서 일반 빌드와 섞이지 않게 한다 (D007)
BUILD ?= build
TBIN  ?= tests/bin

BIN      := minidb
SRCS     := $(wildcard src/*.c)
LIB_SRCS := $(filter-out src/main.c, $(SRCS))
LIB_OBJS := $(patsubst src/%.c, $(BUILD)/%.o, $(LIB_SRCS)) $(BUILD)/cJSON.o
TESTS    := $(patsubst tests/%.c, $(TBIN)/%, $(wildcard tests/test_*.c))

ASAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -O1
TSAN_FLAGS := -fsanitize=thread -O1

.PHONY: all test sqltest servertest servertest-tsan asan tsan bench run clean

all: $(BIN)

$(BIN): $(BUILD)/main.o $(LIB_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -c $< -o $@

# 외부 라이브러리(MIT, D006). 우리 코드가 아니라 경고 옵션은 그대로 두고 따로 빌드한다
$(BUILD)/cJSON.o: third_party/cJSON.c third_party/cJSON.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD) $(TBIN):
	mkdir -p $@

$(TBIN)/%: tests/%.c $(LIB_OBJS) | $(TBIN)
	$(CC) $(CFLAGS) -Itests $^ -o $@ $(LDFLAGS)

# 테스트 실행 파일을 하나씩 돌리고, 하나라도 실패하면 make 가 실패한다
test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

# SQL 기능 테스트: tests/sql/*.sql 실행 결과를 .out 과 비교
sqltest: $(BIN)
	@sh tests/run_sql_tests.sh

# 서버 통합 테스트: 실제로 서버를 띄워 HTTP 로 요청을 보낸다 (Python 표준 라이브러리만 사용)
servertest: $(BIN)
	python3 tests/server_test.py

# 같은 통합 테스트를 TSan 으로 빌드한 서버에 돌린다. 동시 요청 경로의 레이스는 단위 테스트로는 안 잡힌다
# (cJSON 전역 오류 변수 레이스를 이 방법으로 찾았다)
servertest-tsan:
	$(MAKE) BUILD=build/tsan EXTRA_CFLAGS="$(TSAN_FLAGS)" EXTRA_LDFLAGS="$(TSAN_FLAGS)" build/tsan/minidb
	MINIDB_BIN=build/tsan/minidb python3 tests/server_test.py

build/tsan/minidb: $(BUILD)/main.o $(LIB_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

# 같은 단위 테스트를 sanitizer 로 다시 빌드해 돌린다. ASan 과 TSan 은 같이 켤 수 없어 나눈다 (D007)
asan:
	$(MAKE) test BUILD=build/asan TBIN=tests/bin/asan EXTRA_CFLAGS="$(ASAN_FLAGS)" EXTRA_LDFLAGS="$(ASAN_FLAGS)"

tsan:
	$(MAKE) test BUILD=build/tsan TBIN=tests/bin/tsan EXTRA_CFLAGS="$(TSAN_FLAGS)" EXTRA_LDFLAGS="$(TSAN_FLAGS)"

# 100만 건 생성기 (외부 의존 없음)
tools/gen_data: tools/gen_data.c
	$(CC) -std=c11 -Wall -Wextra -O2 $< -o $@

# 100만 건 벤치마크 (#8): make bench ROWS=1000000
ROWS ?= 1000000
bench: $(BIN) tools/gen_data
	bash bench/run_bench.sh $(ROWS)

run: $(BIN)
	./$(BIN)

clean:
	rm -rf build tests/bin $(BIN) tools/gen_data

-include $(wildcard $(BUILD)/*.d)
