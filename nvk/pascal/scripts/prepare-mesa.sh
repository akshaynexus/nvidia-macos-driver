#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Prepare a Mesa source tree for the Pascal NVK build:
#   Mesa 17ca6174 (nvk/MESA_BASE)  +  nvk/nvk-macos.patch  +  nvk/pascal/nvk-pascal.patch
#   + nvk/pascal/nvk-build-fixes.patch  + three NVRM class headers (fetched, pinned)
# link the pg106 bridge (pascal/bridge, pascal/core) into
# src/nouveau/vulkan/nvkmd/pg106/{bridge,core}, and prefetch the Rust crates.
#
# Usage: prepare-mesa.sh <workdir> [--macos-only]
#   <workdir>/mesa-17ca6174.tar.gz is downloaded if missing and checked
#   against its SHA-256; the tree is (re)created at <workdir>/mesa.
#   --macos-only stops after nvk-macos.patch (the base mkpatch.py diffs
#   against) and creates <workdir>/mesa-macos instead.
#
# Works on Linux and macOS (POSIX sh, curl or the existing tarball, patch).
set -eu

[ $# -ge 1 ] || { echo "usage: $0 <workdir> [--macos-only]" >&2; exit 2; }
WORK=$1
MODE=${2:-}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)

BASE=$(cat "$REPO/nvk/MESA_BASE")
FULL=17ca6174dcc6cb22059ac343bc29f8af7800f42e
[ "$BASE" = "$(printf %s "$FULL" | cut -c1-8)" ] || {
    echo "nvk/MESA_BASE is $BASE; this script pins $FULL" >&2; exit 2; }
# SHA-256 of GitLab's archive of that commit, as downloaded 2026-11-07.
SHA=602ae1e398fd07c8ac5e0389051836f7cccff77de82c43ec97acdfdfeab3fc0d
TAR=$WORK/mesa-$BASE.tar.gz
URL=https://gitlab.freedesktop.org/mesa/mesa/-/archive/$FULL/mesa-$FULL.tar.gz

mkdir -p "$WORK"
if [ ! -f "$TAR" ]; then
    echo "downloading $URL"
    curl -fsSL -o "$TAR.part" "$URL"
    mv "$TAR.part" "$TAR"
fi
if command -v sha256sum >/dev/null 2>&1; then
    GOT=$(sha256sum "$TAR" | cut -d' ' -f1)
else
    GOT=$(shasum -a 256 "$TAR" | cut -d' ' -f1)
fi
[ "$GOT" = "$SHA" ] || { echo "checksum mismatch for $TAR: $GOT" >&2; exit 3; }

if [ "$MODE" = "--macos-only" ]; then DEST=$WORK/mesa-macos; else DEST=$WORK/mesa; fi
rm -rf "$DEST" "$WORK/extract"
mkdir -p "$WORK/extract"
tar xzf "$TAR" -C "$WORK/extract"
mv "$WORK/extract/mesa-$FULL" "$DEST"
rmdir "$WORK/extract"

( cd "$DEST" && patch -p1 -s --no-backup-if-mismatch < "$REPO/nvk/nvk-macos.patch" )
echo "applied nvk/nvk-macos.patch"
[ "$MODE" = "--macos-only" ] && { echo "tree: $DEST"; exit 0; }

( cd "$DEST" && patch -p1 -s --no-backup-if-mismatch < "$REPO/nvk/pascal/nvk-pascal.patch" )
echo "applied nvk/pascal/nvk-pascal.patch"
( cd "$DEST" && patch -p1 -s --no-backup-if-mismatch < "$REPO/nvk/pascal/nvk-build-fixes.patch" )
echo "applied nvk/pascal/nvk-build-fixes.patch"

# The NVRM backend includes three Kepler class headers that
# open-gpu-kernel-modules 615.78.08 no longer has (nvkmd_nvrm_pdev.c uses
# their class ids). Fetch NVIDIA's 570.86.16 copies (MIT), pinned by SHA-256,
# into nvkmd/nvrm/class/ next to the cla26f.h nvk-macos.patch carries.
OGKM570=https://raw.githubusercontent.com/NVIDIA/open-gpu-kernel-modules/570.86.16/src/common/sdk/nvidia/inc/class
CLS=$DEST/src/nouveau/vulkan/nvkmd/nvrm/class
mkdir -p "$WORK/ogkm-570.86.16"
for e in cla0c0:079c8f47ade5b52df48c55f3da786c104a5877907d9ca7434f6ff40a0550d735 \
         cla197:a0efb2656a3810ae3e26918e763b2cb0df62c5ad9f7cd4f693ba8170fb33b7ad \
         cla1c0:c545aa9975ac88aaa0c60a518a3bc71f2b166e87bcfc6d007f2b97fee205e7be; do
    h=${e%%:*}; want=${e#*:}; f=$WORK/ogkm-570.86.16/$h.h
    [ -f "$f" ] || { curl -fsSL -o "$f.part" "$OGKM570/$h.h" && mv "$f.part" "$f"; }
    if command -v sha256sum >/dev/null 2>&1; then got=$(sha256sum "$f" | cut -d' ' -f1)
    else got=$(shasum -a 256 "$f" | cut -d' ' -f1); fi
    [ "$got" = "$want" ] || { echo "checksum mismatch for $f: $got" >&2; exit 3; }
    cp "$f" "$CLS/$h.h"
done
echo "added class/cla0c0.h cla197.h cla1c0.h (open-gpu-kernel-modules 570.86.16)"

# Prefetch the Rust crates NAK/NVK's meson wraps name into the tree's
# packagecache (cached in <workdir>/crates across tree rebuilds). meson
# verifies each against the source_hash in its .wrap, so a bad download
# fails the configure. meson's own downloader stalled ~2 min per crate on
# the reference box; curl does not.
mkdir -p "$WORK/crates" "$DEST/subprojects/packagecache"
for w in "$DEST"/subprojects/*-rs.wrap; do
    u=$(sed -n 's/^source_url *= *//p' "$w"); f=$(sed -n 's/^source_filename *= *//p' "$w")
    [ -n "$u" ] && [ -n "$f" ] || continue
    [ -f "$WORK/crates/$f" ] || curl -fsSL --max-time 120 -o "$WORK/crates/$f" "$u" ||
        { rm -f "$WORK/crates/$f"; echo "note: could not prefetch $f (meson will try)"; continue; }
    cp "$WORK/crates/$f" "$DEST/subprojects/packagecache/$f"
done

PG=$DEST/src/nouveau/vulkan/nvkmd/pg106
ln -s "$REPO/pascal/bridge" "$PG/bridge"
ln -s "$REPO/pascal/core" "$PG/core"
echo "linked pascal/bridge and pascal/core into $PG"

# The NVRM (Turing+) backend in the same library needs NVIDIA's
# open-gpu-kernel-modules headers at nvkmd/nvrm/open-gpu-kernel-modules
# (nvk-macos.patch meson.build). Link the repo's submodule if it is checked
# out; otherwise put a 615.78.08 checkout there before meson setup.
OGKM=$REPO/third_party/open-gpu-kernel-modules
if [ -d "$OGKM/src/common/sdk/nvidia/inc" ]; then
    ln -s "$OGKM" "$DEST/src/nouveau/vulkan/nvkmd/nvrm/open-gpu-kernel-modules"
    echo "linked third_party/open-gpu-kernel-modules for the NVRM backend"
else
    echo "note: no open-gpu-kernel-modules checkout; the full NVK build needs one at"
    echo "      $DEST/src/nouveau/vulkan/nvkmd/nvrm/open-gpu-kernel-modules"
fi
echo "tree: $DEST"
