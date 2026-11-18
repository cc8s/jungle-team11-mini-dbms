CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -g -O2 -pthread -D_GNU_SOURCE -Isrc
LDFLAGS := -pthread

# 락 방식: TABLE_RW (기본, 테이블마다 RW 락) 또는 GLOBAL (엔진 뮤텍스 하나). make LOCK_MODE=GLOBAL
# 바꾸면 build/lock_mode 의 내용이 바뀌어 그 값으로 컴파일한 오브젝트를 모두 다시 만든다
LOCK_MODES := GLOBAL TABLE_RW
LOCK_MODE  ?= TABLE_RW
ifeq ($(filter $(LOCK_MODE), $(LOCK_MODES)),)
$(error LOCK_MODE must be one of: $(LOCK_MODES))
endif
CFLAGS     += -DLOCK_MODE_$(LOCK_MODE)
LOCK_STAMP := build/lock_mode

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

# TSan 빌드 (데이터 레이스 검사). ASan 과 같이 쓸 수 없어서 디렉터리를 따로 쓴다
TSAN_FLAGS    := -fsanitize=thread -fno-omit-frame-pointer
TSAN_DIR      := build/tsan
TSAN_LIB_OBJS := $(patsubst src/%.c, $(TSAN_DIR)/%.o, $(LIB_SRCS))
TSAN_TESTS    := $(patsubst tests/%.c, $(TSAN_DIR)/tests/%, $(TEST_SRCS))

# 두 락 방식 모두 전체 테스트를 통과해야 한다 (S40). 기본 빌드와 별개로 GLOBAL 빌드를 따로 둔다
GLOBAL_CFLAGS   := $(filter-out -DLOCK_MODE_%, $(CFLAGS)) -DLOCK_MODE_GLOBAL
GLOBAL_DIR      := build/global
GLOBAL_LIB_OBJS := $(patsubst src/%.c, $(GLOBAL_DIR)/%.o, $(LIB_SRCS))
GLOBAL_TESTS    := $(patsubst tests/%.c, $(GLOBAL_DIR)/tests/%, $(TEST_SRCS))

# 작은 차수로 test_btree 를 한 번 더 빌드한다. split 이 자주 일어나 경계 경로를 많이 밟는다.
# 차수가 바뀌면 노드 크기가 바뀌므로 트리 소스만 따로 컴파일하고, 메모리 오류를 잡도록 ASan 을 켠다.
SMALL_ORDERS := 3 4
ORDER_TESTS  := $(foreach order, $(SMALL_ORDERS), build/order$(order)/test_btree)
BTREE_DEPS   := tests/test_btree.c tests/test.h src/btree.c src/btree.h src/btree_internal.h

# 100 만 건 같은 느린 테스트는 이 환경 변수가 1 일 때만 돈다 (make test-slow)
SLOW_TESTS_ENV := MINIDB_SLOW_TESTS=1

# 벤치마크: 일반 빌드 (-O2) 오브젝트로 링크한다. 데이터는 bench 가 /tmp 아래에 만든다
BENCH       := build/bench/bench_index
BENCH_SIZES ?=

.PHONY: all test test-asan test-tsan test-global test-order test-slow bench run clean FORCE

# 테스트 링크에만 쓰이는 오브젝트를 make 가 중간 파일로 보고 지우지 않게 한다 (매번 다시 빌드 방지)
.SECONDARY: $(LIB_OBJS) $(ASAN_LIB_OBJS) $(TSAN_LIB_OBJS) $(GLOBAL_LIB_OBJS)

all: $(BIN)

$(BIN): build/main.o $(LIB_OBJS)
	$(CC) $(filter %.o, $^) -o $@ $(LDFLAGS)

# 내용이 같으면 파일을 건드리지 않으므로 mtime 이 그대로고, 오브젝트도 다시 만들지 않는다
$(LOCK_STAMP): FORCE | build
	@echo $(LOCK_MODE) | cmp -s - $@ || echo $(LOCK_MODE) > $@

build/%.o: src/%.c $(LOCK_STAMP) | build
	$(CC) $(CFLAGS) -MMD -c $< -o $@

build tests/bin $(ASAN_DIR) $(ASAN_DIR)/tests $(TSAN_DIR) $(TSAN_DIR)/tests \
		$(GLOBAL_DIR) $(GLOBAL_DIR)/tests:
	mkdir -p $@

tests/bin/%: tests/%.c $(LIB_OBJS) $(LOCK_STAMP) | tests/bin
	$(CC) $(CFLAGS) -Itests -MMD $(filter %.c %.o, $^) -o $@ $(LDFLAGS)

$(ASAN_DIR)/%.o: src/%.c $(LOCK_STAMP) | $(ASAN_DIR)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -MMD -c $< -o $@

$(ASAN_DIR)/tests/%: tests/%.c $(ASAN_LIB_OBJS) | $(ASAN_DIR)/tests
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -Itests -MMD $(filter %.c %.o, $^) -o $@ \
		$(LDFLAGS) $(ASAN_FLAGS)

$(TSAN_DIR)/%.o: src/%.c $(LOCK_STAMP) | $(TSAN_DIR)
	$(CC) $(CFLAGS) $(TSAN_FLAGS) -MMD -c $< -o $@

$(TSAN_DIR)/tests/%: tests/%.c $(TSAN_LIB_OBJS) | $(TSAN_DIR)/tests
	$(CC) $(CFLAGS) $(TSAN_FLAGS) -Itests -MMD $(filter %.c %.o, $^) -o $@ \
		$(LDFLAGS) $(TSAN_FLAGS)

$(GLOBAL_DIR)/%.o: src/%.c | $(GLOBAL_DIR)
	$(CC) $(GLOBAL_CFLAGS) -MMD -c $< -o $@

$(GLOBAL_DIR)/tests/%: tests/%.c $(GLOBAL_LIB_OBJS) | $(GLOBAL_DIR)/tests
	$(CC) $(GLOBAL_CFLAGS) -Itests -MMD $(filter %.c %.o, $^) -o $@ $(LDFLAGS)

build/order%/test_btree: $(BTREE_DEPS)
	mkdir -p $(@D)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) -DBTREE_ORDER=$* -Itests tests/test_btree.c src/btree.c -o $@ \
		$(LDFLAGS) $(ASAN_FLAGS)

# 테스트 실행 파일을 하나씩 돌리고, 하나라도 실패하면 make 가 실패한다
test: $(TESTS) test-order
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

test-asan: $(ASAN_TESTS) test-order
	@for t in $(ASAN_TESTS); do echo "== $$t"; ./$$t || exit 1; done

# 전체 테스트를 ThreadSanitizer 로 다시 돌린다. 레이스가 보고되면 실패 (halt_on_error)
test-tsan: $(TSAN_TESTS)
	@for t in $(TSAN_TESTS); do echo "== $$t"; TSAN_OPTIONS=halt_on_error=1 ./$$t || exit 1; done

# 전체 테스트를 LOCK_MODE=GLOBAL 빌드로 다시 돌린다 (기본 make test 는 TABLE_RW)
test-global: $(GLOBAL_TESTS)
	@for t in $(GLOBAL_TESTS); do echo "== $$t"; ./$$t || exit 1; done

test-order: $(ORDER_TESTS)
	@for t in $(ORDER_TESTS); do echo "== $$t"; ./$$t || exit 1; done

# 느린 테스트를 일반 빌드 (-O2) 와 ASan 빌드 (누수 검사 포함) 에서 모두 돌린다
test-slow: tests/bin/test_btree $(ASAN_DIR)/tests/test_btree
	@for t in $^; do echo "== $$t (slow)"; $(SLOW_TESTS_ENV) ./$$t || exit 1; done

$(BENCH): bench/bench_index.c $(LIB_OBJS) $(LOCK_STAMP)
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD $(filter %.c %.o, $^) -o $@ $(LDFLAGS)

# make bench BENCH_SIZES="10000 100000" 로 규모를 바꿀 수 있다 (기본: 1만, 10만, 100만)
bench: $(BENCH)
	./$(BENCH) $(BENCH_SIZES)

run: $(BIN)
	./$(BIN)

clean:
	rm -rf build tests/bin $(BIN)

-include $(wildcard build/*.d $(ASAN_DIR)/*.d tests/bin/*.d $(ASAN_DIR)/tests/*.d $(TSAN_DIR)/*.d $(TSAN_DIR)/tests/*.d \
	$(GLOBAL_DIR)/*.d $(GLOBAL_DIR)/tests/*.d \
	build/bench/*.d)
