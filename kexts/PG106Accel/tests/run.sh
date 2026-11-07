#!/bin/bash
# NullMoth NVIDIA driver for macOS
# Copyright (c) 2026 NullMoth Systems.
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# run.sh: host tests for PG106Accel's decision logic (pga_logic.h) and the plugin's WindowServer rule
# (plugin/nvmtl_ws_gpu.h). Logic only: no IOKit, no GPU. Exit 0 = pass.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
OUT="${TMPDIR:-/tmp}/pga-tests.$$"
mkdir -p "$OUT"; trap 'rm -rf "$OUT"' EXIT
CC="${CC:-cc}"
for std in c99 c11; do
	"$CC" -std=$std -Wall -Wextra -Werror -I "$HERE/.." -I "$REPO/pascal/bridge/include" -I "$REPO/plugin" \
		"$HERE/test_pga_logic.c" -o "$OUT/t_$std"
	"$OUT/t_$std"
done
# the same headers as C++ (the kext includes pga_logic.h from C++)
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -x c++ -I "$HERE/.." -I "$REPO/pascal/bridge/include" -I "$REPO/plugin" \
	"$HERE/test_pga_logic.c" -o "$OUT/t_cxx"
"$OUT/t_cxx"
