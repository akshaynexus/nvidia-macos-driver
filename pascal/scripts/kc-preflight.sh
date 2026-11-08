#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# pascal/scripts/kc-preflight.sh — check the cross-built kexts against the
# REAL kernel collections of the Mac that will load them (macOS 26.7.2 on
# the Helios 300: BootKernelExtensions.kc + SystemKernelExtensions.kc), with
# macos_helios300's tools/kc-audit.py: every import resolves in the
# collection the kext links against, every Apple parent class has the
# size the kext was compiled with (kKcAuditParentSizes), every vtable is
# laid out like the real parent's.
#
#   PG106.kext      OpenCore-injected -> Boot collection only
#   PG106FB.kext    /Library/Extensions (aux KC) -> Boot + System
#                   (IOGraphicsFamily, IONDRVSupport live in System)
#   PG106Accel.kext /Library/Extensions -> Boot + System (IOAcceleratorFamily2)
#
# usage: pascal/scripts/kc-preflight.sh [PG106.kext] [PG106FB.kext] [PG106Accel.kext]
#   (defaults: the three cross-build outputs; PG106Accel's is
#   ${TMPDIR:-/tmp}/PG106Accel-cross/out, kexts/PG106Accel/cross-build.sh's)
#   HELIOS_KC_DIR   directory holding the two .kc files
#                   (default ~/pascal-build/macos-26.7.2-25G332; Apple binaries,
#                   local only, never committed)
#   KC_AUDIT        tools/kc-audit.py (default ../macos_helios300/tools/kc-audit.py)
# Exit 0 = every audit passed; 1 = findings; 77 = collections or tool missing.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KC_DIR=${HELIOS_KC_DIR:-$HOME/pascal-build/macos-26.7.2-25G332}
AUDIT=${KC_AUDIT:-$ROOT/../macos_helios300/tools/kc-audit.py}
BOOT=$KC_DIR/BootKernelExtensions.kc
SYS=$KC_DIR/SystemKernelExtensions.kc
PG106=${1:-$ROOT/pascal/kext/build/cross/PG106.kext}
FB=${2:-$ROOT/pascal/fb/build/cross/PG106FB.kext}
ACCEL=${3:-${TMPDIR:-/tmp}/PG106Accel-cross/out/PG106Accel.kext}
[ -f "$AUDIT" ] || { echo "kc-preflight: SKIP — $AUDIT not found (set KC_AUDIT)"; exit 77; }
[ -f "$BOOT" ] && [ -f "$SYS" ] || { echo "kc-preflight: SKIP — $KC_DIR lacks BootKernelExtensions.kc / SystemKernelExtensions.kc (set HELIOS_KC_DIR)"; exit 77; }
rc=0
run() { # name kext plist kc...
	local name=$1 kext=$2 plist=$3; shift 3
	if [ ! -f "$kext" ]; then echo "kc-preflight: $name: no binary at $kext (not built) — skipped"; return; fi
	echo "== $name against $(for k in "$@"; do basename "$k"; done | paste -sd+ -)"
	local args=(); for k in "$@"; do args+=(--kc "$k"); done
	python3 -I "$AUDIT" "${args[@]}" --kext "$kext" --plist "$plist" || rc=1
}
run PG106 "$PG106/Contents/MacOS/PG106" "$PG106/Contents/Info.plist" "$BOOT"
run PG106FB "$FB/Contents/MacOS/PG106FB" "$FB/Contents/Info.plist" "$BOOT" "$SYS"
run PG106Accel "$ACCEL/Contents/MacOS/PG106Accel" "$ACCEL/Contents/Info.plist" "$BOOT" "$SYS"
# PG106Accel's macOS 26 forwarders (pga_tahoe_fwd.h) are slots the kext
# defines, which kc-audit skips: check that each jumps to its own method in
# the real IOAcceleratorFamily2 vtables (a separate program, PolyForm-NC).
if [ -f "$ACCEL/Contents/MacOS/PG106Accel" ]; then
	echo "== PG106Accel macOS 26 forwarders against the collections' IOAcceleratorFamily2 vtables"
	python3 -I "$ROOT/kexts/PG106Accel/check_tahoe_fwd_kc.py" "$AUDIT" "$BOOT" "$SYS" || rc=1
fi
for k in "$PG106/Contents/MacOS/PG106" "$FB/Contents/MacOS/PG106FB" "$ACCEL/Contents/MacOS/PG106Accel"; do
	[ -f "$k" ] && echo "sha256 $(sha256sum "$k" | cut -c1-16) $(basename "$k")"
done
[ $rc = 0 ] && echo "kc-preflight: PASS" || echo "kc-preflight: FAIL"
exit $rc
