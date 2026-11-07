#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# pascal/scripts/hwtest_smoke.sh — runs tools/pg106_hwtest against a FAKE
# sysfs tree (regular files standing in for resource0/1), to check its
# refusals and plumbing on any Linux box. No GPU is touched.
#
# usage: hwtest_smoke.sh <pg106_hwtest binary>
set -u
BIN=${1:?binary}
T=$(mktemp -d "${TMPDIR:-/tmp}/pg106-smoke.XXXXXX")
trap 'rm -rf "$T"' EXIT
D="$T/0000:01:00.0"
mkdir -p "$D"
printf '0x10de\n' >"$D/vendor"
printf '0x1c20\n' >"$D/device"
printf '1\n' >"$D/enable"
truncate -s 16M "$D/resource0"
truncate -s 1M "$D/resource1"
fail=0
expect() { # expect <exit code> <grep pattern> <args...>
	local want=$1 pat=$2; shift 2
	out=$("$BIN" --sysfs "$T" "$@" 2>&1); got=$?
	if [ "$got" -ne "$want" ] || ! grep -q -- "$pat" <<<"$out"; then
		echo "FAIL: pg106_hwtest $* -> exit $got (want $want), output:"; echo "$out"
		fail=1
	else
		echo "ok: $* -> $want ($pat)"
	fi
}
expect 64 usage
expect 64 usage --go
expect 2 "not a GP106" --probe            # BOOT_0 reads 0 from the fake BAR0
printf '\xa1\x00\x60\x13' | dd of="$D/resource0" conv=notrunc status=none  # 0x136000a1
expect 0 "probe: .* 0 writes" --probe
expect 0 "BOOT_0 0x136000a1 (GP106)" --probe
# unposted fake GPU, no VBIOS: devboot stops at POST, cleanly
expect 1 "stage post" --go --fw "$T/nofw"
printf '0x1c8d\n' >"$D/device"
expect 2 "not the GTX 1060" --probe
printf '0x1c20\n' >"$D/device"
printf '0x1002\n' >"$D/vendor"
expect 2 "not an NVIDIA" --probe
printf '0x10de\n' >"$D/vendor"
ln -s /nonexistent/nvidia "$D/driver"
expect 2 "a driver is bound" --probe
rm "$D/driver"
printf '0\n' >"$D/enable"
expect 2 "decode is off" --probe
[ $fail -eq 0 ] && echo "PASS hwtest_smoke (fake sysfs, no hardware)"
exit $fail
