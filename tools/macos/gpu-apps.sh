#!/bin/bash
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# gpu-apps.sh: choose which apps render on the GTX 1060 (GP106) on the Helios 300, the prime-run equivalent.
# The Metal plugin (plugin/NVMTLDevice.m nvmtl_process_is_allowed) gives the GP106 only to processes named in
# /Library/GPUBundles/nvmtl-allow.txt; every other process stays on the Intel HD 630.
#
#   gpu-apps.sh list                      the rules in the list
#   sudo gpu-apps.sh add APP...           allow (APP = /Applications/Foo.app, an executable path, or a process name)
#   sudo gpu-apps.sh remove APP...        take the allow rule away again (deny rules are kept)
#   sudo gpu-apps.sh deny APP...          never give it the GP106
#   gpu-apps.sh check APP                 what the plugin would decide for it
#   gpu-apps.sh run APP [ARGS...]         launch once on the GP106 without a list entry (NVMTL_ENABLE=1)
#   gpu-apps.sh run --log APP [ARGS...]   the same, with the plugin's full log in /tmp/nvmtl-<name>.log
#                                         (NVMTL_PROCESS_ENV=1 NVMTL_LOG_PATH=...; every machine: sudo touch
#                                         /Library/GPUBundles/nvmtl-log logs all processes, see tools/macos/README.md)
# The plugin reads the list when a process creates its first Metal device: changes apply to apps started afterwards.
set -euo pipefail
LIST=${NVMTL_ALLOW:-/Library/GPUBundles/nvmtl-allow.txt}
die() { printf 'gpu-apps: %s\n' "$*" >&2; exit 1; }

name_of() {   # app bundle / executable path / name -> the process name getprogname() returns
	local a=${1%/}
	if [ -d "$a" ] && [ -f "$a/Contents/Info.plist" ]; then
		local exe; exe=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$a/Contents/Info.plist" 2>/dev/null) \
			|| die "$a has no CFBundleExecutable"
		printf '%s\n' "$exe"
	elif [ -f "$a" ] && [ -x "$a" ]; then basename "$a"
	else printf '%s\n' "$a"; fi
}
valid() {
	case "$1" in ""|"*"|-*|\#*|*[[:space:]]*|*/*) die "'$1' is not a usable process name";; esac
	[ ${#1} -lt 200 ] || die "'$1' is too long"
}
need_root() { [ "$(id -u)" = 0 ] || die "run with sudo (the list is root-owned, 0644)"; }
write_list() {   # stdin -> the list, atomically, root:wheel 0644
	local tmp; tmp=$(mktemp "$(dirname "$LIST")/.nvmtl-allow.XXXXXX")
	cat > "$tmp"; chown root:wheel "$tmp"; chmod 0644 "$tmp"; mv -f "$tmp" "$LIST"
}
rules() { [ -f "$LIST" ] && grep -vE '^[[:space:]]*(#|$)' "$LIST" | sed 's/^[[:space:]]*//' || true; }
decide() {   # the plugin's walk: first matching rule wins
	local me=$1 r
	while IFS= read -r r; do
		case "$r" in
			-*) r=${r#-}; r=${r#"${r%%[![:space:]]*}"}; [ "$r" = "$me" ] && { echo "denied (rule -$me)"; return; };;
			*) [ "$r" = "*" ] || [ "$r" = "$me" ] && { echo "allowed (rule $r)"; return; };;
		esac
	done < <(rules)
	echo "not allowed (no rule matches: Intel GPU)"
}

cmd=${1:-list}; shift || true
# run: the app gets the caller's stdin, as prime-run passes it
if [ "$cmd" = run ]; then
	log=0; [ "${1:-}" = --log ] && { log=1; shift; }
	[ $# -gt 0 ] || die "run: which app?"
	a=${1%/}; shift
	if [ -d "$a" ] && [ -f "$a/Contents/Info.plist" ]; then a="$a/Contents/MacOS/$(name_of "$a" </dev/null)"; fi
	[ -x "$a" ] || a=$(command -v "$a") || die "run: $a is not an executable"
	if [ "$log" = 1 ]; then
		lp="/tmp/nvmtl-$(basename "$a").log"
		echo "gpu-apps: plugin log -> $lp" >&2
		NVMTL_ENABLE=1 NVMTL_PROCESS_ENV=1 NVMTL_LOG_PATH="$lp" exec "$a" "$@"
	fi
	NVMTL_ENABLE=1 exec "$a" "$@"
fi
# Every other command reads /dev/null: run over SSH as `{ echo ...; cat gpu-apps.sh; } | hbmac`, the rest of the
# script is on stdin (write_list reads its own pipe, not this stdin).
case "$cmd" in
list)
	[ -f "$LIST" ] || die "no $LIST (install-gpu.sh installs the default one)"
	echo "$LIST:"; rules | sed 's/^/  /' ;;
add|deny)
	need_root; [ $# -gt 0 ] || die "$cmd: which app?"; [ -f "$LIST" ] || die "no $LIST"
	for a in "$@"; do
		n=$(name_of "$a"); valid "$n"
		want=$n; [ "$cmd" = deny ] && want="-$n"
		# drop every earlier rule for this name, then put the new one in place: deny rules go to the top (before a
		# possible "*"), allow rules at the end
		body=$(grep -vxE "[[:space:]]*-?[[:space:]]*$(printf '%s' "$n" | sed 's/[.[\*^$()+?{}|]/\\&/g')[[:space:]]*" "$LIST" || true)
		if [ "$cmd" = deny ]; then
			{ printf '%s\n' "$body" | awk -v d="$want" 'BEGIN{done=0} !done && $0 !~ /^[[:space:]]*#/ && $0 !~ /^[[:space:]]*$/ {print d; done=1} {print} END{if(!done) print d}'; } | write_list
		else
			{ printf '%s\n' "$body"; printf '%s\n' "$want"; } | write_list
		fi
		echo "$cmd: $n ($(decide "$n"))"
	done ;;
remove)
	need_root; [ $# -gt 0 ] || die "remove: which app?"; [ -f "$LIST" ] || die "no $LIST"
	for a in "$@"; do
		n=$(name_of "$a"); valid "$n"
		grep -vxE "[[:space:]]*$(printf '%s' "$n" | sed 's/[.[\*^$()+?{}|]/\\&/g')[[:space:]]*" "$LIST" | write_list || true
		echo "remove: $n ($(decide "$n"))"
	done ;;
check)
	[ $# = 1 ] || die "check: one app"; n=$(name_of "$1"); echo "$n: $(decide "$n")" ;;
*) sed -n '2,18p' "$0"; exit 2 ;;
esac </dev/null
