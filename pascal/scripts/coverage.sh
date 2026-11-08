#!/usr/bin/env bash
# coverage.sh — merged gcov report over ALL pascal test binaries.
#
# Why this exists: every tests/test_*.c binary compiles all of core/src into
# itself, so each source has one .gcda per binary (build/<bin>-<src>.gcda).
# Feeding gcov several .gcda for the same source at once does NOT accumulate
# counts — verified empirically on GCC 16.2.1 (a 5-file invocation reports the
# single-binary number and exits 0). So the merge is done with `gcov-tool
# merge`, which folds profile counters across directories (GCC docs:
# https://gcc.gnu.org/onlinedocs/gcc/Invoking-Gcov-Tool.html).
#
# Per-binary .gcda files carry different compile stamps and different names,
# so before merging each binary's profiles are copied into their own
# directory and renamed to the plain source name (<src>.gcda); the merged
# directory is then overlaid with one binary's .gcno for line mapping.
#
# Output: human-readable table on stdout and written next to the CSV, plus a
# machine-readable CSV. Both default to research/evidence/PHASE0-COVERAGE.*
# in the repo root (override the .txt path as the first argument).
#
# Limitations stated honestly:
# - These are host-side unit-test numbers: simulation evidence, not hardware
#   proof (HANDOFF-PASCAL.md §7).
# - If gcov-tool is unavailable, the script falls back to per-binary gcov and
#   reports the MAX coverage seen for each source, which is a lower bound of
#   the true merged union and is labeled as such in the report header.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PASCAL_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
ROOT_DIR=$(cd "$PASCAL_DIR/.." && pwd)
OUT_TXT=${1:-$ROOT_DIR/research/evidence/PHASE0-COVERAGE.txt}
OUT_CSV=${OUT_TXT%.*}.csv

fail() { echo "coverage.sh: error: $*" >&2; exit 1; }

command -v gcov >/dev/null 2>&1 || fail "gcov not found in PATH (install gcc)"
command -v make >/dev/null 2>&1 || fail "make not found in PATH"
HAVE_GCOV_TOOL=0
if command -v gcov-tool >/dev/null 2>&1; then HAVE_GCOV_TOOL=1; fi

GCOV_VER=$(gcov --version | head -1)

WORK_ROOT=$(mktemp -d)
trap 'rm -rf "$WORK_ROOT"' EXIT

# Sibling agents edit core/src concurrently in this repo. A file changed
# between two binaries' compiles gets different gcov stamps per binary, and
# `gcov-tool merge` then silently DROPS the mismatched counters — the source
# reports a bogus 0.00% even though individual binaries covered it. Such an
# attempt is discarded and the whole build+run+merge is retried. A source
# that NO binary covers (brand-new module, no test yet) legitimately reads
# 0.00% and is accepted. After MAX_ATTEMPTS the last attempt's numbers are
# reported with an anomaly warning in the header.
MAX_ATTEMPTS=3

# --- one build+run+merge attempt; sets BINARIES/GCDAS/EXCLUDED_*/ROWS ------
attempt() { # $1 = attempt number
	local n=$1 why t f s bname cand ok out
	WORK_DIR=$WORK_ROOT/a$n
	# Private build directory: siblings run `make` on the shared build/
	# concurrently (rm -rf build + rebuild) and that STOMPS in-flight
	# instrumented profiles — the direct cause of rotating bogus 0.00%
	# rows. Building somewhere only this script knows makes the attempt
	# immune (Makefile clean/coverage-build/all all honor BUILD_DIR).
	MAKE_BUILD=$WORK_DIR/make-build
	mkdir -p "$WORK_DIR"

	# --- 1. build instrumented, run every test binary ----------------------
	# Each test binary is independent, so a compile failure in one sibling
	# test file must not discard the evidence from the others: build with -k
	# and record the excluded binaries in the report header. If NOTHING
	# builds, the later "no test binaries" check aborts the attempt.
	make -C "$PASCAL_DIR" BUILD_DIR="$MAKE_BUILD" clean
	BUILD_LOG="$WORK_DIR/build.log"
	if ! make -k -C "$PASCAL_DIR" BUILD_DIR="$MAKE_BUILD" coverage-build >"$BUILD_LOG" 2>&1; then
		echo "coverage.sh: note: some test binaries failed to build (excluded below)" >&2
	fi

	BINARIES=$(find "$MAKE_BUILD" -maxdepth 1 -type f -perm -u+x -name 'test_*' | sort) ||
		fail "listing test binaries failed"
	[ -n "$BINARIES" ] || return 1

	# Which test sources did NOT produce a binary, and why (first error line).
	EXCLUDED_NAMES=""
	EXCLUDED_WHY=""
	for c in "$PASCAL_DIR"/tests/test_*.c; do
		t=$(basename "$c" .c)
		if [ ! -x "$MAKE_BUILD/$t" ]; then
			why=$(grep -m1 "tests/$t\.c.*error:" "$BUILD_LOG" |
				      sed "s#.*tests/$t\.c:##" || true)
			[ -n "$why" ] || why="no binary produced"
			EXCLUDED_NAMES="$EXCLUDED_NAMES $t"
			EXCLUDED_WHY="$EXCLUDED_WHY
    $t: $why"
		fi
	done

	echo "== running test binaries (attempt $n/$MAX_ATTEMPTS) =="
	for bin in $BINARIES; do
		echo "-- $(basename "$bin")"
		"$bin" >/dev/null
	done

	GCDAS=$(find "$MAKE_BUILD" -maxdepth 1 -name '*.gcda' | sort)
	[ -n "$GCDAS" ] || return 1

	# --- 2. collect per-binary profiles, renamed to plain source names -----
	# <bdir>/<bin>-<src>.gcda -> $WORK_DIR/<bin>/<src>.gcda  (same for .gcno)
	FIRST_BIN=""
	for bin in $BINARIES; do
		bname=$(basename "$bin")
		mkdir -p "$WORK_DIR/$bname"
		for f in "$MAKE_BUILD"/$bname-*.gcda "$MAKE_BUILD"/$bname-*.gcno; do
			[ -e "$f" ] || continue
			s=${f##*$bname-}
			cp "$f" "$WORK_DIR/$bname/$s"
		done
		[ -z "$FIRST_BIN" ] && FIRST_BIN=$bname
	done

	# --- 3. merge ----------------------------------------------------------
	if [ "$HAVE_GCOV_TOOL" -eq 1 ]; then
		cp -r "$WORK_DIR/$FIRST_BIN" "$WORK_DIR/fold"
		for bin in $BINARIES; do
			bname=$(basename "$bin")
			[ "$bname" = "$FIRST_BIN" ] && continue
			gcov-tool merge "$WORK_DIR/fold" "$WORK_DIR/$bname" -o "$WORK_DIR/fold2"
			rm -rf "$WORK_DIR/fold"
			mv "$WORK_DIR/fold2" "$WORK_DIR/fold"
		done
	# gcov-tool output carries only .gcda; overlay .gcno for line mapping.
	# The merged counters keep the first input's stamp, so the first
	# binary's notes match; if a stamp drifts, step 4 retries with the
	# other binaries' notes.
	cp "$WORK_DIR/$FIRST_BIN"/*.gcno "$WORK_DIR/fold/"
	MERGE_MODE="gcov-tool merge (counters summed across all binaries)"
else
	MERGE_MODE="UNMERGED FALLBACK: gcov-tool not available; per-source value is the MAX over binaries (lower bound of the true union)"
fi

# gcov resolves the recorded relative source paths (core/src/*.c) against its
# working directory, so run it from a directory that links back into the tree.
ln -s "$PASCAL_DIR/core" "$WORK_DIR/core"
ln -s "$PASCAL_DIR/tests" "$WORK_DIR/tests"

# --- 4. per-source report rows --------------------------------------------
ROWS=$(for c in "$PASCAL_DIR"/core/src/*.c; do
	report_row "core/src/$(basename "$c")"
	done) || return 1

# A 0.00% row for a source that at least one individual binary DID cover
# means gcov-tool dropped that source's counters (its stamp changed
# mid-build: a sibling edited the file while make was compiling) — discard
# the attempt and rebuild. A source covered by NO binary at all (a
# brand-new module without a test yet) legitimately reports 0.00% and is
# kept, so a sibling can land new core code without breaking this gate.
zeroed=$(printf '%s\n' "$ROWS" | awk '$4 == "0.00" {print $1}')
for z in $zeroed; do
	base=$(basename "$z")
	base=${base%.c}
	for bin in $BINARIES; do
		bname=$(basename "$bin")
		# Run gcov from $WORK_DIR (where the core/ symlink lives) with
		# the profile dir as -o; running from inside the per-binary dir
		# would fail to resolve core/src/... and read as 0% everywhere.
		out=$(cd "$WORK_DIR" && gcov -o "$bname" "$bname/$base.gcda" 2>&1) || continue
		if [ "$(parse_pct "$out" "$z")" != "0.00" ]; then
			echo "coverage.sh: attempt $n discarded: $z merged to 0.00% but" \
			     "$bname covers it (core source edited mid-build;" \
			     "gcov-tool dropped the counters)" >&2
			return 1
		fi
	done
done
}

# --- report helpers --------------------------------------------------------
# Extract "Lines executed:P% of N" for one source from gcov stdout.
# gcov output pairs are:  File '<src>'\n Lines executed:P% of N
# $2 is "executed:P%" — split on ':' to get the bare percentage.
parse_pct()  { printf '%s\n' "$1" | grep -A1 -F "File '$2'" | awk '$1=="Lines" {split($2,a,":"); sub("%", "", a[2]); print a[2]; found=1; exit} END {if (!found) exit 1}'; }
parse_lines() { printf '%s\n' "$1" | grep -A1 -F "File '$2'" | awk '$1=="Lines" {print $NF; found=1} END {if (!found) exit 1}'; }

report_row() { # $1=source path relative to pascal/ (core/src/foo.c)
	local src=$1 out pct lines covered
	# step 2 renamed the profiles to plain file names: foo.c -> foo.gcda
	local base=${src##*/}
	base=${base%.c}
	if [ "$HAVE_GCOV_TOOL" -eq 1 ]; then
		local ok=0 cand
		for cand in $(for bin in $BINARIES; do basename "$bin"; done); do
			# retry across binaries' notes files in case the merged
			# counters' stamp drifted from the first binary's
			cp "$WORK_DIR/$cand/$base.gcno" "$WORK_DIR/fold/" 2>/dev/null || true
			if out=$(cd "$WORK_DIR" && gcov -o fold "fold/$base.gcda" 2>&1); then
				ok=1
				break
			fi
		done
		[ "$ok" -eq 1 ] || fail "gcov failed for $src: ${out:-no output}"
	else
		# unmerged fallback: report the max over binaries (lower bound)
		local bestpct="" bestlines="" bname p n
		for bin in $BINARIES; do
			bname=$(basename "$bin")
			if out=$(cd "$WORK_DIR/$bname" && gcov -o . "$base.gcda" 2>&1); then
				p=$(parse_pct "$out" "$src") || continue
				n=$(parse_lines "$out" "$src") || continue
				if [ -z "$bestpct" ] || awk -v a="$p" -v b="$bestpct" 'BEGIN { exit !(a > b) }'; then
					bestpct=$p
					bestlines=$n
				fi
			fi
		done
		out="File '$src'
Lines executed:${bestpct:-0.00}% of ${bestlines:-0}"
	fi
	pct=$(parse_pct "$out" "$src") || fail "no gcov percentage reported for $src"
	lines=$(parse_lines "$out" "$src") || fail "no gcov line count reported for $src"
	covered=$(awk -v p="$pct" -v n="$lines" 'BEGIN { printf "%.0f", p * n / 100 }')
	printf '%s %s %s %s\n' "$src" "$lines" "$covered" "$pct"
}

# --- 5. driver: attempt until clean or out of retries ----------------------
at=1
while :; do
	if attempt "$at"; then
		break
	fi
	if [ "$at" -ge "$MAX_ATTEMPTS" ]; then
		echo "coverage.sh: warning: reporting last attempt despite anomalies" >&2
		ANOMALY_WARNING="yes"
		break
	fi
	# Siblings are actively editing sources; give an in-flight edit time to
	# reach a compiling state before the next full rebuild.
	echo "coverage.sh: waiting 30s before retry (sibling edits in flight?)" >&2
	sleep 30
	at=$((at + 1))
done

[ -n "${ROWS:-}" ] || fail "no coverage data after $MAX_ATTEMPTS attempts — core sources do not compile right now (sibling edits in flight?); existing evidence file left untouched"

# --- 6. emit evidence ------------------------------------------------------
TOTAL=$(printf '%s\n' "$ROWS" | awk '{l+=$2; c+=$3} END{printf "%d %d %.2f", l, c, (l>0)?100*c/l:0}')
NBIN=$(printf '%s\n' "$BINARIES" | wc -l)
NGCDA=$(printf '%s\n' "$GCDAS" | wc -l)
NOW=$(date -u '+%Y-%m-%d %H:%M:%SZ')

{
	echo "PHASE 0 COVERAGE — merged gcov report over all test binaries"
	echo "============================================================="
	echo "Evidence class: host-side unit tests (simulation, not hardware proof)"
	echo "Generated:      $NOW by pascal/scripts/coverage.sh"
	echo "Toolchain:      $GCOV_VER"
	echo "Binaries:       $NBIN test binaries, $NGCDA .gcda files"
	echo "Build dir:      private per-attempt dir (make BUILD_DIR override) so"
	echo "                concurrent sibling make runs cannot stomp profiles"
	echo "Merge mode:     $MERGE_MODE (attempt $at of up to $MAX_ATTEMPTS;"
	echo "                attempts are retried when a concurrent edit of core"
	echo "                sources mid-build corrupts the counter merge — see the"
	echo "                0.00% guard in the script)"
	if [ -n "${ANOMALY_WARNING:-}" ]; then
		echo "WARNING:        last attempt still had anomalous rows; numbers"
		echo "                below may undercount the affected source(s)."
	fi
	echo "Note: plain gcov does NOT accumulate multiple .gcda for one source"
	echo "(verified empirically on this toolchain — a multi-.gcda invocation"
	echo "exits 0 reporting the single-binary count); merging is gcov-tool's"
	echo "documented job. Merged profiles report Runs:N with N = binaries run."
	if [ -n "$EXCLUDED_NAMES" ]; then
		echo "Excluded:      test binaries that failed to COMPILE (sibling test"
		echo "               sources, not core code — their coverage is missing"
		echo "               from this table and the numbers remain valid for the"
		echo "               included binaries):$EXCLUDED_WHY"
	fi
	echo
	echo "file                       lines  covered     %"
	echo "-----------------------------------------------------"
	printf '%s\n' "$ROWS" | awk '{printf "%-25s %6s %8s %6s%%\n", $1, $2, $3, $4}'
	read -r tl tc tp <<<"$TOTAL"
	echo "-----------------------------------------------------"
	printf '%-25s %6d %8d %6s%%\n' "TOTAL" "$tl" "$tc" "$tp"
} | tee "$OUT_TXT"

{
	echo "file,lines,covered,percent"
	printf '%s\n' "$ROWS" | awk -F' ' '{printf "%s,%s,%s,%s\n", $1, $2, $3, $4}'
	read -r tl tc tp <<<"$TOTAL"
	printf 'TOTAL,%d,%d,%s\n' "$tl" "$tc" "$tp"
} > "$OUT_CSV"

# No shared-tree cleanup at the end: the instrumented build lived in a
# private directory (removed by the WORK_ROOT trap) and the shared build/
# may belong to a sibling's in-flight `make` — do not touch it.

echo
echo "wrote $OUT_TXT"
echo "wrote $OUT_CSV"
