#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# pascal/scripts/hw-run.sh — the hardware run of pg106_hwtest on the Helios
# GP106 (10de:1c20), as used for hw-run2..6 (2026-11-08; run 6 passed all
# three stages). Three stages, each on a FRESH GPU (D3cold cycle first):
#   A  POST only            --stop-after 2  (PMU falcon reset + DEVINIT)
#   B  POST + PTOP + FIFO   --stop-after 3  + selftest (host semaphore, CE copy)
#   C  full bring-up        (mc init, ACR/SEC2, GR, READY) + selftest
# Stops at the first failing stage. Always restores the nvidia stack + gdm
# (after a last D3cold cycle so nvidia gets a GPU with no WPR lock).
#
# Staged by scripts/stage-hw-run.sh NAME (binary + firmware + VBIOS +
# SHA256SUMS in one directory; RUN_NAME baked in). Run on the laptop
# through hb (stdin); root steps go through `asroot`. RUN_DIR (default
# /tmp/pg106/$RUN_NAME) must hold the staged files. Logs go to
# ~/pg106-logs/$RUN_NAME-<time>/ (override with LOG=). C_ARGS adds
# pg106_hwtest arguments to stage C (e.g. C_ARGS=--gr).
# Never run without the owner's/coordinator's go: it stops gdm and unloads
# the nvidia driver for the duration.
set -u
RUN_NAME=${RUN_NAME:-hw-run}
RUN_DIR=${RUN_DIR:-/tmp/pg106/$RUN_NAME}
C_ARGS=${C_ARGS:-}
BDF=0000:01:00.0
DEV=${PG106_FAKE_DEV:-/sys/bus/pci/devices/$BDF} # PG106_FAKE_DEV: dry-run tests only
# Logs survive a watchdog reboot (not under /tmp); synced after each stage.
LOG=${LOG:-$HOME/pg106-logs/$RUN_NAME-$(date +%Y%m%d-%H%M%S)}
NV_MODS="nvidia_uvm nvidia_drm nvidia_modeset nvidia"
export LC_ALL=C

say() { printf '%s %s\n' "$(date +%T)" "$*"; }
die() { say "ABORT: $*"; exit 1; }
command -v asroot >/dev/null 2>&1 || die "asroot not available (run through hb)"

cd "$RUN_DIR" 2>/dev/null || die "no $RUN_DIR"
mkdir -p "$LOG"
exec > >(tee -a "$LOG/run.log") 2>&1
say "== $RUN_NAME on $(uname -n) $(uname -r)"
sha256sum -c SHA256SUMS --quiet || die "SHA256SUMS mismatch"
say "SHA256SUMS OK"
[ "$(cat $DEV/vendor) $(cat $DEV/device)" = "0x10de 0x1c20" ] || die "$BDF is not 10de:1c20"

DMESG0=$(asroot dmesg | wc -l)
GDM_WAS=$(systemctl is-active gdm 2>/dev/null)
LOADED=""
for m in $NV_MODS; do lsmod | grep -q "^$m " && LOADED="$LOADED $m"; done
say "gdm was $GDM_WAS; nvidia modules loaded:${LOADED:- none}"

pm_state() {
	printf 'runtime=%s power=%s control=%s enable=%s driver=%s' \
		"$(cat $DEV/power/runtime_status 2>/dev/null)" \
		"$(cat $DEV/power_state 2>/dev/null)" \
		"$(cat $DEV/power/control 2>/dev/null)" \
		"$(cat $DEV/enable 2>/dev/null)" \
		"$( [ -e $DEV/driver ] && basename "$(readlink $DEV/driver)" || echo none)"
}

# Let the driverless GPU runtime-suspend into D3cold, up to 40 s.
d3cold_cycle() {
	local i
	asroot sh -c "echo 0 > $DEV/enable" 2>/dev/null
	asroot sh -c "echo auto > $DEV/power/control"
	for i in $(seq 1 40); do
		if [ "$(cat $DEV/power/runtime_status)" = suspended ] &&
		   [ "$(cat $DEV/power_state 2>/dev/null)" = D3cold ]; then
			say "D3cold after ${i}s ($(pm_state))"
			return 0
		fi
		sleep 1
	done
	say "WARNING: no D3cold within 40 s ($(pm_state))"
	return 1
}

wake() {
	asroot sh -c "echo on > $DEV/power/control"
	sleep 1
	asroot sh -c "echo 1 > $DEV/enable"
	sleep 0.5
	say "woken: $(pm_state)"
	[ "$(cat $DEV/power_state 2>/dev/null)" = D0 ] || return 1
}

restore() {
	trap - EXIT
	say "== restore"
	[ -e $DEV/driver ] || d3cold_cycle
	asroot sh -c "echo auto > $DEV/power/control"
	for m in $(echo $LOADED | tr ' ' '\n' | tac); do asroot modprobe "$m" || say "modprobe $m failed"; done
	[ "$GDM_WAS" = active ] && { asroot systemctl start gdm || say "gdm start failed"; }
	sleep 2
	say "after restore: $(pm_state); gdm $(systemctl is-active gdm); modules: $(lsmod | awk '/^nvidia/{printf "%s ", $1}')"
	asroot dmesg | tail -n +$((DMESG0 + 1)) > "$LOG/dmesg.txt"
	say "dmesg since start: $(wc -l < "$LOG/dmesg.txt") lines -> $LOG/dmesg.txt; GPU-related:"
	grep -iE 'nvidia|nouveau|01:00|NVRM|pcieport|AER|vga_switcheroo' "$LOG/dmesg.txt" | tail -40
	say "== $RUN_NAME result: $RESULT (logs: $LOG)"
	sync
}
RESULT="incomplete"
trap restore EXIT

# --- quiesce --------------------------------------------------------------
[ "$GDM_WAS" = active ] && { asroot systemctl stop gdm || die "cannot stop gdm"; sleep 5; }
for m in $NV_MODS; do
	if lsmod | grep -q "^$m "; then asroot rmmod "$m" || die "rmmod $m failed (in use?)"; fi
done
lsmod | grep -qE '^(nvidia|nouveau) ' && die "nvidia/nouveau still loaded"
[ -e $DEV/driver ] && die "a driver is still bound to $BDF"
say "quiesced: $(pm_state)"

FW="$RUN_DIR/firmware"
ROM="$RUN_DIR/vbios.rom"
BIN="$RUN_DIR/pg106_hwtest"

stage() { # stage <name> <hwtest --go args...>
	local name=$1; shift
	say "== stage $name: D3cold cycle"
	if ! d3cold_cycle; then
		[ "$name" = C ] && { RESULT="stage C not run: no D3cold (WPR may be locked)"; exit 1; }
	fi
	wake || { RESULT="stage $name: GPU did not return to D0"; exit 1; }
	say "-- probe (read-only)"
	asroot timeout 30 "$BIN" --probe --dev $BDF 2>&1 | tee "$LOG/$name-probe.txt"
	say "-- pg106_hwtest --go $*"
	asroot timeout 180 "$BIN" --go --dev $BDF --fw "$FW" --vbios "$ROM" "$@" 2>&1 | tee "$LOG/$name.txt"
	local rc=${PIPESTATUS[0]}
	say "stage $name exit $rc"
	asroot timeout 30 "$BIN" --probe --dev $BDF > "$LOG/$name-probe-after.txt" 2>&1
	grep -E 'posted|PMC_ENABLE|FIFO|BAR1 bind|SEC2' "$LOG/$name-probe-after.txt"
	sync
	return $rc
}

stage A --stop-after 2 || { RESULT="FAIL at stage A (POST)"; exit 1; }
stage B --stop-after 3 --timeout-ms 1000 || { RESULT="FAIL at stage B (TOP/FIFO/CE selftest); A passed"; exit 1; }
stage C --timeout-ms 1000 $C_ARGS || { RESULT="FAIL at stage C (ACR/SEC2/GR/READY); A and B passed"; exit 1; }
RESULT="PASS: A (POST), B (FIFO + CE selftest), C (full bring-up + selftest)"
exit 0
