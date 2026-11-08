#!/usr/bin/env python3
# NullMoth NVIDIA driver for macOS
# Copyright (c) 2026 NullMoth Systems.
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""air_xlate_check.py - Apple AIR -> (LLVM C API, as the plugin) -> IR text -> translator -> SPIR-V -> spirv-val, on Linux.

    python3 -I plugin/tests/air_xlate_check.py [--metallib F.metallib ...] [--bc F.air.bc ...] [--out DIR]
        [--libllvm /usr/lib/libLLVM.so.23.1] [--xlate libnvmtl_translate.so] [--llvm-version 23.1.4]
        [--modes default,canonical] [--expect-fail NAME=substring ...]

With no --metallib/--bc it checks the plugin's own Apple-compiled library (plugin/nvconv_lib.h, 17 modules,
air64_v27-apple-macosx15.7.0). Modules are found the way check-air-llvm.py (macos_helios300) finds them: every
bitcode wrapper (0x0b17c0de) whose payload is LLVM bitcode.

The translator is built for the host from translator/wrapper (cargo build --release) unless --xlate is given. The
host libLLVM must be the release's version (default 23.1.4, what NVMTLDriver.bundle/Contents/MacOS/air-runtime ships),
because the printed IR syntax is what the translator reads.

Modes: "default" (what every process gets) and "canonical" (NVMTL_CANONICAL_POINTERS, set by app profiles).
Each mode prints one line per module and a summary; exit 0 when every module in every mode translated and validated,
1 otherwise, 77 when a tool is missing (no host LLVM of that version, no cargo, no spirv-val, no C compiler).
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.dirname(HERE)
ROOT = os.path.dirname(PLUGIN)


def modules_of(blob, stem, out_dir):
    paths = []
    for m in re.finditer(re.escape(b"\xde\xc0\x17\x0b"), blob):
        o = m.start()
        if o + 20 > len(blob):
            continue
        _, _, off, size, _ = struct.unpack_from("<5I", blob, o)
        if blob[o + off:o + off + 4] != b"BC\xc0\xde" or o + off + size > len(blob):
            continue
        p = os.path.join(out_dir, f"{stem}-{len(paths):02d}.air.bc")
        with open(p, "wb") as fh:
            fh.write(blob[o:o + off + size])
        paths.append(p)
    return paths


def header_blob(path):
    with open(path, encoding="utf-8") as fh:
        body = fh.read().split("[] = {", 1)[1].split("};", 1)[0]
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body))


def host_libllvm(version):
    try:
        got = subprocess.run(["llvm-config", "--version"], capture_output=True, text=True, check=True).stdout.strip()
        libdir = subprocess.run(["llvm-config", "--libdir"], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None, "llvm-config not found"
    if got != version:
        return None, f"host LLVM is {got}, not {version} (--libllvm)"
    major, minor = version.split(".")[:2]
    for name in (f"libLLVM.so.{major}.{minor}", f"libLLVM-{major}.so", f"libLLVM.so.{major}"):
        if os.path.isfile(os.path.join(libdir, name)):
            return os.path.join(libdir, name), None
    return None, f"no libLLVM.so.{major}.{minor} in {libdir}"


def build_xlate(work):
    cargo = shutil.which("cargo") or os.path.expanduser("~/.cargo/bin/cargo")
    if not os.path.exists(cargo):
        return None, "no cargo"
    target = os.environ.get("AIR_XLATE_TARGET_DIR", os.path.join(work, "target"))
    r = subprocess.run([cargo, "build", "--release", "--locked", "--manifest-path",
                        os.path.join(ROOT, "translator", "wrapper", "Cargo.toml"), "--target-dir", target],
                       capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stderr[-3000:])
        return None, "cargo build of translator/wrapper failed"
    return os.path.join(target, "release", "libnvmtl_translate.so"), None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--metallib", action="extend", nargs="+", default=[])
    ap.add_argument("--bc", action="extend", nargs="+", default=[])
    ap.add_argument("--out")
    ap.add_argument("--libllvm")
    ap.add_argument("--xlate")
    ap.add_argument("--llvm-version", default="23.1.4")
    ap.add_argument("--modes", default="default,canonical")
    ap.add_argument("--expect-fail", action="append", default=[],
                    help="NAME=substring: module NAME (file stem) is a known gap whose error contains substring")
    a = ap.parse_args(argv)

    spirv_val = shutil.which("spirv-val")
    cc = shutil.which("cc") or shutil.which("clang")
    lib, why = (a.libllvm, None) if a.libllvm else host_libllvm(a.llvm_version)
    if not lib or not spirv_val or not cc:
        print(f"air_xlate_check: SKIP {why or ('no spirv-val' if not spirv_val else 'no C compiler')}")
        return 77
    work = a.out or tempfile.mkdtemp(prefix="air-xlate-")
    os.makedirs(work, exist_ok=True)
    xlate = a.xlate
    if not xlate:
        xlate, why = build_xlate(work)
        if not xlate:
            print(f"air_xlate_check: SKIP {why}")
            return 77
    exe = os.path.join(work, "air_xlate_check")
    r = subprocess.run([cc, "-O1", "-Wall", "-Wextra", "-Werror", "-o", exe, os.path.join(HERE, "air_xlate_check.c"), "-ldl"],
                       capture_output=True, text=True)
    if r.returncode:
        print("air_xlate_check: FAIL compiling air_xlate_check.c\n" + r.stderr[-3000:])
        return 1

    src = os.path.join(work, "bc")
    os.makedirs(src, exist_ok=True)
    mods = list(a.bc)
    if not a.metallib and not a.bc:
        mods += modules_of(header_blob(os.path.join(PLUGIN, "nvconv_lib.h")), "nvconv", src)
    for ml in a.metallib:
        with open(ml, "rb") as fh:
            mods += modules_of(fh.read(), os.path.splitext(os.path.basename(ml))[0], src)
    if not mods:
        print("air_xlate_check: FAIL no AIR module found")
        return 1
    expect = dict(e.split("=", 1) for e in a.expect_fail)

    failed = 0
    for mode in a.modes.split(","):
        out = os.path.join(work, mode)
        os.makedirs(out, exist_ok=True)
        r = subprocess.run([exe, lib, xlate, out, "1" if mode == "canonical" else "0", *mods],
                           capture_output=True, text=True)
        if r.returncode not in (0, 1):
            print(f"air_xlate_check: FAIL harness ({mode}): {r.stderr.strip()[-2000:]}")
            return 1
        ok = bad = known = skipped = 0
        for line in r.stdout.splitlines():
            parts = line.split(None, 2)
            if len(parts) < 3:
                continue
            kind, base, rest = parts
            stem = base.replace(".air.bc", "")
            if kind == "ok":
                spv = os.path.join(out, base + ".spv")
                v = subprocess.run([spirv_val, "--target-env", "vulkan1.2", spv], capture_output=True, text=True)
                if v.returncode:
                    msg = (v.stderr or v.stdout).strip().splitlines()
                    print(f"  FAIL {mode:9} {stem}: spirv-val: {msg[0] if msg else '?'}")
                    bad += 1
                else:
                    st, ir, _, sb = rest.split()
                    print(f"  ok   {mode:9} {stem}: {st}, {ir} B IR -> {sb} B SPIR-V, spirv-val vulkan1.2 PASS")
                    ok += 1
            elif kind == "FAIL":
                if "no !air.vertex/fragment/kernel entry" in rest:
                    # a function-only module (visible / stitched functions): the plugin links it, never translates it alone
                    print(f"  skip {mode:9} {stem}: no entry point (function library module)")
                    skipped += 1
                elif stem in expect and expect[stem] in rest:
                    print(f"  gap  {mode:9} {stem}: {rest[:300]}")
                    known += 1
                else:
                    print(f"  FAIL {mode:9} {stem}: {rest[:600]}")
                    bad += 1
        print(f"air_xlate_check: {mode}: {ok} translated + validated, {known} known gaps, {skipped} without an entry, "
              f"{bad} failed, of {len(mods)}")
        failed += bad
    print(f"air_xlate_check: {'FAIL' if failed else 'PASS'} (LLVM {os.path.basename(lib)}, translator {xlate}, work {work})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
