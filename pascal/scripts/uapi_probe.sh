#!/bin/sh
# SPDX-License-Identifier: MIT
#
# pascal/scripts/uapi_probe.sh - READ-ONLY uapi oracle probe.
#
# Answers, with raw evidence for research/evidence/UAPI-ORACLE.md:
#   1. Is the nouveau kernel driver bound to this box's NVIDIA GPU?
#   2. Do nouveau render nodes exist (live-oracle preconditions)?
#   3. What does the oracle test report (DRM_IOCTL_VERSION + GETPARAM)?
#
# SAFETY (binding, see UAPI-ORACLE.md): this script never loads/unloads
# or rebinds drivers, never writes sysfs, never takes DRM master, never
# touches MMIO. It reads /sys and /dev/dri metadata and runs the oracle
# test binary, which issues only the ioctls any normal app would
# (DRM_IOCTL_VERSION, NOUVEAU_GETPARAM). If nouveau is not bound, the
# finding is recorded and nothing is attempted to "fix" it.

set -u

echo "== uapi_probe.sh $(date -u +%Y-%m-%dT%H:%M:%SZ) =="
echo

echo "--- /dev/dri ---"
ls -l /dev/dri/ 2>&1
echo

echo "--- driver binding per card (readlink of /sys/class/drm/*/driver) ---"
for d in /sys/class/drm/card[0-9]*; do
    [ -e "$d" ] || continue
    dev=$(readlink -f "$d/device" 2>/dev/null)
    drv=$(basename "$(readlink -f "$d/driver" 2>/dev/null)" 2>/dev/null)
    ids=$(grep -H '' "$dev/vendor" "$dev/device" 2>/dev/null | tr '\n' ' ')
    echo "$d driver=${drv:-<none>} $ids"
done
echo

echo "--- render node owners ---"
for r in /sys/class/drm/renderD*; do
    [ -e "$r" ] || continue
    dev=$(readlink -f "$r/device" 2>/dev/null)
    drv=$(readlink "$dev/driver" 2>/dev/null)
    pci=$(basename "$dev" 2>/dev/null)
    echo "$r pci=$pci driver=$(basename "${drv:-<none>}")"
done
echo

echo "--- loaded kernel modules ---"
if lsmod 2>/dev/null | grep -E 'nouveau' >/dev/null; then
    lsmod | grep -E 'nouveau'
else
    echo "nouveau NOT loaded"
fi
if lsmod 2>/dev/null | grep -E '^nvidia ' >/dev/null; then
    echo "(proprietary NVIDIA modules loaded:)"
    lsmod | grep -E '^nvidia' | awk '{print $1}'
fi
echo

echo "--- lspci class 0x03 kernel drivers (read-only) ---"
if command -v lspci >/dev/null 2>&1; then
    lspci -d ::03xx -k 2>/dev/null
else
    echo "lspci not installed; skipping"
fi
echo

echo "--- oracle test binary ---"
BIN=$(dirname "$0")/../build/test_uapi_oracle
if [ -x "$BIN" ]; then
    "$BIN"
else
    echo "build/test_uapi_oracle not built yet (make -C pascal all); "
    echo "running compile-time ABI oracle via cc one-shot instead:"
    CC=${CC:-cc}
    "$CC" -std=c11 -Wall -Wextra -I "$(dirname "$0")/../core/include" \
        -o /tmp/opencode-uapi-oracle "$(dirname "$0")/../tests/test_uapi_oracle.c" \
        "$(dirname "$0")"/../core/src/*.c 2>/tmp/opencode-uapi-oracle-build.log \
        && /tmp/opencode-uapi-oracle
    rc=$?
    [ $rc -ne 0 ] && cat /tmp/opencode-uapi-oracle-build.log
    rm -f /tmp/opencode-uapi-oracle /tmp/opencode-uapi-oracle-build.log
    exit $rc
fi
