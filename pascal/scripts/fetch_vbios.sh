#!/bin/bash
# SPDX-License-Identifier: MIT
# pascal/scripts/fetch_vbios.sh — download the real GP10x VBIOS images used by
# tests/test_vbios_real.c and tests/test_devinit_real.c into
# research/firmware/vbios/, carve them, and verify each sha256.
#
# The images are NVIDIA firmware from the TechPowerUp VBIOS database and are
# not redistributed in this repository (research/firmware/vbios/README.md has
# provenance). TechPowerUp's download endpoint rejects scripted clients, so
# this fetches the Internet Archive's raw (`id_`) captures of the same URLs,
# exactly as the images were originally obtained.
#
# Usage: pascal/scripts/fetch_vbios.sh [--force]
#   PG106_VBIOS_DIR overrides the destination directory.
#   Existing files whose sha256 already matches are left alone; --force
#   re-downloads everything. Exit 0 = every image present and verified.
#
# Needs: curl, python3, sha256sum (or shasum -a 256).

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="${PG106_VBIOS_DIR:-$(cd "$HERE/../.." && pwd)/research/firmware/vbios}"
FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

# file | Wayback capture timestamp |
# original TechPowerUp URL | sha256 of the carved expansion ROM
IMAGES=(
	"nvidia-gtx1060-3gb-tpu208872.rom|20190513122741|https://www.techpowerup.com/vgabios/208872/NVIDIA.GTX10603GB.3072.181212.rom|956a83578de785e19b40cc3d197d47e3e3e2edc1e22c040fb3b96a49c187837e"
	"gigabyte-gtx1060-6gb-tpu186364.rom|20220711215054|https://www.techpowerup.com/vgabios/186364/Gigabyte.GTX1060.6144.160815.rom|680decacefcda1a19738ffdafc382d5748ad520a57cd3d26a7561328b3594f1c"
	"asus-gtx1050ti-tpu188459.rom|20190821024918|https://www.techpowerup.com/vgabios/188459/Asus.GTX1050Ti.4096.161020.rom|ac47f97f7e2080e08305349fac03fa12d24d78f5ace3e7eda1b1e035ba636248"
	"msi-gtx1050-tpu193426.rom|20230314181441|https://www.techpowerup.com/vgabios/193426/MSI.GTX1050.2048.170109.rom|634237a26b5f6366ab3341dab8cab98d6de727b7dd883763744814b75556cd3e"
	"msi-gtx1050ti-mobile-tpu241139.rom|20220614191824|https://www.techpowerup.com/vgabios/241139/MSI.GTX1050Ti.4096.171123.rom|8e226bd93e442070b7d276cc6be8a81f725f3cdf0ff4665efcaaaec92176f328"
)

sha256() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d' ' -f1
	else
		shasum -a 256 "$1" | cut -d' ' -f1
	fi
}

# Carve the expansion ROM out of a capture: from the first 0x55AA whose
# PCIR pointer (u16 at +0x18) lands on a "PCIR" structure, for the PCIR
# image length (u16 at PCIR+0x10, in 512-byte units). Plain ROMs come out
# unchanged; NVGI-wrapped flash dumps lose their 0xa00 header and tail.
CARVE='
import sys
data = open(sys.argv[1], "rb").read()
i = 0
while True:
    i = data.find(b"\x55\xaa", i)
    if i < 0:
        sys.exit("no expansion ROM (55aa + PCIR) in capture")
    p = i + int.from_bytes(data[i + 0x18:i + 0x1a], "little")
    if data[p:p + 4] == b"PCIR":
        n = int.from_bytes(data[p + 0x10:p + 0x12], "little") * 512
        if n and i + n <= len(data):
            open(sys.argv[2], "wb").write(data[i:i + n])
            sys.exit(0)
    i += 2
'

mkdir -p "$DEST"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fail=0
for row in "${IMAGES[@]}"; do
	IFS='|' read -r file ts url want <<<"$row"
	out="$DEST/$file"
	if [ "$FORCE" = 0 ] && [ -f "$out" ] && [ "$(sha256 "$out")" = "$want" ]; then
		echo "OK    $file (already present)"
		continue
	fi
	raw="$WORK/$file.capture"
	if ! curl -fsSL --retry 3 -o "$raw" "https://web.archive.org/web/${ts}id_/${url}"; then
		echo "FAIL  $file: download failed"
		fail=1
		continue
	fi
	if ! python3 -I -c "$CARVE" "$raw" "$WORK/$file"; then
		echo "FAIL  $file: could not carve an expansion ROM"
		fail=1
		continue
	fi
	got="$(sha256 "$WORK/$file")"
	if [ "$got" != "$want" ]; then
		echo "FAIL  $file: sha256 $got, expected $want"
		fail=1
		continue
	fi
	mv "$WORK/$file" "$out"
	echo "OK    $file (fetched, sha256 verified)"
done

if [ "$fail" != 0 ]; then
	echo "fetch_vbios: some images are missing; the real-ROM tests will skip them"
	exit 1
fi
echo "fetch_vbios: all images present in $DEST"
