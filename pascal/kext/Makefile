# pascal/kext/Makefile — Linux-side stub harness for the GP106 kext tree.
#
# The kext itself can only be compiled on macOS (scripts/build-macos.sh,
# owner-run — HANDOFF-PASCAL.md §3). This Makefile builds and runs the parts
# that are pure C on Linux:
#
#   test_device_gate  the kext's own decision logic (src/pg106_device_gate.c)
#                     + the core PMC decoder it calls (../core/src/pmc.c)
#   test_core_shim    ALL of pascal/core compiled through the KEXT libc shim
#                     (src/pg106_klibc_shim.h + src/klibc/*.h) — the Linux
#                     stand-in for the kext's header environment, proving the
#                     core's libc surface is fully covered by the shim
#
# Targets:
#   all    build both test binaries
#   test   run them, then scripts/symbol-audit.sh (the Phase 3 gate)
#   audit  just the symbol audit
#   clean

ROOT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O1 -g
CPPFLAGS += -I$(ROOT)/src -I$(ROOT)/../core/include

BUILD_DIR := build
KEXT_SHIM_FLAGS := -DPG106_KLIBC_KERNEL_SIM \
                   -include $(ROOT)/src/pg106_klibc_shim.h \
                   -I$(ROOT)/src/klibc

CORE_SRC  := $(wildcard $(ROOT)/../core/src/*.c)
CORE_HDR  := $(wildcard $(ROOT)/../core/include/pascal/*.h)
GATE_SRC  := $(ROOT)/src/pg106_device_gate.c
GATE_HDR  := $(ROOT)/src/pg106_device_gate.h
SHIM_HDR  := $(ROOT)/src/pg106_klibc_shim.h $(wildcard $(ROOT)/src/klibc/*.h)

.PHONY: all test audit clean

all: $(BUILD_DIR)/test_device_gate $(BUILD_DIR)/test_core_shim

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/test_device_gate: $(ROOT)/stub/test_device_gate.c $(GATE_SRC) $(GATE_HDR) $(ROOT)/../core/src/pmc.c $(ROOT)/../core/include/pascal/pmc.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(ROOT)/stub/test_device_gate.c $(GATE_SRC) $(ROOT)/../core/src/pmc.c

$(BUILD_DIR)/test_core_shim: $(ROOT)/stub/test_core_shim.c $(SHIM_HDR) $(CORE_SRC) $(CORE_HDR) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(KEXT_SHIM_FLAGS) -o $@ $(ROOT)/stub/test_core_shim.c $(CORE_SRC)

test: all
	./$(BUILD_DIR)/test_device_gate
	./$(BUILD_DIR)/test_core_shim
	bash $(ROOT)/scripts/symbol-audit.sh

audit:
	bash $(ROOT)/scripts/symbol-audit.sh

clean:
	rm -rf $(BUILD_DIR)
