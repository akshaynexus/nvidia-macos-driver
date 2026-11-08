#!/usr/bin/env python3
# NullMoth NVIDIA driver for macOS
# Copyright (c) 2026 NullMoth Systems.
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""check_tahoe_fwd_kc.py KC_AUDIT KC... — check pga_tahoe_fwd.h's forwarders against the target Mac's real collections.

Each forwarder (a naked override PG106Accel defines for an inherited slot macOS 26 no longer exports) jumps through
the base class's vtable at a fixed offset: `movq __ZTV<Base>@GOTPCREL(%rip), %r11; jmpq *<off>(%r11)`. The offset is
taken from PG106Accel's own vtable layout, which comes from the reconstructed IOAcceleratorFamily2 headers. kc-audit
skips slots the kext defines, so nothing else checks that slot (off / 8 - 2) of the REAL base vtable holds the method
the forwarder is named after. This does: it dumps each base class from the collections (kc-audit --dump) and compares
the method name and argument count at that slot. A mismatch would call the wrong method of the family at run time.

Exit 0 = every forwarder lands on its method, 1 = mismatches, 2 = usage / missing input."""
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
FWD = os.path.join(HERE, 'pga_tahoe_fwd.h')
DEF = re.compile(r'__attribute__\(\(naked\)\) .*? (\w+)::(\w+)\((.*?)\)( const)? \{ __asm__\("movq __ZTV\d+(\w+)@GOTPCREL'
                 r'\(%rip\), %r11\\n\\tjmpq \*(\d+)\(%r11\)"\); \}')


def arity(args):
    args = args.strip()
    return 0 if not args else args.count(',') + 1


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    audit, kcs = argv[1], argv[2:]
    fwds = []
    for line in open(FWD):
        m = DEF.search(line)
        if m:
            fwds.append((m.group(1), m.group(2), arity(m.group(3)), bool(m.group(4)), m.group(5), int(m.group(6)) // 8 - 2))
    if not fwds:
        print('check_tahoe_fwd_kc: no forwarders parsed from %s' % FWD)
        return 2
    bases = sorted({f[4] for f in fwds})
    cmd = [sys.executable, '-I', audit]
    for k in kcs:
        cmd += ['--kc', k]
    for b in bases:
        cmd += ['--dump', b]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    tables, cur = {}, None
    for l in out.splitlines():
        m = re.match(r'## (\w+): (\d+) slots', l)
        if m:
            cur = tables.setdefault(m.group(1), {})
            continue
        m = re.match(r'\s*(\d+) (.*)$', l)
        if cur is not None and m:
            cur[int(m.group(1))] = m.group(2)
    bad = 0
    for cls, name, n, const, base, idx in fwds:
        t = tables.get(base)
        if t is None:
            print('MISSING %s: no vtable of %s in the collections' % (cls, base))
            bad += 1
            continue
        real = t.get(idx, '<beyond the end>')
        m = re.match(r'(?:[\w:]+::)?(\w+)\((.*)\)( const)?$', real)
        ok = m and m.group(1) == name and arity(m.group(2)) == n and bool(m.group(3)) == const
        if not ok:
            print('MISMATCH %s::%s/%d -> %s slot %d holds %s' % (cls, name, n, base, idx, real))
            bad += 1
    # A failure line starts with FAIL: macos_helios300 tools/release/kc_gate.py fails a kext whose kc-preflight block
    # has a line starting with FAIL.
    if bad:
        print('FAIL check_tahoe_fwd_kc: %d of %d forwarders do not land on their method' % (bad, len(fwds)))
    else:
        print('check_tahoe_fwd_kc: %d forwarders, %d base vtables, all land on their method in the collection'
              % (len(fwds), len(bases)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
