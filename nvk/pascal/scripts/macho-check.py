#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check that a Mach-O x86_64 dylib (or executable) loads on macOS.

  macho-check.py <dylib> <MacOSX.sdk>

1. The file is a Mach-O x86_64 MH_DYLIB/MH_EXECUTE (llvm-objdump --macho --private-header).
2. Every LC_LOAD_DYLIB names a library the SDK has a .tbd stub for.
3. Every undefined symbol is bound to one of those libraries (two-level
   namespace, `(from <lib>)` in llvm-nm -m), and that library or one it
   re-exports exports the symbol for x86_64-macos according to its .tbd.
   A symbol with no library ("dynamically looked up", from
   `-undefined dynamic_lookup`, which Mesa's darwin build uses) is an error
   unless it is weak: a strong one makes dlopen fail at load time.

Prints a report and exits 0 only if all checks pass. Needs llvm-objdump,
llvm-nm (any LLVM >= 15) and PyYAML.
"""
import os, re, subprocess, sys
import yaml

def run(*a):
    return subprocess.run(a, check=True, capture_output=True, text=True).stdout

class TbdLoader(yaml.SafeLoader):
    pass
TbdLoader.add_multi_constructor('!', lambda l, s, n: l.construct_mapping(n, deep=True))

def tbd_docs(path):
    with open(path) as f:
        text = f.read()
    if text.lstrip().startswith('{'):
        sys.exit(f'{path}: TBD v5 (JSON) is not handled; this SDK should ship v4 YAML')
    for d in yaml.load_all(text, Loader=TbdLoader):
        if not d:
            continue
        name = d['install-name']
        syms, reex = set(), []
        for e in d.get('reexported-libraries', []) or []:
            if any(t.startswith('x86_64-macos') for t in e.get('targets', [])):
                reex += e.get('libraries', [])
        for key in ('exports', 'reexports'):
            for e in d.get(key, []) or []:
                if not any(t.startswith('x86_64-macos') for t in e.get('targets', [])):
                    continue
                syms.update(e.get('symbols', []) or [])
                syms.update(e.get('weak-symbols', []) or [])
                syms.update(e.get('thread-local-symbols', []) or [])
                for c in e.get('objc-classes', []) or []:
                    syms.add('_OBJC_CLASS_$_' + c); syms.add('_OBJC_METACLASS_$_' + c)
                for c in e.get('objc-eh-types', []) or []:
                    syms.add('_OBJC_EHTYPE_$_' + c)
                for c in e.get('objc-ivars', []) or []:
                    syms.add('_OBJC_IVAR_$_' + c)
        yield name, syms, reex

def sdk_tbd_for(sdk, install_name):
    # /usr/lib/libSystem.B.dylib -> <sdk>/usr/lib/libSystem.B.tbd ; frameworks the same way
    base = os.path.splitext(install_name)[0] if install_name.endswith('.dylib') else install_name
    cands = [sdk + base + '.tbd']
    m = re.match(r'(.*/(\w+)\.framework)/Versions/\w+/(\w+)$', install_name)
    if m:                                    # SDK frameworks keep <Name>.tbd at the top
        cands.append(sdk + m.group(1) + '/' + m.group(3) + '.tbd')
    for p in cands:
        if os.path.exists(p):
            return p
    return None

def main():
    dylib, sdk = sys.argv[1], sys.argv[2]
    ok = True
    hdr = run('llvm-objdump', '--macho', '--private-header', dylib)
    m = re.search(r'MH_MAGIC_64\s+X86_64\s+\S+\s+\S+\s+(\w+)', hdr)
    print(f'file: {dylib} ({os.path.getsize(dylib)} bytes)')
    if not m or m.group(1) not in ('DYLIB', 'EXECUTE', 'BUNDLE'):
        print('FAIL not a Mach-O x86_64 dylib/executable/bundle:\n' + hdr); return 1
    print(f'  Mach-O x86_64 {m.group(1)}')
    lc = run('llvm-objdump', '--macho', '--private-headers', dylib)
    mv = re.search(r'cmd LC_BUILD_VERSION.*?platform (\S+).*?sdk (\S+).*?minos (\S+)', lc, re.S)
    if mv:
        print(f'  platform {mv.group(1)} minos {mv.group(3)} sdk {mv.group(2)}')
    else:
        print('FAIL no LC_BUILD_VERSION'); ok = False
    idn = re.search(r'cmd LC_ID_DYLIB.*?name (\S+)', lc, re.S)
    if idn:
        print(f'  install name {idn.group(1)}')
    used = [l.split()[0] for l in run('llvm-objdump', '--macho', '--dylibs-used', dylib).splitlines()[1:]]
    if idn and used and used[0] == idn.group(1):
        used = used[1:]                      # --dylibs-used lists the dylib's own LC_ID_DYLIB first
    # library short name (llvm-nm "(from X)") -> install name
    short = {}
    exports = {}          # install name -> (syms, reexports)
    def load(name):
        if name in exports:
            return
        t = sdk_tbd_for(sdk, name)
        exports[name] = (set(), [])
        if t is None:
            return
        for n, s, r in tbd_docs(t):
            exports[n] = (s, r)
    for u in used:
        b = os.path.basename(u)
        short[b.split('.')[0]] = u
        if b.startswith('lib') and '.' in b:
            short[b.split('.')[0]] = u
        load(u)
        if sdk_tbd_for(sdk, u) is None:
            print(f'FAIL {u}: no .tbd in the SDK'); ok = False
        else:
            print(f'  links {u}')
    def provides(name, sym, seen=()):
        if name in seen:
            return False
        load(name)
        s, r = exports.get(name, (set(), []))
        if sym in s:
            return True
        return any(provides(x, sym, seen + (name,)) for x in r)
    undef = [l for l in run('llvm-nm', '-m', '--undefined-only', dylib).splitlines() if l.strip()]
    bad, flat, weakflat, bound, n = [], [], [], 0, 0
    for l in undef:
        n += 1
        mm = re.match(r'\s*\(undefined\)\s+(weak )?external (\S+)(?: \(from (\S+)\))?(?: \(dynamically looked up\))?', l)
        if not mm:
            bad.append('unparsed: ' + l); continue
        weak, sym, lib = mm.group(1), mm.group(2), mm.group(3)
        if lib is None:
            (weakflat if weak else flat).append(sym)
            continue
        inst = short.get(lib)
        if inst is None or not provides(inst, sym):
            bad.append(f'{sym} (from {lib}) not exported by the SDK stub')
        else:
            bound += 1
    print(f'  {n} undefined symbols:')
    print(f'    {bound} bound to an SDK library and exported by its .tbd (x86_64-macos)')
    print(f'    {len(weakflat)} weak, dynamically looked up (NULL if absent; Mesa entrypoint tables)')
    print(f'    {len(flat)} strong, dynamically looked up (must be 0)')
    print(f'    {len(bad)} bound but not exported by the SDK (must be 0)')
    if os.environ.get('MACHO_CHECK_SELFTEST'):
        assert provides('/usr/lib/libSystem.B.dylib', '_malloc')
        assert not provides('/usr/lib/libSystem.B.dylib', '_no_such_symbol_xyz')
        print('  selftest: _malloc found in libSystem re-exports, a bogus symbol is not')
    for s in flat:
        print(f'FAIL strong symbol with no library (dynamic_lookup): {s}'); ok = False
    for b in bad:
        print(f'FAIL {b}'); ok = False
    print('PASS' if ok else 'FAILED')
    return 0 if ok else 1

sys.exit(main())
