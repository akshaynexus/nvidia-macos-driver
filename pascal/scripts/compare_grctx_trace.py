#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# compare_grctx_trace.py — diff our GR golden-context generation (the
# BAR0 writes test_grctx dumps with PG106_GRCTX_DUMP=<file>) against
# nouveau's own, taken from the owner's mmiotrace of the same GPU
# (research/hardware/helios300/raw/<trace>/mmio.trace, gitignored).
#
#   compare_grctx_trace.py <mmio.trace> <dump> [--bar0 0xa3000000]
#
# The window in the trace is nouveau's gf100_grctx_generate: from the
# FECS BIND_POINTER (0x409504 = 3) to WFI_GOLDEN_SAVE (0x409504 = 9).
# Only GR-range writes (0x400000-0x5fffff: PGRAPH incl. FECS/GPCCS) are
# compared, in order. Registers that carry a buffer address (ours and
# nouveau's VMMs place them differently) are compared by address only.
# Exit 0 on MATCH, 1 otherwise.
import sys

ADDR_ONLY = {
    0x409500,                             # the instance block (BIND_POINTER / save)
    0x40800c, 0x419004,                   # pagepool
    0x408004, 0x418e24,                   # bundle
    0x418810, 0x419848, 0x419c2c, 0x419b00,  # attrib_cb
}

def trace_writes(path, bar0):
    out, win = [], False
    with open(path) as f:
        for ln in f:
            p = ln.split()
            if len(p) < 6 or p[0] != 'W':
                continue
            a, v = int(p[4], 16), int(p[5], 16)
            if (a >> 24) != (bar0 >> 24):
                continue
            a -= bar0
            if a == 0x409504 and v == 3:
                win = True
                out = []
            if win and 0x400000 <= a < 0x600000:
                out.append((a, v))
            if win and a == 0x409504 and v == 9:
                break
    return out

def dump_writes(path):
    out, win = [], False
    with open(path) as f:
        for ln in f:
            a, v = ln.split()
            a, v = int(a, 16), int(v, 16)
            if a == 0x409504 and v == 3:
                win = True
                out = []
            if win and 0x400000 <= a < 0x600000:
                out.append((a, v))
            if win and a == 0x409504 and v == 9:
                break
    return out

def main():
    args = sys.argv[1:]
    bar0 = 0xa3000000
    if '--bar0' in args:
        i = args.index('--bar0')
        bar0 = int(args[i + 1], 16)
        del args[i:i + 2]
    t = trace_writes(args[0], bar0)
    d = dump_writes(args[1])
    print(f'trace: {len(t)} GR writes in the golden window; ours: {len(d)}')
    n = min(len(t), len(d))
    bad = 0
    for i in range(n):
        (ta, tv), (da, dv) = t[i], d[i]
        if ta != da or (tv != dv and ta not in ADDR_ONLY):
            bad += 1
            if bad <= 20:
                print(f'  #{i}: trace {ta:06x}=0x{tv:x}  ours {da:06x}=0x{dv:x}')
    if len(t) != len(d):
        print(f'  length differs: trace {len(t)} vs ours {len(d)}')
        for i in range(n, max(len(t), len(d))):
            if i - n >= 10:
                break
            if i < len(t):
                print(f'  trace only #{i}: {t[i][0]:06x}=0x{t[i][1]:x}')
            else:
                print(f'  ours only #{i}: {d[i][0]:06x}=0x{d[i][1]:x}')
    if bad or len(t) != len(d):
        print(f'MISMATCH: {bad} differing writes of {n} compared')
        return 1
    print(f'MATCH {n}/{n} GR writes (buffer addresses by register only)')
    return 0

if __name__ == '__main__':
    sys.exit(main())
