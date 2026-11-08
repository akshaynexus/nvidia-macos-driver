# pascal/fb/Makefile — Linux-side checks for PG106FB.kext (the GP106 HDMI
# IOFramebuffer). The display logic itself is tested in pascal/tests
# (test_disp_hw against the mock display engine); this builds the kext
# against the macOS SDK and audits its imports (scripts/cross-build-macos.sh).
# Exit 77 from the script (SDK / ld64 / xnu config missing) is a loud skip.

ROOT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
BUILD_DIR ?= $(ROOT)/build

.PHONY: all test cross clean
all: test
test cross:
	@BUILD_DIR=$(abspath $(BUILD_DIR))/cross bash $(ROOT)/scripts/cross-build-macos.sh; \
	rc=$$?; if [ $$rc -eq 77 ]; then echo "fb: cross-build SKIPPED (tools missing)"; \
	elif [ $$rc -ne 0 ]; then exit $$rc; fi
clean:
	rm -rf $(BUILD_DIR)
