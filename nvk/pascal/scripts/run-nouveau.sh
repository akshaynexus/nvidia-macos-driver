#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Runs on the Helios 300 under Linux, from the bundle build-nak-nouveau.sh
# makes: the macOS dylib's NAK (on upstream NVK + nouveau.ko) against the
# real GP106. Read-only towards the system: it does not load, unload or
# rebind any driver and does not touch the desktop. It refuses to start
# unless the GP106 is already bound to nouveau with GR firmware loaded
# (nvk/pascal/CROSS-BUILD.md "Real GP106 under nouveau" says how the owner
# gets there and back).
#
#   1. NAK's hw_tests: 45 compute tests that build instructions with NAK,
#      encode them for the GPU's SM (6.1), run them and compare with a CPU
#      reference.
#   2. pg106_vk_smoke --driver ./libvulkan_nouveau.so (runbook step 3 checks)
#      with NAK_DEBUG=print, so the SM 6.1 code can be compared with the
#      pg106 sim's (nak-sm61-check.py).
#   3. offload_selftest --driver (runbook step 3b, host-import + copy modes).
#
# Usage: sh run-nouveau.sh [outdir]     (default ./results-<date>)
#   Each step has a 120 s timeout. Exit 0 only if all three pass.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/results-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"
log() { echo "$*" | tee -a "$OUT/summary.txt"; }

( cd "$HERE" && sha256sum -c SHA256SUMS --quiet ) || { echo "bundle checksum mismatch"; exit 2; }
# The GP106 (10de:1c..) and the driver bound to it.
DEV=$(for d in /sys/bus/pci/devices/*; do
        [ "$(cat "$d/vendor")" = 0x10de ] && [ "$(cat "$d/class" | cut -c1-4)" = 0x03 ] && echo "$d"; done | head -1)
[ -n "$DEV" ] || { echo "no NVIDIA display device"; exit 2; }
DRV=$(basename "$(readlink "$DEV/driver" 2>/dev/null)" 2>/dev/null)
log "device $(basename "$DEV") $(cat "$DEV/device") driver ${DRV:-none} kernel $(uname -r)"
case $(cat "$DEV/device") in 0x1c[0-7]?) ;;   # GP106 ids (GP107 starts at 0x1c8x)
    *) log "STOP: $(cat "$DEV/device") is not a GP106"; exit 3;; esac
[ "$DRV" = nouveau ] || { log "STOP: the GP106 is not bound to nouveau (driver: ${DRV:-none}); see CROSS-BUILD.md"; exit 3; }
RN=$(ls "$DEV/drm" 2>/dev/null | grep renderD | head -1)
[ -n "$RN" ] && [ -r "/dev/dri/$RN" ] && [ -w "/dev/dri/$RN" ] ||
    { log "STOP: no accessible render node for the GP106 (/dev/dri/$RN; user in group render?)"; exit 3; }
dmesg 2>/dev/null | grep -i nouveau | tail -20 > "$OUT/dmesg-before.txt"
grep -qi "failed to load\|gr: init failed\|firmware.*fail" "$OUT/dmesg-before.txt" &&
    log "warning: nouveau logged a firmware/GR failure (dmesg-before.txt); compute will likely fail"

rc=0
# NAK's runner takes the first NVIDIA DRM device (the Helios has one).
log "1/3 NAK hw_tests"
timeout 600 "$HERE/nak" hw_tests:: --test-threads 1 > "$OUT/nak-hw.log" 2>&1 || rc=1
log "    $(grep '^test result' "$OUT/nak-hw.log" || echo 'no result line (crash/timeout?)')"
grep -E '^test .* FAILED' "$OUT/nak-hw.log" | tee -a "$OUT/summary.txt"

log "2/3 pg106_vk_smoke --driver ./libvulkan_nouveau.so"
NAK_DEBUG=print timeout 120 "$HERE/pg106_vk_smoke" --driver "$HERE/libvulkan_nouveau.so" \
    > "$OUT/smoke.out" 2> "$OUT/smoke-nak.log" || rc=1
tee -a "$OUT/summary.txt" < "$OUT/smoke.out"

log "3/3 offload_selftest --driver ./libvulkan_nouveau.so"
timeout 120 "$HERE/offload_selftest" --driver "$HERE/libvulkan_nouveau.so" > "$OUT/offload.out" 2>&1 || rc=1
grep -E 'PASS|FAIL|offload_selftest' "$OUT/offload.out" | tee -a "$OUT/summary.txt"

dmesg 2>/dev/null | grep -i nouveau | tail -40 > "$OUT/dmesg-after.txt"
log "result: $([ $rc = 0 ] && echo PASS || echo FAIL)  (logs in $OUT)"
exit $rc
