#!/bin/bash
# NullMoth NVIDIA driver for macOS
# Copyright (c) 2026 NullMoth Systems.
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# cross-build.sh — build PG106Accel.kext on Linux and audit it, for macOS 26 (Tahoe).
#
# Same toolchain as pascal/kext/scripts/cross-build-macos.sh (clang -mkernel/-fapple-kext against the macOS SDK's
# Kernel.framework, Apple's ld64 from cctools-port, the xnu export lists). That script is GPL; nothing is shared
# with it but the approach.
#
#  1. bootstrap build without Tahoe forwards; its vtables give the inherited slots macOS 26 no longer exports;
#     pga_tahoe_fwd.h is regenerated from them and must match the committed copy (PGA_REGEN=1 rewrites it);
#  2. the real build: MH_KEXT_BUNDLE, _kmod_info present;
#  3. KPI audit: every import is exported by an xnu KPI or a kext in OSBundleLibraries (IOAcceleratorFamily2 is
#     checked against a macOS 26 copy of the binary; IOPCIFamily and IOGraphicsFamily by class name);
#  4. licence check: the only file under pascal/ in the build is pascal/bridge/include/pg106_kif.h, and it is MIT
#     (SPDX-License-Identifier: MIT); nothing GPL is compiled in;
#  5. the display-machine vtable slot that the family's framebuffer sweep calls (+0x8d8) is our found_framebuffer,
#     which takes the GP106's PG106FB and nothing else: it goes through pga_fb_decide (host-tested in check 0), names
#     the class PG106FB, and only then calls the family's IOAccelDisplayMachine::found_framebuffer;
#  6. the display pipe is ours: the accelerator's newDisplayPipe and the pipe's performTransaction /
#     isTransactionComplete are PG106Accel's in the vtables the family calls.
#  0. (first) the host tests of the decision logic, tests/run.sh.
#
# This proves the kext compiles, links and imports only what macOS 26 exports. It does not prove it loads, that
# Metal accepts it, or that a frame reaches the HDMI screen: that needs the Helios 300
# (research/evidence/OWNER-RUNBOOK.md steps 4c and 4e).
#
# Environment:
#   PGA_MACOS_SDK  MacOSX*.sdk with Kernel.framework and usr/lib/libkmod.a
#                  (default: the SDK `xtool sdk install` extracts, ~/.config/swiftpm/swift-sdks/darwin.artifactbundle/...)
#   PGA_LD64       Apple ld64 for Linux (cctools-port x86_64-apple-darwinNN-ld)
#   PGA_XNU_CONFIG xnu config/ directory with *.exports (default: fetch $PGA_XNU_TAG, xnu-12377.1.9 = macOS 26)
#   PGA_IAF2       macOS 26 IOAcceleratorFamily2 binary
#                  (default research/prior-art/meteor-lake-xe/research/IOAcceleratorFamily2-recovery-reference.macho)
#   PGA_REGEN=1    write the regenerated pga_tahoe_fwd.h instead of failing on a difference
#   BUILD_DIR      output (default ${TMPDIR:-/tmp}/PG106Accel-cross, outside the repository)
# Exit 0 = every check passed; 1 = a check failed; 77 = a tool or input is missing.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${TMPDIR:-/tmp}/PG106Accel-cross}"
MIN=26.0
XNU_TAG="${PGA_XNU_TAG:-xnu-12377.1.9}"
SDK="${PGA_MACOS_SDK:-$HOME/.config/swiftpm/swift-sdks/darwin.artifactbundle/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk}"
LD64="${PGA_LD64:-$(command -v x86_64-apple-darwin17-ld || true)}"
IAF2="${PGA_IAF2:-$REPO/research/prior-art/meteor-lake-xe/research/IOAcceleratorFamily2-recovery-reference.macho}"
CLANG="${CLANG:-clang}"
NM="${PGA_NM:-llvm-nm}"
OBJDUMP="${PGA_OBJDUMP:-llvm-objdump}"

skip() { echo "cross-build: SKIP — $*"; exit 77; }
fail() { echo "FAIL: $*"; exit 1; }
for t in "$CLANG" "$NM" "$OBJDUMP" c++filt python3 od cc c++; do command -v "$t" >/dev/null || skip "$t not found"; done
[ -n "$LD64" ] && [ -x "$LD64" ] || skip "Apple ld64 not found (set PGA_LD64)"
KF="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
[ -d "$KF/IOKit" ] && [ -f "$SDK/usr/lib/libkmod.a" ] || skip "macOS SDK with Kernel.framework not found at $SDK (set PGA_MACOS_SDK)"
[ -f "$IAF2" ] || skip "no macOS 26 IOAcceleratorFamily2 binary at $IAF2 (set PGA_IAF2)"
SDK_VER="$(grep -oE '"Version":"[0-9.]+"' "$SDK/SDKSettings.json" | head -1 | cut -d'"' -f4)"

rm -rf "$BUILD_DIR"; mkdir -p "$BUILD_DIR"

# ---- 0. host tests of the decision logic ----
TMPDIR="$BUILD_DIR" bash "$HERE/tests/run.sh" | tail -1 | grep -q '^PASS' || fail "host tests (tests/run.sh)"
echo "PASS host tests: pga_logic.h + plugin/nvmtl_ws_gpu.h ($(TMPDIR="$BUILD_DIR" bash "$HERE/tests/run.sh" | tail -1))"
FLAGS=(-target "x86_64-apple-macos$MIN" -isysroot "$SDK" -c -O2 -mkernel -nostdinc -fno-builtin -fno-common
       # upstream LLVM lowers global destructors to __cxa_atexit, which the kernel does not export
       -mllvm --disable-atexit-based-global-dtor-lowering
       -x c++ -std=gnu++17 -fapple-kext -fno-exceptions -fno-rtti
       -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
       -I "$HERE" -I "$REPO/kexts/NVRM/accel/re" -I "$REPO/kexts/NVRM/accel/iofam" -I "$REPO/pascal/bridge/include" -I "$KF"
       -Wall -Werror -Wno-unused-parameter -Wno-deprecated-declarations -Wno-inconsistent-missing-override)
build() {   # build <dir> [extra clang flags...]
	local d=$1; shift
	mkdir -p "$d/PG106Accel.kext/Contents/MacOS"
	"$CLANG" "${FLAGS[@]}" "$@" -MD -MF "$d/deps.txt" "$HERE/pg106-accel.cpp" -o "$d/pg106-accel.o" || return 1
	"$LD64" -arch x86_64 -platform_version macos "$MIN" "$SDK_VER" -syslibroot "$SDK" -kext -static \
		-o "$d/PG106Accel.kext/Contents/MacOS/PG106Accel" "$d/pg106-accel.o" -L"$SDK/usr/lib" -lkmodc++ -lkmod \
		2> >(grep -v -e "directory not found" -e "was built for newer macOS version" >&2) || return 1
	cp "$HERE/Info.plist" "$d/PG106Accel.kext/Contents/Info.plist"
	"$NM" -u "$d/PG106Accel.kext/Contents/MacOS/PG106Accel" | sed 's/^ *U //' | sort -u > "$d/imports.txt"
}

# ---- export sets: xnu KPIs and the kexts in OSBundleLibraries ----
if [ -n "${PGA_XNU_CONFIG:-}" ]; then CFG="$PGA_XNU_CONFIG"
else
	XNU="$BUILD_DIR/xnu"
	{ git init -q "$XNU" && git -C "$XNU" sparse-checkout set config >/dev/null 2>&1 &&
	  git -C "$XNU" fetch -q --depth 1 --filter=blob:none https://github.com/apple-oss-distributions/xnu.git "refs/tags/$XNU_TAG" &&
	  git -C "$XNU" checkout -q FETCH_HEAD; } || skip "cannot fetch $XNU_TAG for the KPI audit (set PGA_XNU_CONFIG)"
	CFG="$XNU/config"
fi
"$NM" -gU "$IAF2" | awk '{print $3}' | sort -u > "$BUILD_DIR/iaf2.exports"
# a macOS 26 family: IOAccelResource2::pageon(IOAccelEvent*, bool) is one of the methods Tahoe stopped exporting
grep -qx '__ZN16IOAccelResource26pageonEP12IOAccelEventb' "$BUILD_DIR/iaf2.exports" &&
	fail "$IAF2 exports IOAccelResource2::pageon: it is not a macOS 26 IOAcceleratorFamily2"
echo "PASS IOAcceleratorFamily2 reference is a macOS 26 build ($(wc -l < "$BUILD_DIR/iaf2.exports") exports)"
audit() {   # audit <imports> <unresolved-out>: prints per-library counts, writes what nothing exports
	python3 -I - "$CFG" "$1" "$HERE/Info.plist" "$BUILD_DIR/iaf2.exports" "$2" <<'PY'
import os, plistlib, sys
cfg, imports, plist, iaf2, out = sys.argv[1:6]
kpi = {"com.apple.kpi.bsd": "BSDKernel", "com.apple.kpi.iokit": "IOKit", "com.apple.kpi.libkern": "Libkern",
       "com.apple.kpi.mach": "Mach", "com.apple.kpi.unsupported": "Unsupported", "com.apple.kpi.private": "Private"}
libs = plistlib.load(open(plist, "rb")).get("OSBundleLibraries", {})
if "com.apple.kpi.private" in libs: sys.exit("FAIL: com.apple.kpi.private is Apple-only")
where = {}
for b in libs:
    for suf in ("", ".x86_64", ".x86_64.MacOSX", ".MacOSX") if b in kpi else ():
        f = os.path.join(cfg, kpi[b] + suf + ".exports")
        if os.path.exists(f):
            for l in open(f):
                s = l.split(":")[0].strip()
                if s and not s.startswith("#"): where.setdefault(s, b)
if "com.apple.iokit.IOAcceleratorFamily2" in libs:
    for l in open(iaf2):
        if l.strip(): where.setdefault(l.strip(), "com.apple.iokit.IOAcceleratorFamily2")
# kexts with no export list here, accepted by the classes they define (as pascal/kext's audit does for IOPCIFamily)
byname = {"com.apple.iokit.IOPCIFamily": ("IOPCIDevice",), "com.apple.iokit.IOGraphicsFamily": ("13IOAccelerator", "13IOFramebuffer"),
          "com.apple.iokit.IOSurface": ("ZNK9IOSurface",)}
counts, bad = {}, []
for s in (l.strip() for l in open(imports) if l.strip()):
    b = where.get(s) or next((k for k, ns in byname.items() if k in libs and any(n in s for n in ns)), None)
    if b: counts[b] = counts.get(b, 0) + 1
    else: bad.append(s)
for k, n in sorted(counts.items()): print("  %s: %d" % (k, n))
open(out, "w").write("".join(s + "\n" for s in bad))
PY
}

# ---- 1. bootstrap build and the Tahoe forwards ----
build "$BUILD_DIR/boot" -DPGA_FWD_BOOTSTRAP || fail "bootstrap build"
audit "$BUILD_DIR/boot/imports.txt" "$BUILD_DIR/boot/missing.txt" >/dev/null
python3 -I "$HERE/gen_tahoe_fwd.py" "$BUILD_DIR/boot/PG106Accel.kext/Contents/MacOS/PG106Accel" "$BUILD_DIR/boot/missing.txt" \
	"$BUILD_DIR/pga_tahoe_fwd.h"
if ! cmp -s "$BUILD_DIR/pga_tahoe_fwd.h" "$HERE/pga_tahoe_fwd.h"; then
	if [ "${PGA_REGEN:-0}" = 1 ]; then cp "$BUILD_DIR/pga_tahoe_fwd.h" "$HERE/pga_tahoe_fwd.h"; echo "NOTE pga_tahoe_fwd.h regenerated"
	else diff -u "$HERE/pga_tahoe_fwd.h" "$BUILD_DIR/pga_tahoe_fwd.h" | head -20; fail "pga_tahoe_fwd.h is stale (PGA_REGEN=1 rewrites it)"; fi
fi
echo "PASS pga_tahoe_fwd.h matches this source"

# ---- 2. the real build ----
OUT="$BUILD_DIR/out"; BIN="$OUT/PG106Accel.kext/Contents/MacOS/PG106Accel"
build "$OUT" || fail "build"
echo "PASS compile + link"
[ "$(od -An -tu4 -j12 -N4 "$BIN" | tr -d ' ')" = 11 ] || fail "Mach-O filetype is not MH_KEXT_BUNDLE (11)"
echo "PASS MH_KEXT_BUNDLE, macOS $MIN, SDK $SDK_VER"
"$NM" "$BIN" | grep -q " _kmod_info$" || fail "no _kmod_info (kmutil would leave the kext out of the collection)"
echo "PASS _kmod_info present; $("$NM" "$BIN" | grep -c 'gMetaClassE$') metaclasses"

# ---- 3. KPI audit ----
audit "$OUT/imports.txt" "$OUT/unresolved.txt"
if [ -s "$OUT/unresolved.txt" ]; then echo "FAIL: imports no declared KPI or kext exports:"; sed 's/^/    /' "$OUT/unresolved.txt"; exit 1; fi
echo "PASS KPI audit: all $(wc -l < "$OUT/imports.txt") imports resolve to OSBundleLibraries entries"

# ---- 4. licence: nothing from the GPL tree; the one shared header is MIT ----
PDEPS="$(tr ' \\' '\n\n' < "$OUT/deps.txt" | grep "/pascal/" | sort -u || true)"
for f in $PDEPS; do
	case "$(cd "$(dirname "$f")" && pwd)/$(basename "$f")" in
	"$REPO/pascal/bridge/include/pg106_kif.h") ;;
	*) fail "a file under pascal/ other than pascal/bridge/include/pg106_kif.h is part of the build: $f" ;;
	esac
	head -3 "$f" | grep -qx '// SPDX-License-Identifier: MIT' || fail "$f is not SPDX MIT"
done
[ -n "$PDEPS" ] || fail "pg106_kif.h is not in the build (the include path is wrong)"
echo "PASS licence: from pascal/ only pascal/bridge/include/pg106_kif.h (SPDX MIT) is compiled in; $(tr ' \\' '\n\n' < "$OUT/deps.txt" | grep -c '\.h$') headers in all"

# ---- 5. the framebuffer sweep reaches our filter, which takes only PG106FB ----
# IOAccelDisplayMachine::start(IOPCIDevice *) adds each framebuffer it finds with a virtual call through vtable +0x8d8;
# PG106Accel's own pairing calls the same slot. The decision itself is pga_fb_decide (check 0 tests that the Intel
# panel, a PG106FB on another GPU, headless mode, a missing/incompatible PG106FBKifABI, PG106 not ready and a second
# PG106FB are all refused).
python3 -I - "$REPO/tools" "$BIN" "$OUT/imports.txt" "$OBJDUMP" <<'PY'
import subprocess, sys
sys.path.insert(0, sys.argv[1]); from vtdump import vtables
dem = lambda s: subprocess.run(["c++filt", "-_", s], capture_output=True, text=True).stdout.strip()
vt = vtables(sys.argv[2])
name = dem(vt["24PG106AccelDisplayMachine"][0x8d8 // 8])
if name != "PG106AccelDisplayMachine::found_framebuffer(IOFramebuffer*)": sys.exit("FAIL: vtable +0x8d8 is %s" % name)
data = open(sys.argv[2], "rb").read()
if b"\0PG106FB\0" not in data: sys.exit("FAIL: the class name PG106FB is not in the binary (the filter cannot be keying on it)")
imports = set(l.strip() for l in open(sys.argv[3]))
# -fapple-kext calls the base class's found_framebuffer through IOAccelLegacyDisplayMachine's own vtable, slot +0x8d8
# (+0x10 for offset-to-top and RTTI): the filter's accepting branch must make exactly that call.
if "__ZTV27IOAccelLegacyDisplayMachine" not in imports:
    sys.exit("FAIL: no reference to IOAccelLegacyDisplayMachine's vtable (the filter cannot reach the family's found_framebuffer)")
dis = subprocess.run([sys.argv[4], "-d", "--no-show-raw-insn", sys.argv[2]], capture_output=True, text=True).stdout
fn, body = "__ZN24PG106AccelDisplayMachine17found_framebufferEP13IOFramebuffer>:", []
if fn not in dis: sys.exit("FAIL: found_framebuffer not in the disassembly")
for l in dis.split(fn, 1)[1].split("\n")[1:]:
    if not l.strip(): break
    body.append(l)
if not any("*0x8e8(%r" in l and ("jmp" in l or "call" in l) for l in body):
    sys.exit("FAIL: found_framebuffer never calls the family's found_framebuffer (no indirect call through +0x8e8)")
if not any("fbAcceptable" in l and "call" in l for l in body):
    sys.exit("FAIL: found_framebuffer does not go through PG106Accel::fbAcceptable (pga_fb_decide)")
print("PASS vtable +0x8d8 (the family's framebuffer sweep) is %s: PG106FB only, then the family's" % name)
# ---- 6. the display pipe ----
acc = [dem(s) for s in vt["10PG106Accel"]]
# slot 331 is IOGraphicsAccelerator2::newDisplayPipe (re/IOGraphicsAccelerator2.h counts the two header words of the
# vtable; vtdump starts at the first method)
if len(acc) <= 329 or acc[329] != "PG106Accel::newDisplayPipe()":
    sys.exit("FAIL: accelerator vtable slot 331 is %s, not PG106Accel::newDisplayPipe()" % (acc[329] if len(acc) > 329 else "missing"))
pipe = [dem(s) for s in vt["21PG106AccelDisplayPipe"]]
for m in ("PG106AccelDisplayPipe::performTransaction(IOAccelDisplayPipeTransaction2*)",
          "PG106AccelDisplayPipe::isTransactionComplete(IOAccelDisplayPipeTransaction2*)",
          "PG106AccelDisplayPipe::validateTransaction(IOAccelDisplayPipeTransaction2*)"):
    if m not in pipe: sys.exit("FAIL: %s is not in the display pipe's vtable" % m)
print("PASS display pipe: newDisplayPipe, performTransaction, isTransactionComplete, validateTransaction are PG106Accel's")
PY
echo "cross-build: OK — $OUT/PG106Accel.kext"
