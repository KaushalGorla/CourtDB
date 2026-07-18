# =============================================================================
# CourtDB - Build System
# =============================================================================
#
# Usage:
#   make          - Build the library
#   make test     - Build and run all tests
#   make bench    - Build benchmarks
#   make clean    - Remove build artifacts
#   make all      - Build everything
#
# =============================================================================

CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 \
            -Wnull-dereference -Wno-unused-parameter
INCLUDES := -I include

# Build mode (debug by default)
BUILD ?= debug
ifeq ($(BUILD),release)
    CXXFLAGS += -O2 -DNDEBUG
else
    CXXFLAGS += -g -O0
endif

# Directories
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
TEST_DIR := $(BUILD_DIR)/tests
BENCH_DIR := $(BUILD_DIR)/benchmarks
LIB_DIR := $(BUILD_DIR)/lib

# =============================================================================
# Source files
# =============================================================================

LIB_SRCS := \
    src/storage/page.cpp \
    src/storage/record.cpp \
    src/storage/heap_file.cpp \
    src/disk/disk_manager.cpp \
    src/buffer/buffer_pool_manager.cpp \
    src/index/btree.cpp \
    src/query/executor.cpp \
    src/recovery/log_record.cpp \
    src/recovery/log_manager.cpp \
    src/recovery/recovery_manager.cpp

LIB_OBJS := $(patsubst src/%.cpp,$(OBJ_DIR)/%.o,$(LIB_SRCS))
LIB_TARGET := $(LIB_DIR)/libcourtdb.a

# =============================================================================
# Test files (each test is a standalone executable with its own main)
# =============================================================================

TEST_SRCS := \
    tests/storage/page_test.cpp \
    tests/storage/heap_file_test.cpp \
    tests/disk/disk_manager_test.cpp \
    tests/buffer/buffer_pool_manager_test.cpp \
    tests/index/btree_test.cpp \
    tests/query/executor_test.cpp \
    tests/recovery/recovery_test.cpp

TEST_BINS := $(patsubst tests/%.cpp,$(TEST_DIR)/%,$(TEST_SRCS))

# =============================================================================
# GoogleTest (downloaded and built locally)
# =============================================================================

GTEST_DIR := $(BUILD_DIR)/googletest
GTEST_SRC := $(GTEST_DIR)/googletest/src/gtest-all.cc
GTEST_MAIN := $(GTEST_DIR)/googletest/src/gtest_main.cc
GTEST_OBJ := $(OBJ_DIR)/gtest-all.o
GTEST_MAIN_OBJ := $(OBJ_DIR)/gtest_main.o
GTEST_INCLUDES := -I $(GTEST_DIR)/googletest/include -I $(GTEST_DIR)/googletest

# =============================================================================
# Targets
# =============================================================================

.PHONY: all lib test bench clean run_tests

all: lib test

lib: $(LIB_TARGET)

test: $(TEST_BINS) run_tests

bench: # TODO: add benchmark targets

clean:
	rm -rf $(BUILD_DIR)

run_tests: $(TEST_BINS)
	@echo "=========================================="
	@echo " Running CourtDB Tests"
	@echo "=========================================="
	@failures=0; \
	for test in $(TEST_BINS); do \
		echo "\n--- Running: $$test ---"; \
		$$test --gtest_color=yes || failures=$$((failures + 1)); \
	done; \
	if [ $$failures -ne 0 ]; then \
		echo "\n$$failures test(s) FAILED"; \
		exit 1; \
	else \
		echo "\nAll tests PASSED"; \
	fi

# =============================================================================
# Library
# =============================================================================

$(LIB_TARGET): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	ar rcs $@ $^

$(OBJ_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# =============================================================================
# GoogleTest Download & Build
# =============================================================================

$(GTEST_DIR):
	@echo "Downloading GoogleTest..."
	@mkdir -p $(GTEST_DIR)
	@git clone --depth 1 --branch v1.14.0 \
		https://github.com/google/googletest.git $(GTEST_DIR) 2>/dev/null || true

$(GTEST_OBJ): $(GTEST_DIR)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(GTEST_INCLUDES) -c $(GTEST_SRC) -o $@

$(GTEST_MAIN_OBJ): $(GTEST_DIR)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(GTEST_INCLUDES) -c $(GTEST_MAIN) -o $@

# =============================================================================
# Test Binaries
# =============================================================================

$(TEST_DIR)/%: tests/%.cpp $(LIB_TARGET) $(GTEST_OBJ) $(GTEST_MAIN_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(GTEST_INCLUDES) \
		$< $(GTEST_OBJ) $(GTEST_MAIN_OBJ) $(LIB_TARGET) \
		-lpthread -lstdc++fs -o $@
