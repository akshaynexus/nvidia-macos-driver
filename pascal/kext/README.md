<!-- SPDX-License-Identifier: GPL-2.0-only -->
# pascal/kext — GP106 XNU kext skeleton (GPL-2.0-only tree)

Phase 3 of `HANDOFF-PASCAL.md`: the XNU kernel-extension **source skeleton**
for the GP106 (GTX 1060 Mobile) driver, plus a Linux stub harness that keeps
the pure-C parts building and tested on Linux. **This tree is source only —
compilation happens on the owner's Mac** (`scripts/build-macos.sh`).

## Licensing — read before touching anything else

- **This tree is `GPL-2.0-only`** (SPDX header on every file; see `LICENSE`).
  It derives from nouveau concepts (nvkm is GPL-2.0-only,
  `HELIOS300-PASCAL-RESEARCH.md` §3.1), so the kext that results from it must
  be GPL-2.0, per `HANDOFF-PASCAL.md` §3.
- **It must NEVER be merged with, compiled into, or shipped inside the
  repo's PolyForm-NC trees** (`kexts/`, `plugin/`, `translator/`). The
  PolyForm-NC license and GPL-2.0 are mutually incompatible
  (`HELIOS300-PASCAL-RESEARCH.md` §5 carries the same warning for the Meteor
  Lake project). The NVRM kext under `kexts/NVRM/` is used here as a
  *reference for shapes and patterns only* — no code is copied from it.
- The GPL kext is also a **separate binary** from the nullmoth NVRM kext: at
  runtime only one NVIDIA personality should match a given GPU
  (`IOProbeScore` and the device-id gate keep PG106 off Turing+ cards, and
  NVRM's own gate rejects Pascal), but do not co-install both on one machine.
- Userspace that talks to this kext (the future NVK bridge, Phase 4) is a
  separate program and may carry its own license; only this tree is GPL.

## Layout

| Path | What |
| --- | --- |
| `src/pg106_device_gate.c/.h` | **Pure C11** PCI-id gate + probe decision (GP10x table from `HELIOS300-PASCAL-RESEARCH.md` §1: 1C20/1C23/1C60 GP106M, 1C21/1C61 1050 Ti-on-GP106M, GP107M/GP104M siblings). No OS calls — built identically by the kext (xcodeproj) and the Linux stub (`Makefile`). |
| `src/pg106_uc_abi.h` | Userspace↔kext ABI: version handshake + PMC probe result (selectors 0/1). Plain C so the Phase 4 bridge compiles against it. |
| `src/PG106.hpp/.cpp` | Driver class: `probe()` (software gate on the wide NVIDIA personality), `start()` (work loop + command gate, BAR0 map, `pg106_pmc_probe` identity via pascal/core). |
| `src/PG106UserClient.hpp/.cpp` | IOUserClient shaped like `NVRMUserClient` (`kexts/NVRM/NVRM.cpp:298-348, 1921-1939`): `externalMethod` dispatch table, two selectors. |
| `src/pg106_klibc_shim.h`, `src/klibc/` | libc shim so pascal/core compiles into the kext (calloc/free → `kern_os_malloc/free`; errno/string headers). |
| `Info.plist` | Personality: `IOPCIDevice`, wide `IOPCIPrimaryMatch 0x000010de&0x0000ffff` + `IOPCIClassMatch 0x03000000&0xff000000` — the same wide-net pattern as `kexts/NVRM/Info.plist`. The real gate is software (`PG106::probe`). Bundle id `org.helios300.pg106`. |
| `PG106.xcodeproj/` | Hand-written Xcode project (target `PG106`, product `PG106.kext`); sources include all of `pascal/core/src/*.c`. MacKernelSDK is referenced at `$(SRCROOT)/../../MacKernelSDK/Headers` — the documented osy/MacKernelSDK usage (clone next to the checkout, point `KERNEL_FRAMEWORK_HEADERS` at `Headers`). |
| `scripts/build-macos.sh` | Owner-run build (Darwin-guarded, fails clearly on Linux). |
| `scripts/symbol-audit.sh` | Phase 3 gate: no macOS-only symbols/includes leak into the portable core. |
| `stub/`, `Makefile` | Linux harness: `make test` builds the gate logic + all of pascal/core through the shim and asserts behavior. |

## What the owner must do to build (macOS)

```
git clone --depth 1 https://github.com/osy/MacKernelSDK     # next to this repo checkout
cd pascal/kext
bash scripts/build-macos.sh                                 # needs Xcode CLT: xcode-select --install
# → PG106.kext in pascal/kext/build/xcodebuild/Release/
```

Loading, hardware testing and any install-side steps are **not part of this
phase** — see `HANDOFF-PASCAL.md` §3 (no automation on owner hardware) and the
runbook that Phase 6 will produce. Do not `kextload` this on a daily-use
machine.

## What is NOT implemented yet (honest list)

This is a Phase 3 *skeleton*: personality + gate + BAR0 identity + userspace
transport stub. In particular:

- **No engine bring-up**: PFIFO/PTIMER/PFB/PPMU/PGRAPH are decoded constants
  (`pascal/core/include/pascal/mmio.h`) but nothing touches them. No GPFIFO
  channels, no GR, no CE, no MMU wiring — the BAR0 map stops after reading
  PMC_BOOT_0.
- **No ACR secure boot** — the blob parsers exist in `pascal/core` (Phase 2
  simulator), but nothing loads firmware in the kext.
- **No interrupts** (MSI), no timers beyond the command gate, no power
  management at all (and per the accepted hard limits there will never be
  RTD3 or reclocking — no Pascal PMU firmware, research doc §3.3).
- **No framebuffer/display** (Phase 5).
- **UserClient has exactly two read-only selectors** (version handshake, PMC
  result). No buffers, no command submission — that is the Phase 4 bridge.
- **The BAR0 map is not validated on hardware.** "GP106 alive" in the log
  means a BOOT_0 word was read and decoded, nothing more (evidence rules,
  `HANDOFF-PASCAL.md` §7).
- **The Xcode project has never been opened by Xcode.** It was written by
  hand on Linux; `xcodebuild` may want trivial fixes (first open in Xcode,
  or `xcodebuild -list` will say). Same for the exact libc-surface of the
  MacKernelSDK headers vs `src/klibc/` — the shim is written so only the
  include order needs adjusting if the SDK already provides some of them.
- `PG106UserClient` inherits IOKit's default client lifecycle; there is no
  per-process accounting like `NVRM.cpp`'s client table (that kext grew it
  after real-world saturation — revisit when the bridge lands).

## Linux-side verification (what ran before this was handed over)

```
cd pascal/kext
make test        # gate tests + core-through-shim tests + symbol audit
bash scripts/symbol-audit.sh
```

Both must pass on Linux; `cd pascal && make test` (the Phase 0/2 suite) must
still pass untouched afterwards.
