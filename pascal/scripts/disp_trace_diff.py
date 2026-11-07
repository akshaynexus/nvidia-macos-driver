#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# disp_trace_diff.py — compare the display driver's init (pascal/core
# disp_hw.c, as logged by tests/test_disp_hw with PG106_DISP_WRLOG) with what
# nouveau did on the real Helios 300 (an owner-approved mmiotrace, nouveau
# loaded with no monitor attached: display init, no modeset).
#
#   PG106_DISP_WRLOG=/tmp/ours.txt ./build/test_disp_hw
#   python3 -I scripts/disp_trace_diff.py ../research/hardware/helios300/raw/posttrace-2026-11-07/mmio.trace /tmp/ours.txt
#
# Reports, per display register class, what nouveau wrote that we do not
# (and the reverse), value mismatches on registers both write, and the
# RAMHT/ctxdma objects nouveau built through BAR2 (decoded and checked
# against the hash/context/object encoding disp_hw.c uses).
import sys

def classify(a):
    if 0x610000 <= a < 0x6a0000 or 0xd000 <= a < 0xe000 or 0x10ec00 <= a < 0x10ed00:
        return True
    return a in (0x070000,)

def load_trace(path):
    bar = {}
    w = []
    for line in open(path):
        f = line.split()
        if not f:
            continue
        if f[0] == "PCIDEV" and f[2].lower() == "10de1c20":
            bar[0] = int(f[4], 16) & ~0xf
            bar[1] = int(f[5], 16) & ~0xf
            bar[3] = int(f[7], 16) & ~0xf
        elif f[0] in ("R", "W") and bar:
            a, v = int(f[4], 16), int(f[5], 16)
            w.append((f[0], a, v))
    return bar, w

def main():
    trace, ours = sys.argv[1], sys.argv[2]
    bar, acc = load_trace(trace)
    b0 = bar[0]
    nv = [(a - b0, v) for k, a, v in acc if k == "W" and b0 <= a < b0 + (16 << 20)
          and classify(a - b0)]
    me = []
    for line in open(ours):
        f = line.split()
        if f and f[0] == "W":
            a, v = int(f[1], 16), int(f[2], 16)
            if classify(a):
                me.append((a, v))
    nvr = {}
    for a, v in nv:
        nvr.setdefault(a, []).append(v)
    mer = {}
    for a, v in me:
        mer.setdefault(a, []).append(v)
    both = sorted(set(nvr) & set(mer))
    only_nv = sorted(set(nvr) - set(mer))
    only_me = sorted(set(mer) - set(nvr))
    print(f"nouveau: {len(nv)} display writes to {len(nvr)} registers; "
          f"disp_hw: {len(me)} writes to {len(mer)} registers; {len(both)} in common")
    skip = (0x70000, 0x640000)  # flush / PUT values depend on the stream
    diff = []
    for a in both:
        if a in skip or (0x640000 <= a < 0x660000 and (a & 0xfff) == 0):
            continue
        if nvr[a][-1] != mer[a][-1]:
            diff.append(a)
    print("\nregisters both write, final value differs (expected where addresses are"
          " allocator-dependent: 0x610010 inst, 0x6114x4 push):")
    for a in diff:
        print(f"  {a:06x}: nouveau {nvr[a][-1]:08x}  disp_hw {mer[a][-1]:08x}")
    def groups(regs):
        out, cur = [], []
        for a in regs:
            if cur and a - cur[-1] > 0x20:
                out.append(cur); cur = []
            cur.append(a)
        if cur:
            out.append(cur)
        return out
    print("\nonly nouveau writes (grouped):")
    for g in groups(only_nv):
        print(f"  {g[0]:06x}..{g[-1]:06x} ({len(g)} regs) first value {nvr[g[0]][0]:08x}")
    print("\nonly disp_hw writes (grouped):")
    for g in groups(only_me):
        print(f"  {g[0]:06x}..{g[-1]:06x} ({len(g)} regs) first value {mer[g[0]][0]:08x}")

    # RAMHT / ctxdma objects written through BAR2 (BAR3 aperture).
    b3 = bar[3]
    mem = {}
    for k, a, v in acc:
        if k == "W" and b3 <= a < b3 + (32 << 20):
            mem[a - b3] = v
    # The RAMHT is where handle 0xf0000000 for chid 0 lands: slot 30.
    base = None
    for off, v in mem.items():
        if v == 0xf0000000 and (off & 0xfff) == 30 * 8:
            base = off - 30 * 8
            break
    print("\nRAMHT (nouveau, via BAR2):")
    if base is None:
        print("  not found")
        return
    def h(chid, handle):
        x = 0
        while handle:
            x ^= handle & 0x1ff
            handle >>= 9
        return x ^ (chid << 5)
    bad = 0
    for co in range(512):
        hd, ctx = mem.get(base + co * 8), mem.get(base + co * 8 + 4)
        if hd is None or ctx is None or (hd == 0 and ctx == 0):
            continue
        chid, off = ctx >> 27, (ctx >> 9) & 0x3ffff
        obj = [mem.get(base + off + 4 * i, 0) for i in range(3)]
        ok = h(chid, hd) == co and (ctx & 1) and obj[0] == 0x49
        bad += not ok
        print(f"  slot {co:3d} chid {chid:2d} handle {hd:08x} obj +{off:05x} flags {obj[0]:08x} "
              f"start {obj[1] << 8:#x} limit {(obj[2] << 8) | 0xff:#x} "
              f"{'== disp_hw encoding' if ok else 'MISMATCH'}")
    print(f"  {bad} entr{'y' if bad == 1 else 'ies'} disagree with disp_hw's hash/context/object encoding")

if __name__ == "__main__":
    main()
