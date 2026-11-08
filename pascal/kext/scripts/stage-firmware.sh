#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# stage-firmware.sh <outdir> — copy the GP106 firmware the kext requests
# from its bundle (OSKextRequestResource, PG106.cpp requestFirmware) into
# <outdir> under the flat resource names of pascal/core/src/devboot.c
# pg106_devboot_fw_names, e.g. gp106/acr/bl.bin -> gp106-acr-bl.bin.
#
# Sources: research/firmware/<path> (linux-firmware, NVIDIA licence in
# licenses/). A file that is not there is reported and skipped; the kext
# then logs it as missing and the stage that needs it fails.
#
# The Xcode project's Resources phase references <kext>/build/fw-stage/*,
# so build-macos.sh runs this with that directory.
#
# --placeholders: write a zero-length file for each missing blob, so the
# Xcode Resources phase (which fails on a missing file) still builds; the
# kext treats an empty resource as missing.
set -u
OUT="${1:?usage: stage-firmware.sh <outdir> [--placeholders]}"
PLACEHOLDERS="${2:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"
PASCAL="$(cd "$HERE/../.." && pwd)"
FW="$(cd "$PASCAL/../research/firmware" && pwd)"
mkdir -p "$OUT"
n=0
miss=0
while read -r res path; do
	if [ -f "$FW/$path" ]; then
		cp "$FW/$path" "$OUT/$res"
		n=$((n + 1))
	else
		echo "stage-firmware: MISSING $path (resource $res) — the kext will log it" >&2
		miss=$((miss + 1))
		[ "$PLACEHOLDERS" = --placeholders ] && : > "$OUT/$res"
	fi
done < <(sed -nE 's/^[[:space:]]*\{ "(gp106-[^"]+)", "([^"]+)".*/\1 \2/p' \
	"$PASCAL/core/src/devboot.c")
echo "stage-firmware: $n staged, $miss missing -> $OUT"
[ "$n" -gt 0 ]
