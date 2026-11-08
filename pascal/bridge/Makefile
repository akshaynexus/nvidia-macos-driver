# pascal/bridge/Makefile - builds the pg106 bridge static lib, the
# mock-transport test binary and the NVK-shaped demo.
#
# Targets:
#   all    build lib, tests, demo
#   test   build + run the mock-transport tests
#   demo   build + run the NVK-shaped sequence over the mock
#   clean
#
# The bridge is self-contained: C11 + libc, plus pascal/core's uapi
# header and validators (MIT, like the bridge - see README.md).

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O1 -g
CPPFLAGS += -Iinclude -I../core/include

BUILD_DIR ?= build

LIB_SRC   := src/bridge.c src/mock.c src/transport_loopback.c src/transport_iokit.c
LIB_OBJ   := $(LIB_SRC:src/%.c=$(BUILD_DIR)/%.o)
# The uapi validators (pg106_uapi_*) live in pascal/core/src/uapi.c -
# pure C11, no hardware, MIT. It is the only core object in the
# library; nothing GPL-2.0-only is linked (README.md, "Licensing").
CORE_UAPI_OBJ := $(BUILD_DIR)/core-uapi.o
# Demo-only: the demo packs its push words with the core encoders.
CORE_PUSHBUF_OBJ := $(BUILD_DIR)/core-pushbuf.o
LIB       := $(BUILD_DIR)/libpg106_bridge.a

TEST_BIN  := $(BUILD_DIR)/test_bridge
DEMO_BIN  := $(BUILD_DIR)/pg106_br_demo

.PHONY: all test demo clean license-check

all: $(LIB) $(TEST_BIN) $(DEMO_BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c $(wildcard include/*.h) src/frame_io.h ../core/include/pascal/uapi.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(CORE_UAPI_OBJ): ../core/src/uapi.c ../core/include/pascal/uapi.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(CORE_PUSHBUF_OBJ): ../core/src/pushbuf.c ../core/include/pascal/pushbuf.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(LIB): $(LIB_OBJ) $(CORE_UAPI_OBJ)
	ar rcs $@ $^

$(TEST_BIN): tests/test_bridge.c $(LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_bridge.c $(LIB)

$(DEMO_BIN): demo/pg106_br_demo.c $(LIB) $(CORE_PUSHBUF_OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ demo/pg106_br_demo.c $(LIB) \
		$(CORE_PUSHBUF_OBJ)

# $(TEST_BIN) always contains a '/', so it runs as a path for both the
# default relative BUILD_DIR and an absolute override.
# Licence boundary (README.md "Licensing"): every source that ends up in
# libpg106_bridge.a must be MIT. A GPL file here would make NVK's link a
# GPL derivative.
license-check:
	@for f in $(LIB_SRC) ../core/src/uapi.c $(wildcard include/*.h) src/frame_io.h; do \
		head -3 $$f | grep -q 'SPDX-License-Identifier: MIT' || \
			{ echo "FAIL licence: $$f is not MIT"; exit 1; }; \
	done; echo "license-check: OK — every libpg106_bridge.a input is MIT"

test: $(TEST_BIN) license-check
	$(TEST_BIN)
	@bash scripts/macos-syntax-check.sh; rc=$$?; \
	if [ $$rc -ne 0 ] && [ $$rc -ne 77 ]; then exit $$rc; fi

demo: $(DEMO_BIN)
	$(DEMO_BIN)

clean:
	rm -rf $(BUILD_DIR)
