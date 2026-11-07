<!-- SPDX-License-Identifier: GPL-2.0-only -->
# pascal/kext — GP106 XNU kext skeleton (GPL-2.0-only tree)

Phase 3 of `HANDOFF-PASCAL.md`: the XNU kernel-extension **source skeleton**
for the GP106 (GTX 1060 Mobile) driver, plus Linux-side checks that keep it
honest. **The kext is only built on macOS** (`scripts/build-macos.sh`); on
Linux, `make test` runs the pure-C tests and compiles every kext translation
unit with clang against MacKernelSDK (`-fsyntax-only`, no codegen/link).

## Licensing — read before touching anything else

- **This tree is `GPL-2.0-only`** (SPDX header on every file; full text in
  `LICENSE`). It derives from nouveau concepts (nvkm is GPL-2.0-only,
  `HELIOS300-PASCAL-RESEARCH.md` §3.1), so the kext must be GPL-2.0
  (`HANDOFF-PASCAL.md` §3).
- **It must NEVER be merged with, compiled into, or shipped inside the
  repo's PolyForm-NC trees** (`kexts/`, `plugin/`, `translator/`). The NVRM
  kext under `kexts/NVRM/` is used here as a *reference for shapes and
  patterns only* — no code is copied from it.
- The GPL kext is a **separate binary** from the nullmoth NVRM kext: only one
  NVIDIA personality should match a given GPU (`IOProbeScore` and the
  device-id gate keep PG106 off everything but GP106 mobile parts, and NVRM's
  own gate rejects Pascal), but do not co-install both on one machine.
- Userspace that talks to this kext (`tools/pg106_probe.c`, the bridge in
  `pascal/bridge/`) is a separate program and may carry its own license.

## Layout

| Path | What |
| --- | --- |
| `src/pg106_device_gate.c/.h` | **Pure C11.** Probe-time PCI-id gate (accepts the seven GP106M/GP106BM mobile ids from pci.ids: 1C20 1C21 1C22 1C23 1C60 1C61 1C62; GP104M/GP107M siblings are named in logs but refused) and the start-time BOOT_0 decision (rejects unreadable `0x00000000`/`0xffffffff` reads, requires a GP106 decode). |
| `src/pg106_uc_abi.h` | Userspace↔kext ABI 0.2: version + PMC identity selectors, fixed `uint32_t`-only structs. |
| `src/PG106.hpp/.cpp` | Driver class: `probe()` (software gate on the wide personality), `start()` (own `IOWorkLoop` + `IOCommandGate` added to it, PCI memory enable, BAR0 map, BOOT_0 identity via `pg106_pmc_probe`, `registerService()`), `stop()`/`free()` with a shared idempotent `teardown()`. |
| `src/PG106UserClient.hpp/.cpp` | IOUserClient shaped like `NVRMUserClient` (`kexts/NVRM/NVRM.cpp:298-348, 1921-1939`): root-only (`clientHasPrivilege(kIOClientPrivilegeAdministrator)`), dispatch table with exact sizes, zeroed outputs. |
| `src/klibc/`, `src/pg106_klibc.c/.h` | libc layer for pascal/core inside the kext: `klibc/stdlib.h` declares `calloc/realloc/free` bound by asm label to `pg106_k*` (implemented on `IOMalloc`/`IOFree` with a size header); `klibc/errno.h` is the kernel's `<sys/errno.h>`. `<string.h>` comes from the SDK. Applies to C files only; nothing is force-included and nothing is renamed by macro. |
| `tools/pg106_probe.c` | Userspace probe (macOS): opens the user client and prints both selectors. Built by `build-macos.sh`; used by the owner runbook. |
| `Info.plist` | Personality: `IOPCIDevice`, wide `IOPCIPrimaryMatch 0x000010de&0x0000ffff` + `IOPCIClassMatch 0x03000000&0xff000000` (the `kexts/NVRM/Info.plist` pattern, as `HANDOFF-PASCAL.md` Phase 3 specifies). Bundle id `org.helios300.pg106`, version 0.1.0. |
| `PG106.xcodeproj/` | Hand-written Xcode project, target `PG106` → `PG106.kext`. See "What Xcode compiles". |
| `scripts/build-macos.sh` | Owner-run build: fetches MacKernelSDK (pinned), runs the syntax check, `xcodebuild`, builds `pg106_probe`. |
| `scripts/kext-syntax-check.sh` | clang `-fsyntax-only` of every Sources-phase TU (read from the pbxproj) for `x86_64-apple-macos10.13`, `-mkernel`, against MacKernelSDK. |
| `scripts/symbol-audit.sh` | No macOS-only symbols/includes in the portable core; glue uses only KPI allocators, no allocator-renaming macros, only SDK headers; plist id list == gate table. |
| `stub/`, `Makefile` | Linux harness. |

## What Xcode compiles

The Sources phase of `PG106.xcodeproj` has nine files:

- kext glue: `PG106.cpp`, `PG106UserClient.cpp`, `pg106_klibc.c`
- kext pure C: `pg106_device_gate.c`
- pascal/core: `pmc.c` (used by start()), plus `mmu.c`, `acr.c`, `pushbuf.c`,
  `qmd.c` — not called yet; they are the Phase 4 building blocks and are
  compiled in so the kext build keeps proving that they build under `-mkernel`
  with the klibc layer.

The rest of `pascal/core/src/*.c` is **not** in the kext yet; it is compiled
through the same klibc headers by `stub/test_core_shim` on Linux. Add a core
file to the Sources phase when kext code first calls it;
`scripts/kext-syntax-check.sh` picks it up from the pbxproj automatically.

## Building on macOS

```
cd pascal/kext
bash scripts/build-macos.sh       # needs Xcode or the CLT: xcode-select --install
# → build/xcodebuild/Release/PG106.kext and build/pg106_probe
```

`build-macos.sh` clones MacKernelSDK (`acidanthera/MacKernelSDK` @
`3f750085caa1`) to `<repo>/MacKernelSDK` (gitignored) unless
`PG106_MACKERNELSDK` points elsewhere. Loading and testing on hardware is
`research/evidence/OWNER-RUNBOOK.md`; do not load this on a daily-use machine.

## Linux-side verification

```
make -C pascal/kext test BUILD_DIR=/tmp/pg106-kext          # gate + core-through-klibc tests,
                                                            # symbol audit, syntax check
PG106_SDK_FETCH=1 make -C pascal/kext syntax BUILD_DIR=...  # clone the pinned SDK if absent
```

The syntax check prints `SKIP` (exit 77, which `make test` tolerates) when no
clang or no SDK is available; a skip is never reported as a pass.

## What is NOT implemented yet (honest list)

- **No engine bring-up**: PFIFO/PTIMER/PFB/PPMU/PGRAPH constants exist in
  `pascal/core` but nothing touches them; the BAR0 map stops after reading
  PMC_BOOT_0.
- **No ACR secure boot**, no firmware loading in the kext.
- **No interrupts** (MSI), no power management (and per the accepted limits
  no RTD3 or reclocking — no Pascal PMU firmware, research doc §3.3).
- **No framebuffer/display** (Phase 5).
- **UserClient has two read-only selectors.** No buffers, no submission.
- **Never compiled to a binary or loaded.** The syntax check proves the
  sources are valid against MacKernelSDK's headers; it does not prove that
  `xcodebuild` accepts the hand-written project, that every symbol resolves
  against the KPI at link/load time, or that anything works on hardware.
- `PG106UserClient` has no per-process accounting like `NVRM.cpp`'s client
  table; revisit when the bridge transport lands.
