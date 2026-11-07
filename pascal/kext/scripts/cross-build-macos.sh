#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# cross-build-macos.sh — build a loadable-format PG106.kext on Linux.
#
# Compiles every TU in the Xcode target's Sources phase with clang against
# the macOS SDK's Kernel.framework, links an MH_KEXT_BUNDLE with Apple's ld64
# (cctools-port; LLVM's ld64.lld has no -kext), assembles the bundle, and
# audits every imported symbol against the KPIs the kext declares, using the
# export lists of the target macOS kernel (xnu config/*.exports).
#
# This proves the kext compiles, links and only imports symbols macOS
# exports. It does NOT prove the kext loads or works: that needs the
# Helios 300 running macOS (OWNER-RUNBOOK.md).
#
# Inputs (environment):
#   PG106_MACOS_SDK  MacOSX*.sdk with Kernel.framework + usr/lib/libkmod.a.
#                    Default: the SDK `xtool sdk install` extracts from Xcode
#                    (~/.config/swiftpm/swift-sdks/darwin.artifactbundle/...).
#   PG106_LD64       Apple ld64 for Linux, e.g. cctools-port's
#                    x86_64-apple-darwinNN-ld (https://github.com/tpoechtrager/cctools-port,
#                    needs apple-libtapi, BlocksRuntime and libdispatch).
#   PG106_MACOS_MIN  deployment target (default 26.0, macOS Tahoe).
#   PG106_XNU_TAG    xnu tag for the KPI audit (default xnu-12377.1.9 = macOS 26).
#   BUILD_DIR        output dir (default pascal/kext/build/cross).
#
# Output: $BUILD_DIR/PG106.kext and $BUILD_DIR/imports.txt. Exit 0 = built and
# every import resolved to a declared KPI; 1 = failure; 77 = tools missing.
set -euo pipefail

KEXT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO_ROOT="$(cd "$KEXT_ROOT/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$KEXT_ROOT/build/cross}"
MIN="${PG106_MACOS_MIN:-26.0}"
XNU_TAG="${PG106_XNU_TAG:-xnu-12377.1.9}"
SDK="${PG106_MACOS_SDK:-$HOME/.config/swiftpm/swift-sdks/darwin.artifactbundle/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk}"
LD64="${PG106_LD64:-$(command -v x86_64-apple-darwin17-ld || true)}"
CLANG="${CLANG:-clang}"
NM="${PG106_NM:-llvm-nm}"

skip() { echo "cross-build-macos: SKIP — $*"; exit 77; }
command -v "$CLANG" >/dev/null || skip "clang not found"
command -v "$NM" >/dev/null || skip "llvm-nm not found (set PG106_NM)"
[ -n "$LD64" ] && [ -x "$LD64" ] || skip "Apple ld64 not found (set PG106_LD64)"
KF="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
[ -d "$KF/IOKit" ] && [ -f "$SDK/usr/lib/libkmod.a" ] ||
	skip "macOS SDK with Kernel.framework not found at $SDK (set PG106_MACOS_SDK)"
SDK_VER="$(grep -oE '"Version":"[0-9.]+"' "$SDK/SDKSettings.json" | head -1 | cut -d'"' -f4)"

OBJ="$BUILD_DIR/obj"
rm -rf "$BUILD_DIR"; mkdir -p "$OBJ"
PBXPROJ="$(ls -d "$KEXT_ROOT"/*.xcodeproj)/project.pbxproj"
mapfile -t names < <(sed -n 's/.*\/\* \(.*\) in Sources \*\/,$/\1/p' "$PBXPROJ")
[ "${#names[@]}" -gt 0 ] || { echo "no Sources entries in $PBXPROJ"; exit 1; }

# Kext code generation, as Apple clang does it for -mkernel/-fapple-kext.
# Upstream LLVM lowers llvm.global_dtors to __cxa_atexit on Mach-O; the
# kernel exports no __cxa_atexit, so keep destructors in __mod_term_func
# (run by libkmod's kmod_stop) with --disable-atexit-based-global-dtor-lowering.
COMMON=(
	-target "x86_64-apple-macos$MIN" -isysroot "$SDK"
	-c -O2 -mkernel -nostdinc -fno-builtin
	-mllvm --disable-atexit-based-global-dtor-lowering
	-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
	-I "$KEXT_ROOT/src/klibc" -I "$KEXT_ROOT/src" -I "$KEXT_ROOT/../core/include"
	-I "$KF"
	-Wall -Wno-unused-parameter -Wno-deprecated-declarations
	-Wno-inconsistent-missing-override
)
fail=0
for name in "${names[@]}"; do
	ref="$(grep -m1 -F "/* $name */ = {isa = PBXFileReference" "$PBXPROJ" |
		sed -n 's/.* path = \([^;]*\);.*/\1/p' | tr -d '"')"
	case "$ref" in ../*) src="$KEXT_ROOT/$ref" ;; *) src="$KEXT_ROOT/src/$ref" ;; esac
	case "$src" in
	*.cpp) lang=(-x c++ -std=gnu++14 -fapple-kext -fno-exceptions -fno-rtti) ;;
	*.c) lang=(-x c -std=gnu11) ;;
	*) echo "FAIL $name: unknown source type"; fail=1; continue ;;
	esac
	if "$CLANG" "${COMMON[@]}" "${lang[@]}" "$src" -o "$OBJ/${name%.*}.o"; then
		echo "PASS compile $name"
	else
		echo "FAIL compile $name"; fail=1
	fi
done
[ "$fail" = 0 ] || { echo "cross-build-macos: compile FAILED"; exit 1; }

KEXT="$BUILD_DIR/PG106.kext"
mkdir -p "$KEXT/Contents/MacOS" "$KEXT/Contents/Resources"
"$LD64" -arch x86_64 -platform_version macos "$MIN" "$SDK_VER" -syslibroot "$SDK" \
	-kext -static -o "$KEXT/Contents/MacOS/PG106" "$OBJ"/*.o \
	-L"$SDK/usr/lib" -lkmodc++ -lkmod 2> >(grep -v "directory not found" >&2)
cp "$KEXT_ROOT/Info.plist" "$KEXT/Contents/Info.plist"
# Firmware the bring-up loads from the bundle (NVIDIA licence: licenses/).
if [ -d "$REPO_ROOT/research/firmware/gp106" ]; then
	mkdir -p "$KEXT/Contents/Resources/firmware"
	cp -r "$REPO_ROOT/research/firmware/gp106" "$KEXT/Contents/Resources/firmware/"
	rm -f "$KEXT/Contents/Resources/firmware/gp106/README.md"
fi
filetype="$(od -An -tu4 -j12 -N4 "$KEXT/Contents/MacOS/PG106" | tr -d ' ')"
[ "$filetype" = 11 ] || { echo "FAIL: Mach-O filetype $filetype, want 11 (MH_KEXT_BUNDLE)"; exit 1; }
echo "PASS link MH_KEXT_BUNDLE, macOS $MIN, SDK $SDK_VER"

# KPI audit: every import must come from a KPI listed in OSBundleLibraries
# (xnu config/<KPI>[.x86_64][.MacOSX].exports) or from IOPCIFamily.
XNU="$BUILD_DIR/xnu"
if [ -n "${PG106_XNU_CONFIG:-}" ]; then
	CFG="$PG106_XNU_CONFIG"
else
	git init -q "$XNU" &&
		git -C "$XNU" sparse-checkout set config >/dev/null 2>&1 &&
		git -C "$XNU" fetch -q --depth 1 --filter=blob:none \
			https://github.com/apple-oss-distributions/xnu.git "refs/tags/$XNU_TAG" &&
		git -C "$XNU" checkout -q FETCH_HEAD ||
		{ echo "cross-build-macos: built, but KPI audit SKIPPED (cannot fetch $XNU_TAG)"; exit 77; }
	CFG="$XNU/config"
fi
"$NM" -u "$KEXT/Contents/MacOS/PG106" | sed 's/^ *U //' | sort -u > "$BUILD_DIR/imports.txt"
python3 -I - "$CFG" "$BUILD_DIR/imports.txt" "$KEXT/Contents/Info.plist" <<'PY'
import os, plistlib, sys
cfg, imports, plist = sys.argv[1:4]
files = {"com.apple.kpi.bsd": "BSDKernel", "com.apple.kpi.iokit": "IOKit",
         "com.apple.kpi.libkern": "Libkern", "com.apple.kpi.mach": "Mach",
         "com.apple.kpi.unsupported": "Unsupported", "com.apple.kpi.dsep": "MACFramework",
         "com.apple.kpi.private": "Private"}
libs = plistlib.load(open(plist, "rb")).get("OSBundleLibraries", {})
declared = {}
for bundle in libs:
    base = files.get(bundle)
    for suf in ("", ".x86_64", ".x86_64.MacOSX", ".MacOSX") if base else ():
        f = os.path.join(cfg, base + suf + ".exports")
        if os.path.exists(f):
            for line in open(f):
                s = line.split(":")[0].strip()
                if s and not s.startswith("#"):
                    declared.setdefault(s, bundle)
syms = [l.strip() for l in open(imports) if l.strip()]
counts, bad = {}, []
for s in syms:
    if s in declared:
        counts[declared[s]] = counts.get(declared[s], 0) + 1
    elif "IOPCIDevice" in s and "com.apple.iokit.IOPCIFamily" in libs:
        counts["com.apple.iokit.IOPCIFamily"] = counts.get("com.apple.iokit.IOPCIFamily", 0) + 1
    else:
        bad.append(s)
for k, n in sorted(counts.items()):
    print(f"  {k}: {n}")
if "com.apple.kpi.private" in libs:
    print("FAIL: kext declares com.apple.kpi.private (Apple-only)")
    sys.exit(1)
if bad:
    print(f"FAIL: {len(bad)} import(s) not exported by any declared KPI:")
    for s in bad:
        print("   ", s)
    sys.exit(1)
print(f"PASS KPI audit: all {len(syms)} imports resolve to declared KPIs")
PY
echo "cross-build-macos: OK — $KEXT"
