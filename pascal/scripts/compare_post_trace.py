#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Diff the modelled POST (preinit + gm200_devinit_post) against a hardware
mmiotrace of nouveau, register by register.

Usage (from pascal/):
    scripts/compare_post_trace.py CAPTURE_DIR [ROM]

CAPTURE_DIR is what scripts/capture_post_trace.sh produced (mmio.trace,
bar0.txt). ROM defaults to ../research/firmware/vbios/helios300-gp106.rom.
The model trace comes from tools/post_trace_model.c, built here.

Compared: EVERY BAR0 write nouveau makes, any width, from the first BAR0
access after module load up to the first read of 0x10a040 with bit 13 set
(DEVINIT done), against the model's writes over the same span:
pg106_devinit_preinit, then gm200_devinit_post with the register-level
nvkm_falcon_reset of the GP102 PMU (disable, reset_eng x2, scrub waits,
0x084 = BOOT_0), the IMEM/DMEM uploads, the 0x5000 mailbox and pmu_exec.
Reads are not compared (nouveau's poll counts depend on timing), and only
register addresses and values are looked at, nothing is reconstructed.

Hardware writes nouveau makes that the port deliberately does not replay
are listed by class (see devinit_pmu.h, pg106_devinit_preinit) and taken
out of the in-order comparison:
  vga   0x6013d4/0x6013d5 (8-bit, nvkm_lockvgac)
  i2c   0x00d000-0x00dfff (nvkm_i2c_preinit pads + buses, DCB-derived)
  fb    0x100c10          (sysmem flush page; vram.c writes it later)
  fifo  0x000200          (nvkm_fifo_preinit PMC reset; fifo.c later)
Anything else nouveau writes that the model does not, or a different
value or order, is a MISMATCH.
Exit 0 = identical sequence (address + value), 1 = mismatch, 2 = usage.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PASCAL = os.path.dirname(HERE)
DONE_REG, DONE_BIT = 0x10a040, 0x2000


def not_replayed(a):
    if a in (0x6013d4, 0x6013d5):
        return "vga"
    if 0x00d000 <= a <= 0x00dfff:
        return "i2c"
    if a == 0x100c10:
        return "fb"
    if a == 0x000200:
        return "fifo"
    return None


def model_writes(rom):
    tool = os.path.join("/tmp", f"pg106_post_trace_model_{os.getuid()}")
    src = [os.path.join(PASCAL, "tools", "post_trace_model.c")] + sorted(
        os.path.join(PASCAL, "core", "src", f)
        for f in os.listdir(os.path.join(PASCAL, "core", "src"))
        if f.endswith(".c"))
    subprocess.run(["cc", "-std=c11", "-O1",
                    "-I", os.path.join(PASCAL, "core", "include"),
                    "-I", os.path.join(PASCAL, "bridge", "include"),
                    "-o", tool] + src, check=True)
    out = subprocess.run([tool, rom], check=True, capture_output=True,
                         text=True).stdout
    w = []
    for line in out.splitlines():
        p = line.split()
        if p[0] == "W":
            w.append((int(p[1], 16), int(p[2], 16), p[3]))
        elif p[0] == "POLL" and int(p[1], 16) == DONE_REG:
            break  # the DEVINIT-done poll ends the compared span
    return w


def hw_writes(capdir):
    bar0 = int(open(os.path.join(capdir, "bar0.txt")).read().split()[0], 16)
    # mmiotrace: "R|W width timestamp map-id phys-addr value pc pid"
    rx = re.compile(r"^([RW]) (\d) \S+ \d+ (0x[0-9a-f]+) (0x[0-9a-f]+)")
    w, done = [], False
    for line in open(os.path.join(capdir, "mmio.trace"), errors="replace"):
        m = rx.match(line)
        if not m:
            continue
        a = int(m.group(3), 16) - bar0
        if not 0 <= a < 0x1000000:
            continue
        v = int(m.group(4), 16)
        if m.group(1) == "R":
            if a == DONE_REG and v & DONE_BIT:
                done = True
                break
            continue
        w.append((a, v, int(m.group(2))))
    return w, done


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    capdir = sys.argv[1]
    rom = sys.argv[2] if len(sys.argv) == 3 else os.path.join(
        PASCAL, "..", "research", "firmware", "vbios", "helios300-gp106.rom")
    mw = model_writes(rom)
    hw, done = hw_writes(capdir)
    if not done:
        print("hardware trace never reads 0x10a040 with bit 13 set")
        return 1
    skipped = {}
    cmp_hw = []
    for a, v, width in hw:
        cls = not_replayed(a)
        if cls:
            skipped.setdefault(cls, []).append((a, v, width))
        else:
            cmp_hw.append((a, v))
    print(f"hardware: {len(hw)} BAR0 writes from module load to DEVINIT "
          f"done; model: {len(mw)} writes over the same span")
    for cls, lst in sorted(skipped.items()):
        print(f"  not replayed ({cls}): {len(lst)} writes: " + ", ".join(
            f"0x{a:06x}=0x{v:x}" for a, v, _ in lst[:10]) +
            (" ..." if len(lst) > 10 else ""))
    n = min(len(mw), len(cmp_hw))
    for i in range(n):
        a, v, ph = mw[i]
        ha, hv = cmp_hw[i]
        if (a, v) != (ha, hv):
            print(f"MISMATCH at write {i} ({ph}): model W 0x{a:06x} "
                  f"0x{v:08x}, hardware W 0x{ha:06x} 0x{hv:08x}")
            for k in range(max(0, i - 3), min(len(cmp_hw), i + 4)):
                print(f"   hw[{k}] W 0x{cmp_hw[k][0]:06x} "
                      f"0x{cmp_hw[k][1]:08x}")
            return 1
    if len(mw) != len(cmp_hw):
        more = "model" if len(mw) > len(cmp_hw) else "hardware"
        print(f"MISMATCH: {more} has {abs(len(mw) - len(cmp_hw))} extra "
              f"writes after write {n}")
        return 1
    by_phase = {}
    for _, _, ph in mw:
        by_phase[ph] = by_phase.get(ph, 0) + 1
    print(f"MATCH: all {n} compared writes equal nouveau's (address and "
          f"value, in order): " +
          ", ".join(f"{ph} {c}" for ph, c in by_phase.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
