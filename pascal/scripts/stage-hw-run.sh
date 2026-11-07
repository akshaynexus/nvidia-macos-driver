#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# pascal/scripts/stage-hw-run.sh — stage a hardware run of pg106_hwtest:
# builds a static pg106_hwtest from a clean `git archive` of COMMIT (default
# HEAD; uncommitted work in the tree is NOT included), copies the gp106
# firmware and the Helios VBIOS, installs this tree's scripts/hw-run.sh as
# run.sh with
# RUN_NAME baked in, and writes BUILD (commit, binary sha256) and SHA256SUMS.
#
# usage (from the repo root): pascal/scripts/stage-hw-run.sh NAME [COMMIT]
#   -> ~/pascal-build/NAME/ (override with OUT=dir)
# Then copy the directory to the laptop as /tmp/pg106/NAME and pipe run.sh
# through hb (pascal/scripts/hw-run.sh header).
set -eu
NAME=${1:?usage: stage-hw-run.sh NAME [COMMIT]}
COMMIT=${2:-HEAD}
ROOT=$(git rev-parse --show-toplevel)
OUT=${OUT:-$HOME/pascal-build/$NAME}
ROM=$ROOT/research/firmware/vbios/helios300-gp106.rom
[ -f "$ROM" ] || { echo "missing $ROM (research/firmware/vbios/README.md)" >&2; exit 1; }
[ -e "$OUT" ] && { echo "$OUT exists; pick a new NAME" >&2; exit 1; }
SRC=$(mktemp -d "${TMPDIR:-/tmp}/pg106-stage.XXXXXX")
trap 'rm -rf "$SRC"' EXIT
git -C "$ROOT" archive "$COMMIT" pascal research/firmware/gp106 | tar -x -C "$SRC"
cc -std=c11 -O2 -static -I"$SRC/pascal/core/include" -I"$SRC/pascal/bridge/include" \
	-o "$SRC/pg106_hwtest" "$SRC/pascal/tools/pg106_hwtest.c" "$SRC"/pascal/core/src/*.c
mkdir -p "$OUT/firmware"
cp "$SRC/pg106_hwtest" "$OUT/"
cp -r "$SRC/research/firmware/gp106" "$OUT/firmware/"
cp "$ROM" "$OUT/vbios.rom"
sed "s/^RUN_NAME=\${RUN_NAME:-hw-run}/RUN_NAME=\${RUN_NAME:-$NAME}/" \
	"$ROOT/pascal/scripts/hw-run.sh" > "$OUT/run.sh" # runner: this tree's
chmod +x "$OUT/run.sh"
{
	echo "commit $(git -C "$ROOT" rev-parse "$COMMIT")"
	echo "pg106_hwtest sha256 $(sha256sum "$OUT/pg106_hwtest" | cut -d' ' -f1)"
} > "$OUT/BUILD"
(cd "$OUT" && find . -type f ! -name SHA256SUMS | sort | xargs sha256sum > SHA256SUMS)
cat "$OUT/BUILD"
echo "staged: $OUT"
