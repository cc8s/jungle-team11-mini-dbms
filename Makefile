CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -g -O2 -pthread -D_GNU_SOURCE -Isrc $(EXTRA_CFLAGS)
LDFLAGS := -pthread $(EXTRA_LDFLAGS)

# 목적 파일·테스트 실행 파일 위치. sanitizer 빌드는 따로 둬서 일반 빌드와 섞이지 않게 한다 (D007)
BUILD ?= build
TBIN  ?= tests/bin

BIN      := minidb
SRCS     := $(wildcard src/*.c)
LIB_SRCS := $(filter-out src/main.c, $(SRCS))
LIB_OBJS := $(patsubst src/%.c, $(BUILD)/%.o, $(LIB_SRCS))
TESTS    := $(patsubst tests/%.c, $(TBIN)/%, $(wildcard tests/test_*.c))

ASAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -O1
TSAN_FLAGS := -fsanitize=thread -O1

.PHONY: all test sqltest asan tsan run clean

all: $(BIN)

$(BIN): $(BUILD)/main.o $(LIB_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -c $< -o $@

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

# 같은 단위 테스트를 sanitizer 로 다시 빌드해 돌린다. ASan 과 TSan 은 같이 켤 수 없어 나눈다 (D007)
asan:
	$(MAKE) test BUILD=build/asan TBIN=tests/bin/asan EXTRA_CFLAGS="$(ASAN_FLAGS)" EXTRA_LDFLAGS="$(ASAN_FLAGS)"

tsan:
	$(MAKE) test BUILD=build/tsan TBIN=tests/bin/tsan EXTRA_CFLAGS="$(TSAN_FLAGS)" EXTRA_LDFLAGS="$(TSAN_FLAGS)"

run: $(BIN)
	./$(BIN)

clean:
	rm -rf build tests/bin $(BIN)

-include $(wildcard $(BUILD)/*.d)
