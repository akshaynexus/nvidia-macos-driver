#!/bin/sh
# pascal/scripts/edid_corpus_diff.sh — standalone EDID differential harness.
#
# Runs edid-decode(1) and the pascal core EDID parser over every dump in
# research/reference/edid/corpus/ (or just the .bin files given as
# arguments) and prints a field-by-field diff table wherever the overlap
# between the two parsers disagrees. The comparison itself lives in the
# test binary (pascal/tests/test_edid_corpus.c, which documents the
# compared subset); this script builds it with the same flags as the
# Makefile and execs it. Exit status 0 iff every compared field agrees.
#
# Usage:
#   scripts/edid_corpus_diff.sh                # whole corpus
#   scripts/edid_corpus_diff.sh file.bin ...   # specific dumps

set -eu
cd "$(dirname "$0")/.."

command -v edid-decode >/dev/null 2>&1 || {
	echo "edid_corpus_diff.sh: edid-decode not found in PATH" >&2
	exit 2
}

CORPUS=../research/reference/edid/corpus
[ -d "$CORPUS" ] || CORPUS=research/reference/edid/corpus

# Private build dir (never pascal/build/, which concurrent makes share).
BDIR=${BUILD_DIR:-${TMPDIR:-/tmp}/pg106-edid-corpus}
BIN=$BDIR/test_edid_corpus
# Rebuild when missing or stale (same rule + flags as the Makefile; the
# wildcard test rule would also pick the test up via `make`, but building
# here keeps the script self-contained and leaves build/ bookkeeping to it).
if [ ! -x "$BIN" ] || [ tests/test_edid_corpus.c -nt "$BIN" ] ||
	[ core/src/edid.c -nt "$BIN" ] ||
	[ core/include/pascal/edid.h -nt "$BIN" ]; then
	mkdir -p "$BDIR"
	${CC:-cc} ${CFLAGS:--std=c11 -Wall -Wextra -O1 -g} -Icore/include \
		-o "$BIN" tests/test_edid_corpus.c core/src/*.c
fi

if [ "$#" -gt 0 ]; then
	exec "$BIN" "$@"
fi
# Shell glob expansion is sorted, matching the test's internal qsort.
exec "$BIN" "$CORPUS"/*.bin
