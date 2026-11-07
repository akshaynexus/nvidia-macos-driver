#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# pascal/kext/scripts/kext-syntax-check.sh — compile every translation unit of
# the PG106 Xcode target with clang -fsyntax-only against the real
# MacKernelSDK headers, on Linux or macOS.
#
# The Linux stub harness (stub/) only builds the pure-C parts; it cannot see
# C++/IOKit mistakes such as overriding a virtual that does not exist or
# calling an API with the wrong signature. clang can target x86_64-apple-macos
# from any host, and MacKernelSDK is a plain header tree, so this check runs
# the kext's own sources through the same front end Xcode uses. It is a
# syntax/semantic check only: nothing is code-generated or linked, so link
# errors (missing KPI symbols) still need the macOS build.
#
# Translation units are read from PG106.xcodeproj/project.pbxproj (the
# Sources build phase), so the check cannot drift from what Xcode compiles.
#
# SDK location: $PG106_MACKERNELSDK, else <repo>/MacKernelSDK (the path the
# Xcode project and build-macos.sh use). If neither exists and
# PG106_SDK_FETCH=1, the pinned commit is cloned into $BUILD_DIR.
#
# Exit 0 = all TUs clean; 1 = a TU failed; 77 = skipped (no clang or no SDK),
# printed loudly so a skip is never mistaken for a pass.

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
KEXT_ROOT="$(dirname "$HERE")"
REPO_ROOT="$(cd "$KEXT_ROOT/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$KEXT_ROOT/build}"
PBXPROJ="$KEXT_ROOT/PG106.xcodeproj/project.pbxproj"

# Pinned so the check is reproducible; build-macos.sh uses the same commit.
SDK_URL="https://github.com/acidanthera/MacKernelSDK"
SDK_COMMIT="3f750085caa17ec3a7880f11c11bf4f48cd6a164"

CLANG="${CLANG:-clang}"
if ! command -v "$CLANG" >/dev/null 2>&1; then
	echo "kext-syntax-check: SKIP — clang not found (set CLANG=...)"
	exit 77
fi

SDK="${PG106_MACKERNELSDK:-$REPO_ROOT/MacKernelSDK}"
if [ ! -d "$SDK/Headers/IOKit" ] && [ "${PG106_SDK_FETCH:-0}" = 1 ]; then
	SDK="$BUILD_DIR/MacKernelSDK"
	if [ ! -d "$SDK/Headers/IOKit" ]; then
		mkdir -p "$BUILD_DIR"
		git init -q "$SDK" &&
			git -C "$SDK" fetch -q --depth 1 "$SDK_URL" "$SDK_COMMIT" &&
			git -C "$SDK" checkout -q FETCH_HEAD ||
			{ echo "kext-syntax-check: SKIP — could not fetch MacKernelSDK"; exit 77; }
	fi
fi
if [ ! -d "$SDK/Headers/IOKit" ]; then
	echo "kext-syntax-check: SKIP — MacKernelSDK not found at $SDK"
	echo "  set PG106_MACKERNELSDK=<path>, or PG106_SDK_FETCH=1 to clone $SDK_URL @ ${SDK_COMMIT:0:12}"
	exit 77
fi

# Sources phase → file names → paths (PBXFileReference `path = ...`).
mapfile -t names < <(sed -n 's/.*\/\* \(.*\) in Sources \*\/,$/\1/p' "$PBXPROJ")
if [ "${#names[@]}" -eq 0 ]; then
	echo "kext-syntax-check: FAIL — no Sources entries parsed from $PBXPROJ"
	exit 1
fi

# Flags mirror the Xcode target: kernel-extension product defines, the
# project's HEADER_SEARCH_PATHS order (src/klibc first), language standards
# (GCC_C_LANGUAGE_STANDARD gnu11, CLANG_CXX_LANGUAGE_STANDARD gnu++14) and
# MACOSX_DEPLOYMENT_TARGET 10.13.
COMMON=(
	-target x86_64-apple-macos10.13
	-fsyntax-only -mkernel -nostdinc -fno-builtin
	-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
	-I "$KEXT_ROOT/src/klibc" -I "$KEXT_ROOT/src" -I "$KEXT_ROOT/../core/include"
	-I "$SDK/Headers"
	-Wall -Wextra -Werror
	# MacKernelSDK headers trip these under -Wextra; they are not ours.
	-Wno-unused-parameter -Wno-deprecated-declarations
)

fail=0
for name in "${names[@]}"; do
	ref="$(grep -m1 -F "/* $name */ = {isa = PBXFileReference" "$PBXPROJ" |
		sed -n 's/.* path = \([^;]*\);.*/\1/p' | tr -d '"')"
	case "$ref" in
	../*) src="$KEXT_ROOT/$ref" ;;  # core sources, relative to SRCROOT
	*) src="$KEXT_ROOT/src/$ref" ;; # members of the src group
	esac
	if [ ! -f "$src" ]; then
		echo "FAIL $name: pbxproj path $ref does not exist ($src)"
		fail=1
		continue
	fi
	case "$src" in
	*.cpp) lang=(-x c++ -std=gnu++14 -fapple-kext -fno-exceptions -fno-rtti) ;;
	*.c) lang=(-x c -std=gnu11) ;;
	*) echo "FAIL $name: unknown source type"; fail=1; continue ;;
	esac
	if "$CLANG" "${COMMON[@]}" "${lang[@]}" "$src"; then
		echo "PASS syntax $name"
	else
		echo "FAIL syntax $name"
		fail=1
	fi
done

if [ "$fail" != 0 ]; then
	echo "kext-syntax-check: FAILED"
	exit 1
fi
echo "kext-syntax-check: OK — ${#names[@]} TUs against MacKernelSDK ${SDK}"
