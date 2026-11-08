#!/bin/bash
# SPDX-License-Identifier: MIT
# macos-syntax-check.sh — compile the bridge's sources (including the
# macOS-only IOKit transport) with clang -fsyntax-only against a macOS SDK,
# for x86_64 and arm64. Syntax/semantics only: nothing is linked or run.
#
# SDK: $PG106_MACOS_SDK, else the one `xtool sdk install` extracts
# (~/.config/swiftpm/swift-sdks/darwin.artifactbundle/...). Exit 77 = SKIP.
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${PG106_MACOS_SDK:-$HOME/.config/swiftpm/swift-sdks/darwin.artifactbundle/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk}"
CLANG="${CLANG:-clang}"
command -v "$CLANG" >/dev/null 2>&1 || { echo "macos-syntax-check: SKIP — no clang"; exit 77; }
[ -f "$SDK/System/Library/Frameworks/IOKit.framework/Headers/IOKitLib.h" ] ||
	{ echo "macos-syntax-check: SKIP — no macOS SDK with IOKit.framework at $SDK"; exit 77; }
fail=0
for arch in x86_64 arm64; do
	for f in "$HERE"/src/*.c; do
		if "$CLANG" -target "$arch-apple-macos13" -isysroot "$SDK" -fsyntax-only \
			-std=c11 -Wall -Wextra -Werror -I "$HERE/include" \
			-I "$HERE/../core/include" "$f"; then
			echo "PASS macos $arch $(basename "$f")"
		else
			echo "FAIL macos $arch $(basename "$f")"
			fail=1
		fi
	done
done
exit $fail
