CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -g -O2 -pthread -D_GNU_SOURCE -Isrc
LDFLAGS := -pthread

# ASan 빌드는 일반 빌드와 오브젝트를 섞지 않도록 디렉터리를 따로 쓴다
ASAN_FLAGS := -fsanitize=address -fno-omit-frame-pointer
ASAN_DIR   := build/asan

BIN       := minidb
SRCS      := $(wildcard src/*.c)
LIB_SRCS  := $(filter-out src/main.c, $(SRCS))
LIB_OBJS  := $(patsubst src/%.c, build/%.o, $(LIB_SRCS))
TEST_SRCS := $(wildcard tests/test_*.c)
TESTS     := $(patsubst tests/%.c, tests/bin/%, $(TEST_SRCS))

ASAN_LIB_OBJS := $(patsubst src/%.c, $(ASAN_DIR)/%.o, $(LIB_SRCS))
ASAN_TESTS    := $(patsubst tests/%.c, $(ASAN_DIR)/tests/%, $(TEST_SRCS))

.PHONY: all test test-asan run clean

all: $(BIN)

$(BIN): build/main.o $(LIB_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -MMD -c $< -o $@

build tests/bin $(ASAN_DIR) $(ASAN_DIR)/tests:
	mkdir -p $@

tests/bin/%: tests/%.c $(LIB_OBJS) | tests/bin
	$(CC) $(CFLAGS) -Itests -MMD $(filter %.c %.o, $^) -o $@ $(LDFLAGS)

$(ASAN_DIR)/%.o: src/%.c | $(ASAN_DIR)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -MMD -c $< -o $@

$(ASAN_DIR)/tests/%: tests/%.c $(ASAN_LIB_OBJS) | $(ASAN_DIR)/tests
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -Itests -MMD $(filter %.c %.o, $^) -o $@ \
		$(LDFLAGS) $(ASAN_FLAGS)

# 테스트 실행 파일을 하나씩 돌리고, 하나라도 실패하면 make 가 실패한다
test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

test-asan: $(ASAN_TESTS)
	@for t in $(ASAN_TESTS); do echo "== $$t"; ./$$t || exit 1; done

run: $(BIN)
	./$(BIN)

clean:
	rm -rf build tests/bin $(BIN)

-include $(wildcard build/*.d $(ASAN_DIR)/*.d tests/bin/*.d $(ASAN_DIR)/tests/*.d)
