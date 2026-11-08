#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# pascal/kext/scripts/symbol-audit.sh — Phase 3 gate: "kext source passes a
# header/symbol audit — no unresolved references to macOS-only symbols from
# the portable core" (HANDOFF-PASCAL.md §6 Phase 3).
#
# What it checks, over the portable surface (pascal/core/** plus the pure-C
# files of this tree that the IOKit glue calls into):
#   1. No macOS-only symbols outside comments: IOService, IOMemoryMap,
#      IOMemoryDescriptor, IOWorkLoop, IOCommandGate, IOLock, IOMalloc/IOFree,
#      kern_os_malloc/free, kIOReturn*, kIOUCVariableStructureSize,
#      IOExternalMethod*, IOKit/OSObject/libkern types.
#   2. A generic "IO-prefixed identifier" net (any word like IOKit/IOWorkLoop
#      standing alone), also outside comments — catches symbols this list
#      forgot.
#   3. Includes restricted to the portable set: stdbool, stddef, stdint,
#      string, stdlib, errno, and the tree's own <pascal/*.h> headers. Any
#      other system include in the portable core is a failure to report.
#
# The kext glue (src/PG106*.cpp/.hpp, src/pg106_klibc.c, src/klibc/*.h) may
# use macOS symbols, but is audited separately (section "glue" below):
#   4. No non-KPI allocator entry points (kern_os_malloc/free/realloc, kalloc,
#      kfree, zalloc: not declared by any public SDK header) and no
#      IOMallocZero (availability below the 10.13 target is not established).
#   5. No macro that renames a libc allocation function (#define free ...):
#      a forced-include version of that renamed OSObject::free() in C++.
#   6. Every angle-bracket include of the glue is either a portable header,
#      a tree header, or present in MacKernelSDK (when the SDK is found:
#      $PG106_MACKERNELSDK, <repo>/MacKernelSDK, or $BUILD_DIR/MacKernelSDK).
#
#   7. The GP106 id list in the Info.plist comment equals the accepted rows of
#      src/pg106_device_gate.c (the plist match itself is a wide net).
#
# C++/IOKit API misuse (wrong overrides, signatures) is not something a grep
# can find; scripts/kext-syntax-check.sh compiles the glue against the SDK.
#
# Exit 0 = clean; exit 1 with a list of offending files/lines.

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
KEXT_ROOT="$(dirname "$HERE")"
CORE_ROOT="$KEXT_ROOT/../core"

# Portable surface: the core library plus the plain-C files of the kext tree
# that must build unchanged on both sides (Linux stub + kext). An array, not
# a space-joined string: `VAR="a" \n<cont> "b"` parses as an assignment
# followed by a COMMAND in bash (continuations join lines, not assignment
# words), which executed the glob-expanded header paths.
PORTABLE_SOURCES=(
	"$CORE_ROOT"/src/*.c
	"$CORE_ROOT"/include/pascal/*.h
	"$KEXT_ROOT"/src/pg106_device_gate.c
	"$KEXT_ROOT"/src/pg106_device_gate.h
	"$KEXT_ROOT"/src/pg106_uc_abi.h
)

# macOS-only identifiers. Explicit list first (the ones the audit exists for).
MACOS_SYMBOLS='\b(IOService|IOUserClient|IOPCIDevice|IOMemoryMap|IOMemoryDescriptor|IOBufferMemoryDescriptor|IOMemoryCursor|IOWorkLoop|IOCommandGate|IOLock|IORecursiveLock|IOMalloc|IOFree|kern_os_malloc|kern_os_free|kIOReturn[A-Za-z]*|kIOUCVariableStructureSize|IOExternalMethod[A-Za-z]*|IOOptionBits|OSObject|OSDictionary|OSArray|OSString|OSNumber|OSDefineMetaClassAndStructors|OSDeclareDefaultStructors|OSDynamicCast|OSTypeAlloc|libkern|IOLog|IOKit)\b'
# Generic net: any standalone IO-prefixed CamelCase word (IOKit, IOWorkLoop,
# ...) — preceded by a non-identifier character so PG106_MMIO_* never matches.
IO_PREFIX_NET='(^|[^A-Za-z0-9_])IO[A-Z][A-Za-z0-9_]*'
# Angle-bracket includes allowed in the portable core. Quoted includes are
# tree-local and always fine.
ALLOWED_INCLUDES='^(stdbool\.h|stddef\.h|stdint\.h|string\.h|stdlib\.h|errno\.h|pascal/[a-z]+\.h)$'

# Strip /* */ block comments and // line comments. (Perl, because the audit
# must behave identically on Linux and macOS.)
strip_comments() {
	perl -0777 -pe 's{/\*.*?\*/}{ }gs; s{//[^\n]*}{}g' "$1"
}

fail=0
seen_file=0
for f in "${PORTABLE_SOURCES[@]}"; do
	[ -f "$f" ] || continue
	seen_file=1
	stripped="$(strip_comments "$f")"

	hits="$(printf '%s\n' "$stripped" | grep -nE "$MACOS_SYMBOLS")"
	if [ -n "$hits" ]; then
		echo "FAIL $f: macOS-only symbol(s) outside comments:"
		echo "$hits"
		fail=1
	fi

	hits="$(printf '%s\n' "$stripped" | grep -nE "$IO_PREFIX_NET")"
	if [ -n "$hits" ]; then
		echo "FAIL $f: standalone IO-prefixed identifier(s) outside comments:"
		echo "$hits"
		fail=1
	fi

	# Audit angle-bracket includes only (they name external headers).
	inc="$(printf '%s\n' "$stripped" |
		sed -nE 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*<([^>]+)>.*/\1/p')"
	for h in $inc; do
		case "$h" in
		pascal/*) ok=1 ;;
		*) ok=0; printf '%s\n' "$h" | grep -qE "$ALLOWED_INCLUDES" && ok=1 ;;
		esac
		if [ "$ok" != 1 ]; then
			echo "FAIL $f: non-portable include <$h>"
			fail=1
		fi
	done
done

# ---- glue ------------------------------------------------------------------

GLUE_SOURCES=(
	"$KEXT_ROOT"/src/PG106*.cpp
	"$KEXT_ROOT"/src/PG106*.hpp
	"$KEXT_ROOT"/src/pg106_klibc.c
	"$KEXT_ROOT"/src/pg106_klibc.h
	"$KEXT_ROOT"/src/klibc/*.h
)
NON_KPI_ALLOC='\b(kern_os_malloc|kern_os_free|kern_os_realloc|kalloc|kfree|zalloc|IOMallocZero)\b'
ALLOC_RENAME='^[[:space:]]*#[[:space:]]*define[[:space:]]+(malloc|calloc|realloc|free)\b'
GLUE_PORTABLE_INCLUDES='^(stdbool\.h|stddef\.h|stdint\.h|string\.h|stdlib\.h|errno\.h)$'

REPO_ROOT="$(cd "$KEXT_ROOT/../.." && pwd)"
SDK_HEADERS=""
for d in "${PG106_MACKERNELSDK:-}" "$REPO_ROOT/MacKernelSDK" "${BUILD_DIR:-$KEXT_ROOT/build}/MacKernelSDK"; do
	if [ -n "$d" ] && [ -d "$d/Headers/IOKit" ]; then
		SDK_HEADERS="$d/Headers"
		break
	fi
done

seen_glue=0
for f in "${GLUE_SOURCES[@]}"; do
	[ -f "$f" ] || continue
	seen_glue=1
	stripped="$(strip_comments "$f")"

	hits="$(printf '%s\n' "$stripped" | grep -nE "$NON_KPI_ALLOC")"
	if [ -n "$hits" ]; then
		echo "FAIL $f: non-KPI / unavailable allocator call(s):"
		echo "$hits"
		fail=1
	fi
	hits="$(printf '%s\n' "$stripped" | grep -nE "$ALLOC_RENAME")"
	if [ -n "$hits" ]; then
		echo "FAIL $f: macro renames a libc allocation function:"
		echo "$hits"
		fail=1
	fi

	inc="$(printf '%s\n' "$stripped" |
		sed -nE 's/^[[:space:]]*#[[:space:]]*include(_next)?[[:space:]]*<([^>]+)>.*/\2/p')"
	for h in $inc; do
		case "$h" in
		pascal/*) continue ;;
		esac
		printf '%s\n' "$h" | grep -qE "$GLUE_PORTABLE_INCLUDES" && continue
		if [ -z "$SDK_HEADERS" ]; then
			case "$h" in
			IOKit/*|libkern/*|sys/*|kern/*|pexpert/*|mach/*) continue ;; # unverified without the SDK
			esac
			echo "FAIL $f: include <$h> is not an IOKit/libkern/sys/kern/pexpert/mach header"
			fail=1
		elif [ ! -f "$SDK_HEADERS/$h" ]; then
			echo "FAIL $f: include <$h> does not exist in MacKernelSDK ($SDK_HEADERS)"
			fail=1
		fi
	done
done
if [ "$seen_glue" != 1 ]; then
	echo "FAIL: no kext glue sources found under $KEXT_ROOT/src"
	fail=1
fi
[ -z "$SDK_HEADERS" ] &&
	echo "symbol-audit: note — MacKernelSDK not found; glue includes checked by prefix only"

# ---- Info.plist vs gate table ---------------------------------------------

gate_ids="$(grep -E 'PG106_GATE_OK_GP106 \}' "$KEXT_ROOT/src/pg106_device_gate.c" |
	grep -oE '0x[0-9A-Fa-f]{4}' | tr 'a-f' 'A-F' | sort | tr '\n' ' ')"
plist_ids="$(sed -n '/<!-- GPL-2.0-only tree/,/-->/p' "$KEXT_ROOT/Info.plist" |
	grep -oE '0x1C[0-9A-Fa-f]{2}' | tr 'a-f' 'A-F' | sort -u | tr '\n' ' ')"
if [ -z "$gate_ids" ] || [ "$gate_ids" != "$plist_ids" ]; then
	echo "FAIL Info.plist comment ids [$plist_ids] != gate table GP106 ids [$gate_ids]"
	fail=1
fi

if [ "$seen_file" != 1 ]; then
	echo "FAIL: no portable sources found under $CORE_ROOT — audit ran against nothing"
	exit 1
fi

if [ "$fail" != 0 ]; then
	echo "symbol-audit: FAILED"
	exit 1
fi
echo "symbol-audit: OK — portable core clean; kext glue uses KPI allocators and SDK headers only"
