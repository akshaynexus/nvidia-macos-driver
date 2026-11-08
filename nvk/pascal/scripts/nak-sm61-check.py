#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Cross-check NAK's SM 6.1 machine code against NVIDIA's own disassembler.

  nak-sm61-check.py <nvdisasm> <log> [<log> ...]

Each <log> is the stderr of a Vulkan program run with NAK_DEBUG=print on
NVK (here: the pg106 sim ICD, which reports a GP106, SM 6.1). For every
shader in it, this script

  1. takes the "Encoded shader:" words NAK emitted and disassembles them
     with `nvdisasm -b SM61` (CUDA 12.x; CUDA 13 dropped SM 5x/6x),
  2. fails on any word nvdisasm cannot decode,
  3. compares the instruction sequence with NAK's last IR listing
     ("NAK IR after calc_instr_deps"): same opcodes in the same order
     (NOPs ignored, NAK op names mapped to SASS mnemonics), the same
     predicate guards, and the same register destinations.

It proves the SM50-family encoder writes what NAK meant, as NVIDIA's tools
read it. It does not prove the shader is right (that is NAK's IR) or runs
(that needs the GPU).
"""
import os, re, struct, subprocess, sys, tempfile

# NAK sm50 op name (first token, without modifiers) -> SASS mnemonic
OPMAP = {
    'iadd2': 'IADD', 'iadd3': 'IADD3', 'mov': 'MOV', 's2r': 'S2R', 'cs2r': 'CS2R',
    'shl': 'SHL', 'shr': 'SHR', 'shf': 'SHF', 'isetp': 'ISETP', 'fsetp': 'FSETP',
    'dsetp': 'DSETP', 'psetp': 'PSETP', 'ssy': 'SSY', 'sync': 'SYNC', 'bra': 'BRA',
    'exit': 'EXIT', 'xmad': 'XMAD', 'lop2': 'LOP', 'lop3': 'LOP3', 'sel': 'SEL',
    'prmt': 'PRMT', 'fadd': 'FADD', 'fmul': 'FMUL', 'ffma': 'FFMA', 'fmnmx': 'FMNMX',
    'imnmx': 'IMNMX', 'mufu': 'MUFU', 'i2f': 'I2F', 'f2i': 'F2I', 'f2f': 'F2F',
    'i2i': 'I2I', 'flo': 'FLO', 'popc': 'POPC', 'bfe': 'BFE', 'bfi': 'BFI',
    'bar': 'BAR', 'membar': 'MEMBAR', 'ldc': 'LDC', 'ipa': 'IPA', 'ald': 'ALD',
    'ast': 'AST', 'out': 'OUT', 'kill': 'KIL', 'shfl': 'SHFL', 'vote': 'VOTE',
    'tex': 'TEX', 'tld': 'TLD', 'tld4': 'TLD4', 'txq': 'TXQ', 'suld': 'SULD', 'sust': 'SUST',
    'imad': 'IMAD', 'isberd': 'ISBERD', 'p2r': 'P2R', 'r2p': 'R2P', 'iabs': 'IABS',
    'brk': 'BRK', 'pbk': 'PBK', 'cont': 'CONT', 'pcnt': 'PCNT', 'cal': 'CAL', 'ret': 'RET',
    'dadd': 'DADD', 'dmul': 'DMUL', 'dfma': 'DFMA', 'hadd2': 'HADD2', 'hmul2': 'HMUL2',
    'hfma2': 'HFMA2', 'fswzadd': 'FSWZADD', 'fchk': 'FCHK', 'depbar': 'DEPBAR',
    'cctl': 'CCTL', 'warpsync': 'WARPSYNC', 'bssy': 'BSSY',
}
MEM = {'global': 'G', 'shared': 'S', 'local': 'L'}

def nak_op(line):
    """'@!p0  r2 c0 = iadd2 r0 c[...]  // delay=6' -> ('IADD', '!P0', 'R2')"""
    line = line.split('//')[0].strip()
    pred = ''
    m = re.match(r'@(!?)%?(p\w+)\s+(.*)', line)
    if m:
        pred, line = m.group(1) + m.group(2).upper(), m.group(3)
    dst = ''
    if ' = ' in line:
        d, line = line.split(' = ', 1)
        regs = [x for x in d.split() if re.match(r'%?r\d+(\.\.\d+)?$', x)]   # r8 or r8..12
        dst = regs[0].lstrip('%').split('..')[0].upper() if regs else ''
    tok = line.split()[0]
    parts = tok.split('.')
    base = parts[0]
    if base in ('st', 'ld', 'atom'):
        space = next((MEM[p] for p in parts[1:] if p in MEM), '')
        if base == 'atom':
            return ('ATOM' if space == 'G' else 'ATOM' + space), pred, dst
        return base.upper() + space, pred, dst
    return OPMAP.get(base, base.upper()), pred, dst

def nvd_op(line):
    m = re.match(r'\s*/\*([0-9a-f]{4,})\*/\s+(.*?)\s*;?\s*/\*', line)
    if not m:
        return None
    ins = m.group(2).rstrip(' ;')
    pred = ''
    pm = re.match(r'@(!?)(P\d|PT)\s+(.*)', ins)
    if pm:
        pred, ins = pm.group(1) + pm.group(2), pm.group(3)
    mn = ins.split()[0].split('.')[0]
    if mn.endswith('32I'):                   # LOP32I, FMUL32I...: NAK picks the 32-bit-immediate form at encode time
        mn = mn[:-3]
    ops = ins[len(ins.split()[0]):].strip()
    dst = ''
    dm = re.match(r'(R\d+|RZ)\b', ops)
    if dm and mn not in ('ST', 'STG', 'STS', 'STL', 'RED', 'BRA', 'SSY', 'SYNC', 'EXIT', 'BAR', 'MEMBAR', 'SUST', 'AST', 'OUT'):
        dst = dm.group(1)
    return mn, pred, dst

def shaders(log):
    lines = open(log, errors='replace').read().split('\n')
    last_ir, cur, stage = None, None, '?'
    i = 0
    while i < len(lines):
        l = lines[i]
        if l.startswith('NAK IR after '):
            cur = (l, [])
            last_ir = cur
        elif cur is not None and l.startswith(('block', '}')):
            pass
        elif cur is not None and l.startswith(('      ', '@')) and l.strip():
            cur[1].append(l)
        elif l.startswith('Stage:'):
            stage = l.split(':', 1)[1].strip()
            cur = None
        elif l.startswith('Encoded shader:'):
            words = []
            i += 1
            while i < len(lines) and lines[i].startswith('  ') and lines[i].strip():
                words += [int(x, 16) for x in lines[i].split()]
                i += 1
            yield stage, last_ir, words
            last_ir = None
            continue
        elif not l.startswith(' ') and l and not l.startswith(('block', '}')):
            cur = None if not l.startswith('NAK IR') else cur
        i += 1

def main():
    nvd, logs = sys.argv[1], sys.argv[2:]
    total = bad = 0
    for log in logs:
        for k, (stage, ir, words) in enumerate(shaders(log)):
            total += 1
            name = f'{os.path.basename(log)}#{k} ({stage}, {len(words)} words)'
            with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
                f.write(struct.pack('<%dI' % len(words), *words))
            r = subprocess.run([nvd, '-b', 'SM61', '-hex', f.name], capture_output=True, text=True)
            os.unlink(f.name)
            if r.returncode != 0 or re.search(r'\?\?|INVALID|[Ee]rror', r.stdout + r.stderr):
                print(f'FAIL {name}: nvdisasm could not decode it\n{r.stdout}{r.stderr}'); bad += 1; continue
            sass = [x for x in (nvd_op(l) for l in r.stdout.split('\n')) if x and x[0] != 'NOP']
            if ir is None:
                print(f'FAIL {name}: no NAK IR listing before it'); bad += 1; continue
            nak = [x for x in (nak_op(l) for l in ir[1]) if x[0] != 'NOP']
            diffs = [(j, a, b) for j, (a, b) in enumerate(zip(nak, sass)) if a != b]
            if len(nak) != len(sass) or diffs:
                bad += 1
                print(f'FAIL {name}: NAK {len(nak)} instructions, nvdisasm {len(sass)}')
                for j, a, b in diffs[:10]:
                    print(f'    #{j}: NAK {a}  nvdisasm {b}')
                continue
            ops = sorted(set(x[0] for x in sass))
            print(f'PASS {name}: {len(sass)} instructions match ({" ".join(ops)})')
    print(f'{total - bad}/{total} shaders: NAK SM 6.1 encoding = nvdisasm decoding')
    return 1 if bad or not total else 0

sys.exit(main())
