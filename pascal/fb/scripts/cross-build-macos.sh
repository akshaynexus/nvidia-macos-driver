#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# cross-build-macos.sh — build PG106FB.kext (the GP106 HDMI IOFramebuffer)
# on Linux against the macOS SDK, the same way pascal/kext's script builds
# PG106.kext: clang -mkernel per TU, Apple ld64 -kext, bundle, KPI audit.
#
# Sources: src/PG106FB.cpp plus the pascal/core display files it needs
# (compiled into this kext with hidden visibility, so they never clash with
# PG106's copies of the same core files).
#
# Inputs: PG106_MACOS_SDK, PG106_LD64, PG106_XNU_CONFIG, PG106_MACOS_MIN,
# BUILD_DIR (see pascal/kext/scripts/cross-build-macos.sh).
#
# The audit accepts kpi exports from xnu config/*.exports for the declared
# KPIs, and IOGraphicsFamily (a kext, no xnu export list) by name: every
# symbol of the IOFramebuffer/IODisplay/IOGraphicsDevice classes and their
# metaclasses, checked against the SDK's IOFramebuffer.h. IOPCIFamily (the
# HDA-function rescan, IOPCIDevice::kernelRequestProbe) likewise: an
# IOPCIDevice method import must be declared in the SDK's IOPCIDevice.h.
# AppleHDA is not linked against at all: PG106FB talks to it only through
# registry properties (no AppleHDA symbol is imported, which the audit
# enforces by failing on any unknown import).
#
# Exit 0 = built and every import resolved; 1 = failure; 77 = tools missing.
set -euo pipefail

FB_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PASCAL="$(cd "$FB_ROOT/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$FB_ROOT/build/cross}"
MIN="${PG106_MACOS_MIN:-26.0}"
SDK="${PG106_MACOS_SDK:-$HOME/.config/swiftpm/swift-sdks/darwin.artifactbundle/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk}"
LD64="${PG106_LD64:-$(command -v x86_64-apple-darwin17-ld || true)}"
CLANG="${CLANG:-clang}"
NM="${PG106_NM:-llvm-nm}"

skip() { echo "cross-build-macos (PG106FB): SKIP — $*"; exit 77; }
command -v "$CLANG" >/dev/null || skip "clang not found"
command -v "$NM" >/dev/null || skip "llvm-nm not found"
[ -n "$LD64" ] && [ -x "$LD64" ] || skip "Apple ld64 not found (set PG106_LD64)"
KF="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
[ -f "$KF/IOKit/graphics/IOFramebuffer.h" ] && [ -f "$SDK/usr/lib/libkmod.a" ] ||
	skip "macOS SDK with Kernel.framework/IOKit/graphics not found at $SDK"
SDK_VER="$(grep -oE '"Version":"[0-9.]+"' "$SDK/SDKSettings.json" | head -1 | cut -d'"' -f4)"
[ -n "${PG106_XNU_CONFIG:-}" ] && [ -d "$PG106_XNU_CONFIG" ] ||
	skip "set PG106_XNU_CONFIG to an xnu config/ directory for the KPI audit"

SRCS=(
	"$FB_ROOT/src/PG106FB.cpp"
	"$PASCAL/core/src/disp_hw.c"
	"$PASCAL/core/src/disp_core.c"
	"$PASCAL/core/src/disp_mode.c"
	"$PASCAL/core/src/disp_vbios.c"
	"$PASCAL/core/src/edid.c"
	"$PASCAL/core/src/vbios_bit.c"
	"$PASCAL/core/src/devinit.c"
)

OBJ="$BUILD_DIR/obj"
rm -rf "$BUILD_DIR"; mkdir -p "$OBJ"
COMMON=(
	-target "x86_64-apple-macos$MIN" -isysroot "$SDK"
	-c -O2 -mkernel -nostdinc -fno-builtin
	-mllvm --disable-atexit-based-global-dtor-lowering
	-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
	-I "$PASCAL/kext/src/klibc" -I "$FB_ROOT/src" -I "$PASCAL/core/include"
	-I "$PASCAL/bridge/include" -I "$KF"
	-Wall -Wno-unused-parameter -Wno-deprecated-declarations
	-Wno-inconsistent-missing-override
)
fail=0
for src in "${SRCS[@]}"; do
	name="$(basename "$src")"
	case "$src" in
	*.cpp) lang=(-x c++ -std=gnu++14 -fapple-kext -fno-exceptions -fno-rtti) ;;
	*.c) lang=(-x c -std=gnu11 -fvisibility=hidden) ;;
	esac
	if "$CLANG" "${COMMON[@]}" "${lang[@]}" "$src" -o "$OBJ/${name%.*}.o"; then
		echo "PASS compile $name"
	else
		echo "FAIL compile $name"; fail=1
	fi
done
[ "$fail" = 0 ] || { echo "cross-build-macos (PG106FB): compile FAILED"; exit 1; }

BID="$(python3 -I -c 'import plistlib,sys; print(plistlib.load(open(sys.argv[1],"rb"))["CFBundleIdentifier"])' "$FB_ROOT/Info.plist")"
BVER="$(python3 -I -c 'import plistlib,sys; print(plistlib.load(open(sys.argv[1],"rb"))["CFBundleVersion"])' "$FB_ROOT/Info.plist")"
cat > "$BUILD_DIR/PG106FB_info.c" <<KMOD
#include <mach/mach_types.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL($BID, "$BVER", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = 0;
KMOD
"$CLANG" "${COMMON[@]}" -x c -std=gnu11 "$BUILD_DIR/PG106FB_info.c" -o "$OBJ/PG106FB_info.o" ||
	{ echo "FAIL compile PG106FB_info.c"; exit 1; }
echo "PASS compile PG106FB_info.c ($BID $BVER)"

KEXT="$BUILD_DIR/PG106FB.kext"
mkdir -p "$KEXT/Contents/MacOS"
"$LD64" -arch x86_64 -platform_version macos "$MIN" "$SDK_VER" -syslibroot "$SDK" \
	-kext -static -o "$KEXT/Contents/MacOS/PG106FB" "$OBJ"/*.o \
	-L"$SDK/usr/lib" -lkmodc++ -lkmod 2> >(grep -v "directory not found" >&2)
cp "$FB_ROOT/Info.plist" "$KEXT/Contents/Info.plist"
filetype="$(od -An -tu4 -j12 -N4 "$KEXT/Contents/MacOS/PG106FB" | tr -d ' ')"
[ "$filetype" = 11 ] || { echo "FAIL: Mach-O filetype $filetype, want 11"; exit 1; }
echo "PASS link MH_KEXT_BUNDLE, macOS $MIN, SDK $SDK_VER"
"$NM" "$KEXT/Contents/MacOS/PG106FB" | grep -q " _kmod_info$" || { echo "FAIL: no _kmod_info"; exit 1; }
# The core C objects must not export pg106_* (hidden visibility).
if "$NM" -g --defined-only "$KEXT/Contents/MacOS/PG106FB" | grep -E " T _pg106_" >/dev/null; then
	echo "FAIL: PG106FB exports pg106_* core symbols (would clash with PG106)"; exit 1
fi
echo "PASS _kmod_info present; core symbols private; $("$NM" "$KEXT/Contents/MacOS/PG106FB" | grep -c gMetaClass) metaclasses"

"$NM" -u "$KEXT/Contents/MacOS/PG106FB" | sed 's/^ *U //' | sort -u > "$BUILD_DIR/imports.txt"
python3 -I - "$PG106_XNU_CONFIG" "$BUILD_DIR/imports.txt" "$KEXT/Contents/Info.plist" "$KF/IOKit/graphics/IOFramebuffer.h" "$KF/IOKit/pci/IOPCIDevice.h" <<'PY'
import os, plistlib, re, sys
cfg, imports, plist, fbh, pcih = sys.argv[1:6]
pci_methods = set(re.findall(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", open(pcih).read()))
files = {"com.apple.kpi.bsd": "BSDKernel", "com.apple.kpi.iokit": "IOKit",
         "com.apple.kpi.libkern": "Libkern", "com.apple.kpi.mach": "Mach",
         "com.apple.kpi.unsupported": "Unsupported"}
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
# IOGraphicsFamily: the classes it defines, as mangled C++ names
# (_ZN13IOFramebuffer..., _ZTV13IOFramebuffer, __ZN13IOFramebuffer10gMetaClassE ...).
hdr = open(fbh).read()
fb_methods = set(re.findall(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", hdr))
# Padding vtable slots the header declares (OSMetaClassDeclareReserved*).
fb_methods |= {f"_RESERVEDIOFramebuffer{n}" for n in
               re.findall(r"OSMetaClassDeclareReserved(?:Unused|Used)\(\s*IOFramebuffer\s*,\s*(\d+)\s*\)", hdr)}
iog_classes = ("13IOFramebuffer", "16IOGraphicsDevice", "9IODisplay")
syms = [l.strip() for l in open(imports) if l.strip()]
counts, bad = {}, []
for s in syms:
    if s in declared:
        counts[declared[s]] = counts.get(declared[s], 0) + 1
        continue
    if "com.apple.iokit.IOPCIFamily" in libs and "11IOPCIDevice" in s:
        m = re.search(r"11IOPCIDevice(\d+)([A-Za-z_][A-Za-z0-9_]*)", s)
        if m and m.group(2)[: int(m.group(1))] in pci_methods:
            counts["com.apple.iokit.IOPCIFamily"] = counts.get("com.apple.iokit.IOPCIFamily", 0) + 1
            continue
    if "com.apple.iokit.IOGraphicsFamily" in libs and any(c in s for c in iog_classes):
        m = re.search(r"13IOFramebuffer(\d+)([A-Za-z_][A-Za-z0-9_]*)", s)
        ok = True
        if m and not s.startswith(("__ZTV", "__ZN13IOFramebuffer10gMetaClassE",
                                   "__ZN13IOFramebuffer9metaClassE")):
            name = m.group(2)[: int(m.group(1))]
            ok = name in fb_methods or name in ("gMetaClass", "metaClass", "superClass")
        if ok:
            counts["com.apple.iokit.IOGraphicsFamily"] = counts.get("com.apple.iokit.IOGraphicsFamily", 0) + 1
            continue
    bad.append(s)
for k, n in sorted(counts.items()):
    print(f"  {k}: {n}")
if bad:
    print(f"FAIL: {len(bad)} import(s) not exported by any declared KPI / family:")
    for s in bad:
        print("   ", s)
    sys.exit(1)
print(f"PASS KPI audit: all {len(syms)} imports resolve (IOFramebuffer and IOPCIDevice names checked against the SDK headers)")
PY
echo "cross-build-macos (PG106FB): OK — $KEXT"
