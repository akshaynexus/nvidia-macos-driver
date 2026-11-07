#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# pascal/kext/scripts/build-macos.sh — owner-run macOS build of PG106.kext.
#
# Run on the Mac (NOT by automation — HANDOFF-PASCAL.md §3: final kext
# compilation and all macOS validation are the owner's steps):
#
#   cd pascal/kext && bash scripts/build-macos.sh
#
# MacKernelSDK (https://github.com/acidanthera/MacKernelSDK, pinned commit
# below) is cloned to <repo>/MacKernelSDK on first run (gitignored), or taken
# from $PG106_MACKERNELSDK. The Xcode project points its kernel header and
# libkmod settings there, as the SDK's README "Quick start" describes.
#
# Requirements: macOS with the Xcode Command Line Tools (xcodebuild +
# clang); install with `xcode-select --install` if missing. Git for the
# MacKernelSDK clone (or clone it yourself and set PG106_MACKERNELSDK).
#
# Output: PG106.kext under pascal/kext/build/xcodebuild/Release/ (ad-hoc
# signed by Xcode, CODE_SIGN_IDENTITY "-") and the probe tool
# pascal/kext/build/pg106_probe. This script does NOT install or load
# anything: research/evidence/OWNER-RUNBOOK.md covers that.

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
SDK_DIR="${PG106_MACKERNELSDK:-$(cd "$KEXT_ROOT/../.." && pwd)/MacKernelSDK}"
SDK_URL="https://github.com/acidanthera/MacKernelSDK"
SDK_COMMIT="3f750085caa17ec3a7880f11c11bf4f48cd6a164" # = kext-syntax-check.sh

if [ ! -d "$SDK_DIR/Headers" ]; then
	echo "build-macos.sh: fetching $SDK_URL @ ${SDK_COMMIT:0:12} into $SDK_DIR" >&2
	git init -q "$SDK_DIR"
	git -C "$SDK_DIR" fetch -q --depth 1 "$SDK_URL" "$SDK_COMMIT"
	git -C "$SDK_DIR" checkout -q FETCH_HEAD
fi

# Same front-end check the Linux side runs; fails fast before xcodebuild.
PG106_MACKERNELSDK="$SDK_DIR" bash "$KEXT_ROOT/scripts/kext-syntax-check.sh"

# Firmware for the bundle's Resources (the pbxproj Resources phase points
# at build/fw-stage/<resource>).
bash "$KEXT_ROOT/scripts/stage-firmware.sh" "$KEXT_ROOT/build/fw-stage" --placeholders

cd "$KEXT_ROOT"
SYMROOT="$KEXT_ROOT/build/xcodebuild"
OBJROOT="$KEXT_ROOT/build/xcodebuild-obj"

xcodebuild -project PG106.xcodeproj \
	-scheme PG106 \
	-configuration Release \
	SYMROOT="$SYMROOT" OBJROOT="$OBJROOT" \
	PG106_MACKERNELSDK="$SDK_DIR" \
	build

KEXT="$SYMROOT/Release/PG106.kext"
if [ ! -d "$KEXT" ]; then
	echo "build-macos.sh: xcodebuild reported success but $KEXT is missing." >&2
	exit 1
fi

clang -Wall -Wextra -Werror -I"$KEXT_ROOT/src" \
	-o "$KEXT_ROOT/build/pg106_probe" "$KEXT_ROOT/tools/pg106_probe.c" \
	-framework IOKit -framework CoreFoundation

echo
echo "Built: $KEXT"
echo "       $KEXT_ROOT/build/pg106_probe"
echo
echo "Loading this kext RUNS THE BRING-UP on the GPU (POST if needed, PTOP,"
echo "ACR up to the first unported step). Read pascal/kext/README.md ('What is"
echo "real, what is stubbed') and research/evidence/OWNER-RUNBOOK.md before"
echo "loading it anywhere. Safe mode: boot-arg pg106_stop_after=1 (identify +"
echo "posted check only, no GPU writes)."
