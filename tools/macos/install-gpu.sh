#!/bin/bash
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# install-gpu.sh: put the GTX 1060 (GP106) display + Metal stack on the Helios 300 (macOS 26.7.2 25G332), next to
# the OpenCore-injected PG106.kext, or take it off again. Same shape as macos_helios300 tools/macos/audio-*.sh.
#
# What goes where (owner root:wheel, nothing world-writable):
#   PG106FB.kext, PG106Accel.kext          /Library/Extensions (auxiliary kernel collection: their dependencies,
#                                          IOGraphicsFamily / IOAcceleratorFamily2, exist only in the System KC, so
#                                          OpenCore cannot inject them)
#   NVMTLDriver.bundle                     /Library/GPUBundles (PG106Accel's MetalPluginName)
#   libnvmtl_translate.dylib               /Library/GPUBundles/nvmtl (AIR -> SPIR-V, loaded by the plugin)
#   libvulkan_nouveau.dylib, nvk_icd.json  /Library/GPUBundles/nvmtl (NVK with the pg106 backend)
#   nvmtl-allow.txt                        /Library/GPUBundles (which apps get the GP106; kept if one exists)
#
# Run on the Mac. MODE (default dry-run, which changes nothing persistent):
#   MODE=dry-run   every check; a test auxiliary collection built in a temp dir from /Library/Extensions + the new
#                  kexts (kmutil create, nothing published); prints every command install would run
#   MODE=install   back up what is there, copy, sign ad-hoc what is unsigned, build + publish the auxiliary
#                  collection; takes effect at the next boot (after the approval in System Settings, if asked)
#   MODE=revert    put back the latest backup (or remove what this script installed), the old auxiliary collection
#   MODE=status    what is installed, loaded and published now (ioreg properties of PG106, PG106FB, PG106Accel)
# SRC=dir with the release artifacts (PG106FB.kext, PG106Accel.kext, NVMTLDriver.bundle, libnvmtl_translate.dylib,
#   NVK-Pascal/{libvulkan_nouveau.dylib,nvk_icd.json}); a missing one is skipped. ONLY="PG106FB PG106Accel plugin
#   xlate nvk allow" limits the set (one change per boot: e.g. ONLY=PG106FB first).
# Over SSH: { echo MODE=dry-run SRC=/tmp/gpu; cat tools/macos/install-gpu.sh; } | ~/.config/helios300-ssh/hbmac
# Safety hatches at boot: -x (safe mode) or -pg106off make PG106, PG106FB and PG106Accel stand down; -pg106acceloff
# PG106Accel alone; pg106fb=0 PG106FB alone.
set -euo pipefail
MODE=${MODE:-dry-run}
SRC=${SRC:-}
ONLY=${ONLY:-"PG106FB PG106Accel plugin xlate nvk allow"}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd || echo .)
EXT=/Library/Extensions; GB=/Library/GPUBundles; NV=$GB/nvmtl
AUX=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
KB=/System/Library/KernelCollections/BootKernelExtensions.kc
KS=/System/Library/KernelCollections/SystemKernelExtensions.kc
BKROOT=/Library/Helios300/gpu-backup
PINNED_BUILD=25G332

# stdin from /dev/null: run over SSH as `{ echo MODE=...; cat install-gpu.sh; } | hbmac`, the rest of this script is
# on stdin, and asroot (sudo -A) hands its stdin to the command, which would swallow the script (kmutil, ditto...)
R() { if declare -F asroot >/dev/null; then asroot "$@" </dev/null; else sudo "$@" </dev/null; fi; }
say() { printf '%s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }
die() { printf 'STOP: %s\n' "$*" >&2; exit 1; }
live() { [ "$MODE" = install ] || [ "$MODE" = revert ]; }
run() { if live; then say "+ $*"; R "$@"; else say "would run: $*"; fi; }
want() { case " $ONLY " in *" $1 "*) return 0;; esac; return 1; }
sha() { shasum -a 256 "$1" | awk '{print substr($1,1,16)}'; }

# name | source (relative to SRC) | target
ITEMS=(
	"PG106FB|PG106FB.kext|$EXT/PG106FB.kext"
	"PG106Accel|PG106Accel.kext|$EXT/PG106Accel.kext"
	"plugin|NVMTLDriver.bundle|$GB/NVMTLDriver.bundle"
	"xlate|libnvmtl_translate.dylib|$NV/libnvmtl_translate.dylib"
	"nvk|NVK-Pascal/libvulkan_nouveau.dylib|$NV/libvulkan_nouveau.dylib"
	"nvk|NVK-Pascal/nvk_icd.json|$NV/nvk_icd.json"
)

WORK=$(mktemp -d "${TMPDIR:-/tmp}/install-gpu.XXXXXX")
cleanup() { R rm -rf "$WORK" 2>/dev/null || rm -rf "$WORK" 2>/dev/null || true; }
trap cleanup EXIT

ioprop() { # class key
	ioreg -r -c "$1" -d 1 2>/dev/null | sed -n "s/^ *\"$2\" = //p" | head -1
}

status() {
	step "macOS and the GPU kexts"
	say "build $(sw_vers -buildVersion), Darwin $(uname -r); boot-args: $(nvram boot-args 2>/dev/null | cut -f2- || echo '?')"
	kmutil showloaded </dev/null 2>/dev/null | grep -E 'org\.helios300\.pg106|com\.nullmoth\.PG106Accel|com\.nullmoth\.NV(RM|Accel)' \
		| sed 's/^/  loaded: /' || say "  none of PG106 / PG106FB / PG106Accel loaded"
	step "PG106 (OpenCore-injected)"
	for k in PG106Ready PG106Stage PG106Error PG106FirmwareEmbedded PG106BootMs PG106SelfTest PG106LastSleep PG106LastWake \
		PG106WakeMs PG106WakeResult; do v=$(ioprop PG106 "$k"); [ -z "$v" ] || say "  $k = $v"; done
	step "PG106FB"
	for k in PG106FBAccelerated PG106FBKifABI IOFBConnectFlags; do v=$(ioprop PG106FB "$k"); [ -z "$v" ] || say "  $k = $v"; done
	ioreg -r -c PG106FB -d 1 >/dev/null 2>&1 || say "  not attached"
	step "PG106Accel"
	for k in PG106AccelState PG106AccelMode PG106AccelDisplay PG106AccelFramePath MetalPluginName PG106AccelVRAMBytes; do
		v=$(ioprop PG106Accel "$k"); [ -z "$v" ] || say "  $k = $v"; done
	ioreg -r -c PG106Accel -d 1 >/dev/null 2>&1 || say "  not attached"
	step "Files"
	for it in "${ITEMS[@]}"; do
		IFS='|' read -r _ _ t <<< "$it"
		if [ -e "$t" ]; then
			local bin=$t; [ -d "$t" ] && bin=$(find "$t/Contents/MacOS" -type f -maxdepth 1 2>/dev/null | head -1)
			say "  $t  $( [ -f "$bin" ] && sha "$bin")  $(stat -f '%Su:%Sg %Lp' "$t")  $(codesign -dv "$t" 2>&1 | grep -oE 'Signature=adhoc|Authority=[^,]*' | head -1)"
		else say "  $t  (absent)"; fi
	done
	[ -f "$GB/nvmtl-allow.txt" ] && say "  allow list: $(grep -cvE '^[[:space:]]*(#|$)' "$GB/nvmtl-allow.txt") rules" || say "  allow list: absent"
	say "  backups: $(ls -1 "$BKROOT" 2>/dev/null | tr '\n' ' ')"
	say "plugin log: log show --last 5m --style compact --predicate 'eventMessage CONTAINS \"NVMTL\" OR eventMessage CONTAINS \"NullMoth\"'"
}

preflight() {
	step "macOS"
	local build; build=$(sw_vers -buildVersion)
	say "build $build, $(uname -m)"
	[ "$(uname -m)" = x86_64 ] || die "not an Intel Mac"
	[ "$build" = "$PINNED_BUILD" ] || [ "${FORCE:-0}" = 1 ] || die "pinned to $PINNED_BUILD (kc-preflight ran against its collections); running $build (FORCE=1 overrides)"
	local csr; csr=$(csrutil status 2>/dev/null | head -1); say "SIP: $csr"
	step "PG106 must be up first (PG106FB and PG106Accel attach to it)"
	local ready; ready=$(ioprop PG106 PG106Ready)
	say "PG106Ready = ${ready:-absent}, PG106Stage = $(ioprop PG106 PG106Stage)"
	[ "$ready" = Yes ] || die "PG106 is not ready: install nothing on top of it"
	step "Conflicts"
	for k in NVRM NVAccel NVRMFB NVRMAGDC; do
		[ ! -e "$EXT/$k.kext" ] || die "$EXT/$k.kext is installed: the NVRM stack (another product) owns NVMTLDriver.bundle too; remove it first"
	done
	say "no NVRM stack"
	step "Sources ($SRC)"
	[ -n "$SRC" ] && [ -d "$SRC" ] || die "SRC= the directory with the release artifacts"
	for it in "${ITEMS[@]}"; do
		IFS='|' read -r n s t <<< "$it"; want "$n" || continue
		if [ -e "$SRC/$s" ]; then
			local bin="$SRC/$s"; [ -d "$bin" ] && bin=$(find "$SRC/$s/Contents/MacOS" -type f -maxdepth 1 | head -1)
			local sig=unsigned; codesign --verify --strict "$SRC/$s" >/dev/null 2>&1 && sig=signed
			say "  $s  sha256 $(sha "$bin")  $sig"
		else say "  $s  (not in SRC: skipped)"; fi
	done
	if want PG106FB || want PG106Accel; then
		step "Test auxiliary collection (temp dir; nothing published)"
		R mkdir -p "$WORK/repo"
		R ditto "$EXT" "$WORK/repo"
		for n in PG106FB PG106Accel; do
			want "$n" && [ -d "$SRC/$n.kext" ] && { R rm -rf "$WORK/repo/$n.kext"; R ditto "$SRC/$n.kext" "$WORK/repo/$n.kext"; }
		done
		# macOS 26 kmutil skips kexts not owned by root; ad-hoc signature for any unsigned one
		R chown -R root:wheel "$WORK/repo"; R chmod -R go-w "$WORK/repo"
		for n in PG106FB PG106Accel; do
			[ -d "$WORK/repo/$n.kext" ] || continue
			codesign --verify --strict "$WORK/repo/$n.kext" >/dev/null 2>&1 || R codesign --force --sign - "$WORK/repo/$n.kext"
		done
		if R kmutil create -n aux --volume-root / --allow-missing-kdk -B "$KB" -S "$KS" --repository "$WORK/repo" \
			-A "$WORK/aux.kc" -z >"$WORK/kmutil.log" 2>&1; then
			local ins; ins=$(kmutil inspect -a x86_64 -A "$WORK/aux.kc" </dev/null 2>/dev/null || true)
			for id in org.helios300.pg106fb com.nullmoth.PG106Accel; do
				printf '%s' "$ins" | grep -q "$id" && say "  $id: in the test collection" || say "  $id: NOT in the test collection"
			done
		else
			tail -15 "$WORK/kmutil.log"; die "kmutil refused the test collection (log above)"
		fi
	fi
}

install_all() {
	local bk; bk="$BKROOT/$(date +%Y%m%d-%H%M%S)"
	step "Backup to $bk"
	run mkdir -p "$bk"
	for it in "${ITEMS[@]}"; do
		IFS='|' read -r n s t <<< "$it"; want "$n" || continue; [ -e "$SRC/$s" ] || continue
		if [ -e "$t" ]; then run ditto "$t" "$bk/$(basename "$t")"; else run touch "$bk/$(basename "$t").absent"; fi
	done
	[ -f "$GB/nvmtl-allow.txt" ] || run touch "$bk/nvmtl-allow.txt.absent"
	[ -f "$AUX" ] && run cp -p "$AUX" "$bk/AuxiliaryKernelExtensions.kc" || run touch "$bk/AuxiliaryKernelExtensions.kc.absent"
	step "Copy"
	run mkdir -p "$NV"
	for it in "${ITEMS[@]}"; do
		IFS='|' read -r n s t <<< "$it"; want "$n" || continue; [ -e "$SRC/$s" ] || continue
		run rm -rf "$t"
		run ditto "$SRC/$s" "$t"
		run chown -R root:wheel "$t"
		if [ -d "$t" ]; then run chmod -R 755 "$t"; else case "$t" in *.dylib) run chmod 755 "$t";; *) run chmod 644 "$t";; esac; fi
		# dyld and kmutil accept ad-hoc signatures; an unsigned artifact is signed here
		if live && ! codesign --verify --strict "$t" >/dev/null 2>&1; then run codesign --force --sign - "$t"; fi
	done
	if want allow; then
		if [ -f "$GB/nvmtl-allow.txt" ]; then say "allow list kept ($GB/nvmtl-allow.txt)"
		else
			run cp "$HERE/nvmtl-allow.default.txt" "$GB/nvmtl-allow.txt"
			run chown root:wheel "$GB/nvmtl-allow.txt"; run chmod 644 "$GB/nvmtl-allow.txt"
		fi
	fi
	if want PG106FB || want PG106Accel; then
		step "Auxiliary kernel collection (published; loads at the next boot)"
		run kmutil create -n aux --volume-root / --allow-missing-kdk -B "$KB" -S "$KS" --repository "$EXT" -A "$WORK/aux.kc" -z
		if live; then
			kmutil inspect -a x86_64 -A "$WORK/aux.kc" </dev/null >/dev/null 2>&1 || die "the new collection does not inspect (backup in $bk)"
		fi
		run cp "$WORK/aux.kc" "$AUX"
	fi
	say ""
	say "Next: reboot. If System Settings > Privacy & Security says system software was blocked, allow it and reboot again."
	say "Check with MODE=status. Revert with MODE=revert (backup $bk); at the boot menu -x or -pg106off bypasses all three."
}

revert_all() {
	local bk; bk=$(ls -1d "$BKROOT"/* 2>/dev/null | tail -1)
	[ -n "$bk" ] || die "no backup in $BKROOT"
	step "Revert from $bk"
	for it in "${ITEMS[@]}"; do
		IFS='|' read -r n s t <<< "$it"; b="$bk/$(basename "$t")"
		if [ -e "$b" ]; then run rm -rf "$t"; run ditto "$b" "$t"
		elif [ -e "$b.absent" ]; then run rm -rf "$t"; fi
	done
	[ ! -e "$bk/nvmtl-allow.txt.absent" ] || run rm -f "$GB/nvmtl-allow.txt"
	if [ -f "$bk/AuxiliaryKernelExtensions.kc" ]; then run cp -p "$bk/AuxiliaryKernelExtensions.kc" "$AUX"
	elif [ -e "$bk/AuxiliaryKernelExtensions.kc.absent" ]; then run rm -f "$AUX"; fi
	say "Reverted; reboot."
}

# The whole dispatch reads /dev/null (see R): bash has read this case statement before it runs, and resumes reading
# the script (or what an SSH caller sent after it) from its own stdin afterwards.
case "$MODE" in
	status) status ;;
	dry-run) preflight; install_all ;;
	install) preflight; install_all ;;
	revert) revert_all ;;
	*) die "MODE=dry-run|install|revert|status" ;;
esac </dev/null
