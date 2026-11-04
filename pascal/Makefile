# pascal/Makefile — builds every tests/test_*.c against core/src + core/include.
#
# Wildcards on purpose (HANDOFF-PASCAL.md Phase 0): other agents drop new
# tests/test_*.c and core/src/*.c files in without editing this file.
#
# Targets:
#   all      build every test binary
#   test     build + run all test binaries, then the repo-wide python simtest
#   coverage rebuild with gcc --coverage and report on core/src (best-effort)
#   clean

ROOT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))/..

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O1 -g
CPPFLAGS += -Icore/include

BUILD_DIR := build
CORE_SRC  := $(wildcard core/src/*.c)
CORE_HDR  := $(wildcard core/include/pascal/*.h)
TEST_SRC  := $(wildcard tests/test_*.c)
TEST_BIN  := $(TEST_SRC:tests/%.c=$(BUILD_DIR)/%)

.PHONY: all test coverage clean

all: $(TEST_BIN)

# One self-contained binary per test file (each links the whole core).
$(BUILD_DIR)/%: tests/%.c $(CORE_SRC) $(CORE_HDR) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/$*.c $(CORE_SRC)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

test: all
	@set -e; for t in $(TEST_BIN); do echo "== $$t"; ./$$t; done
	python3 $(ROOT)/helios300-gp106-simtest.py

# Best-effort (gcc naming: each object's notes are build/<binary>-<src>.gcno,
# since every test binary compiles the core sources in). Each gcov run is
# per-binary; later runs overwrite earlier .gcov files for the same source —
# good enough to see what the suite exercises, not a merged report.
coverage:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) --coverage" all
	@set -e; for t in $(TEST_BIN); do ./$$t; done
	@set -e; for src in $(CORE_SRC); do \
		b=$$(basename $$src .c); \
		ls $(BUILD_DIR)/*-$$b.gcda >/dev/null 2>&1 && gcov -o $(BUILD_DIR) $(BUILD_DIR)/*-$$b.gcda; \
	done
	@grep -H -E "^(File|Lines)" *.gcov || true

clean:
	rm -rf $(BUILD_DIR)
	rm -f core/src/*.gcda core/src/*.gcno *.gcov core/src/*.gcov
