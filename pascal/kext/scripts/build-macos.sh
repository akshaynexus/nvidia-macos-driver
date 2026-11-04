#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# pascal/kext/scripts/build-macos.sh — owner-run macOS build of PG106.kext.
#
# Run on the Mac (NOT by automation — HANDOFF-PASCAL.md §3: final kext
# compilation and all macOS validation are the owner's steps):
#
#   git clone --depth 1 https://github.com/osy/MacKernelSDK  (next to the repo
#     checkout, i.e. <repo>/../MacKernelSDK — the project points
#     KERNEL_FRAMEWORK_HEADERS there; this is the documented osy/MacKernelSDK
#     usage pattern: use its Headers tree in place of the system kernel
#     headers: https://github.com/osy/MacKernelSDK#usage)
#   cd pascal/kext && bash scripts/build-macos.sh
#
# Requirements: macOS with the Xcode Command Line Tools (xcodebuild +
# clang); install with `xcode-select --install` if missing. Git for the
# MacKernelSDK clone (or clone it yourself and set PG106_MACKERNELSDK).
#
# Output: PG106.kext under pascal/kext/build/xcodebuild/Release/.
# This script does NOT install, load or codesign anything (see README.md).

set -euo pipefail

if [ "$(uname -s)" != "Darwin" ]; then
	echo "build-macos.sh: this script drives xcodebuild and must run on macOS." >&2
	echo "You appear to be on $(uname -s 2>/dev/null || echo unknown)." >&2
	echo "On Linux, the equivalent gate is the stub harness: cd pascal/kext && make test" >&2
	exit 1
fi

command -v xcodebuild >/dev/null 2>&1 || {
	echo "build-macos.sh: xcodebuild not found." >&2
	echo "Install the Xcode Command Line Tools:  xcode-select --install" >&2
	exit 1
}

KEXT_ROOT="$(cd "$(dirname "$0")/.." && pwd)" # pascal/kext
SDK_DIR="${PG106_MACKERNELSDK:-$KEXT_ROOT/../../MacKernelSDK}"

if [ ! -d "$SDK_DIR/Headers" ]; then
	echo "build-macos.sh: MacKernelSDK not found at $SDK_DIR" >&2
	echo "Cloning https://github.com/osy/MacKernelSDK (one-time, --depth 1) ..." >&2
	git clone --depth 1 https://github.com/osy/MacKernelSDK "$SDK_DIR"
fi

cd "$KEXT_ROOT"
SYMROOT="$KEXT_ROOT/build/xcodebuild"
OBJROOT="$KEXT_ROOT/build/xcodebuild-obj"

xcodebuild -project PG106.xcodeproj \
	-scheme PG106 \
	-configuration Release \
	SYMROOT="$SYMROOT" OBJROOT="$OBJROOT" \
	build

KEXT="$SYMROOT/Release/PG106.kext"
if [ ! -d "$KEXT" ]; then
	echo "build-macos.sh: xcodebuild reported success but $KEXT is missing." >&2
	exit 1
fi

echo
echo "Built: $KEXT"
echo
echo "This is the Phase 3 SKELETON (probe gate + BAR0 map + PMC identify +"
echo "two UserClient selectors). Do not install or load it on a daily-use"
echo "system; read pascal/kext/README.md ('Not implemented yet') first, and"
echo "pascal/README.md + HANDOFF-PASCAL.md §3 for the hardware-test rules."
