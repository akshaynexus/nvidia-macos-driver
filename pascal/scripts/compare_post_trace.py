#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Diff the modelled gm200_devinit_post against a hardware mmiotrace.

Usage (from pascal/):
    scripts/compare_post_trace.py CAPTURE_DIR [ROM]

CAPTURE_DIR is what scripts/capture_post_trace.sh produced (mmio.trace,
bar0.txt). ROM defaults to ../research/firmware/vbios/helios300-gp106.rom.
The model trace comes from tools/post_trace_model.c, built here.

Compared: the host-issued writes to the PMU falcon's load/boot registers
(IMEMC/IMEMT/IMEMD, DMEMC/DMEMD, MAILBOX0, BOOTVEC, CPUCTL). Those are the
writes gm200_devinit_post makes and the model reproduces. Falcon reset (PMC
toggles, 0x10a3c0) is abstracted in the model and excluded. The hardware
trace starts at the first IMEMC write after nouveau loads and is compared
in order against the model's writes.
Exit 0 = identical sequence (address + value), 1 = mismatch, 2 = usage.
"""
import os
import re
import subprocess
import sys

FALCON = 0x10a000
CMP = {FALCON + o for o in (0x040, 0x104, 0x100, 0x180, 0x184, 0x188,
                            0x1c0, 0x1c4)}
HERE = os.path.dirname(os.path.abspath(__file__))
PASCAL = os.path.dirname(HERE)


def model_writes(rom):
    tool = os.path.join("/tmp", f"pg106_post_trace_model_{os.getuid()}")
    src = [os.path.join(PASCAL, "tools", "post_trace_model.c")] + sorted(
        os.path.join(PASCAL, "core", "src", f)
        for f in os.listdir(os.path.join(PASCAL, "core", "src"))
        if f.endswith(".c"))
    subprocess.run(["cc", "-std=c11", "-O1", "-I",
                    os.path.join(PASCAL, "core", "include"), "-o", tool]
                   + src, check=True)
    out = subprocess.run([tool, rom], check=True, capture_output=True,
                         text=True).stdout
    w = []
    for line in out.splitlines():
        p = line.split()
        if p[0] == "W":
            a, v = int(p[1], 16), int(p[2], 16)
            if a in CMP:
                w.append((a, v, p[3]))
    return w


def hw_writes(capdir):
    bar0 = int(open(os.path.join(capdir, "bar0.txt")).read().split()[0], 16)
    w = []
    # mmiotrace: "W width timestamp map-id phys-addr value pc pid"
    rx = re.compile(r"^W (\d) \S+ \d+ (0x[0-9a-f]+) (0x[0-9a-f]+)")
    for line in open(os.path.join(capdir, "mmio.trace"), errors="replace"):
        m = rx.match(line)
        if not m or m.group(1) != "4":
            continue
        a = int(m.group(2), 16) - bar0
        if a in CMP:
            w.append((a, int(m.group(3), 16)))
    # Start at the first IMEMC write (gm200_devinit_post's first upload).
    for i, (a, _) in enumerate(w):
        if a == FALCON + 0x180:
            return w[i:]
    return []


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    capdir = sys.argv[1]
    rom = sys.argv[2] if len(sys.argv) == 3 else os.path.join(
        PASCAL, "..", "research", "firmware", "vbios", "helios300-gp106.rom")
    mw, hw = model_writes(rom), hw_writes(capdir)
    print(f"model: {len(mw)} falcon load/boot writes; hardware: {len(hw)} "
          f"from first IMEMC")
    for i, (a, v, ph) in enumerate(mw):
        if i >= len(hw):
            print(f"MISMATCH: hardware trace ends at step {i} "
                  f"(model next: {ph} W 0x{a:06x} 0x{v:08x})")
            return 1
        ha, hv = hw[i]
        if (a, v) != (ha, hv):
            print(f"MISMATCH at step {i} ({ph}): model W 0x{a:06x} "
                  f"0x{v:08x}, hardware W 0x{ha:06x} 0x{hv:08x}")
            return 1
    print(f"MATCH: all {len(mw)} modelled writes equal the first "
          f"{len(mw)} hardware writes (address and value, in order)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
