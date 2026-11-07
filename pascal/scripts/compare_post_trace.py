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
writes gm200_devinit_post makes and the model reproduces. A falcon reset
is one RESET step in the model. On hardware it also writes inside the
compared set: nvkm_falcon_reset -> gm200_flcn_reset_wait_mem_scrubbing does
nvkm_falcon_mask(falcon, 0x040, 0, 0) (falcon/gm200.c:143), an RMW that
writes MAILBOX0's current value back. At each model RESET, hardware writes
up to the next IMEMC (the start of the following load) are therefore
attributed to the reset, skipped and printed. The hardware trace starts at
the first IMEMC write after nouveau loads, which comes after the first
reset, and is compared in order.
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
        elif p[0] == "RESET" and w:  # first reset precedes the trace start
            w.append(("RESET", None, p[3]))
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
    nw = sum(1 for e in mw if e[0] != "RESET")
    print(f"model: {nw} falcon load/boot writes; hardware: {len(hw)} "
          f"from first IMEMC")
    j = 0
    for i, (a, v, ph) in enumerate(mw):
        if a == "RESET":
            k = j
            while j < len(hw) and hw[j][0] != FALCON + 0x180:
                j += 1
            for ha, hv in hw[k:j]:
                print(f"  reset ({ph}): skipped hardware W 0x{ha:06x} "
                      f"0x{hv:08x}")
            continue
        if j >= len(hw):
            print(f"MISMATCH: hardware trace ends at model step {i} "
                  f"(model next: {ph} W 0x{a:06x} 0x{v:08x})")
            return 1
        ha, hv = hw[j]
        if (a, v) != (ha, hv):
            print(f"MISMATCH at model step {i} / hardware {j} ({ph}): "
                  f"model W 0x{a:06x} 0x{v:08x}, hardware W 0x{ha:06x} "
                  f"0x{hv:08x}")
            return 1
        j += 1
    print(f"MATCH: all {nw} modelled writes equal the hardware writes "
          f"(address and value, in order); {len(hw) - j} hardware writes "
          f"follow the modelled sequence")
    return 0


if __name__ == "__main__":
    sys.exit(main())
