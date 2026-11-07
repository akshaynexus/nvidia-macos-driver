#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# capture_post_trace.sh — OWNER-RUN on the Helios 300 under Linux (as root).
#
# Records a hardware mmiotrace of nouveau POSTing the GP106 through the VBIOS
# PMU DEVINIT app (gm200_devinit_post, forced with NvForcePost=1), plus its
# ACR/SEC2/GR boot. This is the "known-good register-write trace" for the
# Phase 1 gate (HANDOFF-PASCAL.md §6) and first hardware evidence for
# Phase 2 / BLOCKERS B10.
#
# What it does to the machine (all undone by the reboot at the end):
#   - stops the display manager (ENDS THE GRAPHICAL SESSION — save work first)
#   - unloads the NVIDIA proprietary modules
#   - loads nouveau (normally blacklisted) under mmiotrace
# The internal panel is on the Intel GPU and keeps working on the console.
#
# Usage:   sudo ./capture_post_trace.sh [outdir]      (default /var/tmp/posttrace)
# Rollback: reboot (the script offers it); nothing is written to disk config.
set -u
O=${1:-/var/tmp/posttrace}
T=/sys/kernel/tracing
DEV=${PG106_PCI:-0000:01:00.0}
DM=${PG106_DM:-gdm}

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
[ -d "$T" ] || mount -t tracefs nodev "$T"
grep -q mmiotrace "$T/available_tracers" || { echo "kernel lacks CONFIG_MMIOTRACE"; exit 1; }
modinfo nouveau >/dev/null || { echo "nouveau module missing"; exit 1; }
mkdir -p "$O"
exec > >(tee "$O/run.log") 2>&1
set -x
date; uname -r
lspci -vvnn -s "${DEV#0000:}" > "$O/lspci.txt"
# BAR0 physical base, needed to turn mmiotrace physical addresses into offsets.
awk '/Region 0: Memory at/ {print "0x"$5; exit}' "$O/lspci.txt" > "$O/bar0.txt"
cat "$O/bar0.txt"

systemctl stop "$DM"; sleep 6
for m in nvidia_uvm nvidia_drm nvidia_modeset nvidia; do
	lsmod | grep -q "^$m " || continue
	rmmod "$m" || { sleep 3; rmmod "$m"; }
done
if lsmod | grep -q '^nvidia'; then
	echo "ABORT: NVIDIA modules still in use; restarting $DM"
	systemctl start "$DM"; exit 1
fi

echo 128000 > "$T/buffer_size_kb"
echo mmiotrace > "$T/current_tracer"
cat "$T/trace_pipe" > "$O/mmio.trace" &
CATPID=$!
sleep 2
dmesg -c > "$O/dmesg-before.txt"
modprobe nouveau config=NvForcePost=1 runpm=0
sleep 30
echo nop > "$T/current_tracer"
sleep 2; kill "$CATPID"
dmesg > "$O/dmesg-nouveau.txt"
for d in /sys/kernel/debug/dri/*/; do [ -f "$d/vbios.rom" ] && cp "$d/vbios.rom" "$O/"; done
lsmod | grep nouveau
wc -l "$O/mmio.trace"
grep -c "^W " "$O/mmio.trace" || true
grep -iE "nouveau.*(devinit|acr|sec2|gr|fail|error|timeout)" "$O/dmesg-nouveau.txt" | head -40
chmod -R a+r "$O"
set +x
echo
echo "Captured into $O. Copy it off the machine, then reboot to restore the"
echo "NVIDIA driver and the desktop:   systemctl reboot"
