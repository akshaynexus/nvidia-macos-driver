# pascal/Makefile — builds every tests/test_*.c against core/src + core/include.
#
# Wildcards on purpose (HANDOFF-PASCAL.md Phase 0): other agents drop new
# tests/test_*.c and core/src/*.c files in without editing this file.
#
# Targets:
#   all      build every test binary
#   test     build + run all test binaries, the kext stub/audit/syntax
#            checks (kext/Makefile), the bridge suite (bridge/Makefile),
#            then the repo-wide python simtest
#   coverage build instrumented, run everything, merged gcov report
#            (scripts/coverage.sh writes research/evidence/PHASE0-COVERAGE.txt)
#   clean

ROOT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))/..

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O1 -g
CPPFLAGS += -Icore/include
# core/src/kdisp.c (the kernel side of the bridge wire protocol) includes
# the wire header, which lives with the bridge (MIT).
CPPFLAGS += -Ibridge/include

# BUILD_DIR overridable so scripts/coverage.sh can build in a private
# directory (siblings run concurrent makes on build/ — see coverage.sh).
BUILD_DIR ?= build
CORE_SRC  := $(wildcard core/src/*.c)
CORE_HDR  := $(wildcard core/include/pascal/*.h)
TEST_SRC  := $(wildcard tests/test_*.c)
TEST_BIN  := $(TEST_SRC:tests/%.c=$(BUILD_DIR)/%)

.PHONY: all test coverage clean harness

# Mesa codegen fixtures (pascal/tests/mesa_ref/, gitignored) are generated
# from the third_party/mesa submodule by scripts/gen_mesa_ref.sh — never
# committed. Regenerated here when missing so a fresh clone just works.
.PHONY: mesa-ref
mesa-ref:
	@if [ ! -d tests/mesa_ref ]; then \
		echo "== regenerating Mesa fixtures from third_party/mesa"; \
		./scripts/gen_mesa_ref.sh || { echo "FAIL: gen_mesa_ref.sh needs the mesa submodule:"; \
			echo "  git submodule update --init third_party/mesa"; exit 1; }; \
	fi

all: mesa-ref $(TEST_BIN)

# One self-contained binary per test file (each links the whole core).
$(BUILD_DIR)/%: tests/%.c $(CORE_SRC) $(CORE_HDR) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/$*.c $(CORE_SRC)

# test_kdisp runs the real userspace bridge over the loopback transport
# into the kernel-side dispatcher, so it links two bridge sources too (a
# GPL test binary; the bridge library itself stays MIT-only, see
# bridge/Makefile license-check).
BRIDGE_SRC := bridge/src/bridge.c bridge/src/transport_loopback.c
$(BUILD_DIR)/test_kdisp: tests/test_kdisp.c $(CORE_SRC) $(CORE_HDR) $(BRIDGE_SRC) $(wildcard bridge/include/*.h) bridge/src/frame_io.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_kdisp.c $(CORE_SRC) $(BRIDGE_SRC)

# Linux userspace hardware harness (never run by `make test` on a GPU:
# test only runs it against a fake sysfs tree, scripts/hwtest_smoke.sh).
$(BUILD_DIR)/pg106_hwtest: tools/pg106_hwtest.c $(CORE_SRC) $(CORE_HDR) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tools/pg106_hwtest.c $(CORE_SRC)

harness: $(BUILD_DIR)/pg106_hwtest

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Every $(TEST_BIN) entry contains a '/' (it is $(BUILD_DIR)/name), so it
# runs as a path as-is; prefixing ./ broke absolute BUILD_DIRs (.//tmp/...).
# The kext and bridge suites build into subdirectories of BUILD_DIR
# (absolute, because their makes run with -C).
test: all $(BUILD_DIR)/pg106_hwtest
	@set -e; for t in $(TEST_BIN); do echo "== $$t"; $$t; done
	./scripts/hwtest_smoke.sh $(BUILD_DIR)/pg106_hwtest
	$(MAKE) -C kext test BUILD_DIR=$(abspath $(BUILD_DIR))/kext
	$(MAKE) -C bridge test BUILD_DIR=$(abspath $(BUILD_DIR))/bridge
	python3 $(ROOT)/helios300-gp106-simtest.py

# Merged coverage over all test binaries (HANDOFF-PASCAL.md §6 Phase 0 gate).
# Every test binary compiles all of core/src into itself, so per-source
# .gcda files exist once per binary; plain gcov given several .gcda for one
# source does NOT accumulate (verified on GCC 16.2.1) — scripts/coverage.sh
# merges the counters with gcov-tool and writes the Phase 0 evidence report.
.PHONY: coverage-build
coverage-build:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) --coverage" all

coverage:
	./scripts/coverage.sh

clean:
	rm -rf $(BUILD_DIR)
	rm -f core/src/*.gcda core/src/*.gcno *.gcov core/src/*.gcov
