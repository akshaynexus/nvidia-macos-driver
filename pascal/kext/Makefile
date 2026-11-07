# pascal/kext/Makefile — Linux-side checks for the GP106 kext tree.
#
# The kext itself is only built on macOS (scripts/build-macos.sh). This
# Makefile runs everything that can run elsewhere:
#
#   test_device_gate  the kext's decision logic (src/pg106_device_gate.c)
#                     + the core PMC decoder it calls (../core/src/pmc.c)
#   test_core_shim    all of pascal/core compiled through the kext's klibc
#                     headers (src/klibc/) and allocator (src/pg106_klibc.c,
#                     host backend), plus direct allocator tests
#   symbol-audit      no macOS-only symbols/includes in the portable sources
#   syntax            every TU of the Xcode target through clang
#                     -fsyntax-only against MacKernelSDK
#                     (scripts/kext-syntax-check.sh; skipped, loudly, when
#                     no SDK is available — PG106_SDK_FETCH=1 clones it)
#
# Targets: all, test, audit, syntax, clean. BUILD_DIR is overridable.

ROOT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O1 -g
CPPFLAGS += -I$(ROOT)/src -I$(ROOT)/../core/include

BUILD_DIR ?= build
# Same header environment as the kext: src/klibc shadows <stdlib.h> and
# <errno.h>; -fno-builtin matches what -mkernel implies for the kext build.
KLIBC_FLAGS := -DPG106_KLIBC_KERNEL_SIM -fno-builtin -I$(ROOT)/src/klibc

CORE_SRC  := $(wildcard $(ROOT)/../core/src/*.c)
CORE_HDR  := $(wildcard $(ROOT)/../core/include/pascal/*.h)
GATE_SRC  := $(ROOT)/src/pg106_device_gate.c
GATE_HDR  := $(ROOT)/src/pg106_device_gate.h
KLIBC_SRC := $(ROOT)/src/pg106_klibc.c
KLIBC_HDR := $(ROOT)/src/pg106_klibc.h $(wildcard $(ROOT)/src/klibc/*.h)

.PHONY: all test audit syntax clean

all: $(BUILD_DIR)/test_device_gate $(BUILD_DIR)/test_core_shim

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/test_device_gate: $(ROOT)/stub/test_device_gate.c $(GATE_SRC) $(GATE_HDR) $(ROOT)/../core/src/pmc.c $(ROOT)/../core/include/pascal/pmc.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(ROOT)/stub/test_device_gate.c $(GATE_SRC) $(ROOT)/../core/src/pmc.c

$(BUILD_DIR)/test_core_shim: $(ROOT)/stub/test_core_shim.c $(KLIBC_SRC) $(KLIBC_HDR) $(CORE_SRC) $(CORE_HDR) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(KLIBC_FLAGS) -o $@ $(ROOT)/stub/test_core_shim.c $(KLIBC_SRC) $(CORE_SRC)

test: all
	./$(BUILD_DIR)/test_device_gate
	./$(BUILD_DIR)/test_core_shim
	bash $(ROOT)/scripts/symbol-audit.sh
	@BUILD_DIR=$(abspath $(BUILD_DIR)) bash $(ROOT)/scripts/kext-syntax-check.sh; \
	rc=$$?; if [ $$rc -ne 0 ] && [ $$rc -ne 77 ]; then exit $$rc; fi

audit:
	bash $(ROOT)/scripts/symbol-audit.sh

syntax:
	BUILD_DIR=$(abspath $(BUILD_DIR)) bash $(ROOT)/scripts/kext-syntax-check.sh

clean:
	rm -rf $(BUILD_DIR)
