# pascal/ — GP106 (GTX 1060 Mobile) driver work for the Helios 300

Three trees, specified by `HANDOFF-PASCAL.md`:

- `core/` — portable C11 library (libc only): the nvkm GP106 subset plus
  NVIDIA-header/Mesa-derived encoders. Builds and tests on plain Linux.
- `kext/` — XNU kext source that links part of the core (Phase 3). Built only
  on macOS by the owner; Linux runs stub tests, a symbol audit and a
  `-fsyntax-only` check against MacKernelSDK.
- `bridge/` — userspace `nvkmd`-shaped backend that speaks the nouveau CS uapi
  subset over an abstract transport (Phase 4). Today the transport is a mock.

**Everything here is simulation or unit-test evidence.** No hardware has been
touched, the kext has never been compiled to a binary or loaded, and no NVK
build uses the bridge yet (`HANDOFF-PASCAL.md` §7; gate status in
`research/evidence/ROADMAP.md`).

## Licensing (per file, SPDX header on each)

| Files | License | Why |
| --- | --- | --- |
| `core/` nouveau-derived: `acr`, `acr_boot`, `devinit`, `devinit_pmu`, `diag`, `dispatch`, `disp_core`, `disp_mode`, `gr_boot`, `mmio.h`, `mmu`, `pmc`, `top`, `vbios_bit` and their tests | GPL-2.0-only | nvkm is GPL-2.0-only |
| `core/` `ce`, `edid`, `pbdma`, `pushbuf`, `qmd`, `uapi` and their tests; `test_mesa_diff`, `test_tinygrad_compat`, `test_uapi_oracle` | MIT | written from NVIDIA class headers (MIT), the VESA/CTA specs, Mesa (MIT) and `nouveau_drm.h` + the nouveau uapi files (MIT) |
| `kext/` | GPL-2.0-only (`kext/LICENSE`) | links GPL core objects |
| `bridge/` | MIT | links only MIT core objects (`uapi.c`; the demo also `pushbuf.c`) |

GPL code must never reach the PolyForm-NC trees (`kexts/`, `plugin/`,
`translator/`; `HANDOFF-PASCAL.md` §3). Upstream code is not copied into the
repo: Mesa, open-gpu-kernel-modules and tinygrad are submodules under
`third_party/`; nouveau was read from a sparse torvalds/linux clone
(d48723711bae for the 2026-11-07 round). Citations name file and line.

## core/ modules

Header in `core/include/pascal/`, implementation in `core/src/`, test in
`tests/test_<name>.c` (plus the cross-module tests below).

| Module | What it models | Main upstream |
| --- | --- | --- |
| `pmc` | PMC_BOOT_0 decode, GP10x family, GP106 = 0x136, NV04/05 legacy path | nvkm `device/base.c` |
| `mmio.h` | BAR0 block map (14 blocks: PMC, PBUS, PFIFO, PTIMER 0x9000, PTOP, PFB, PPMU, PGRAPH, PDISP, the 0x640000 display channel window, PBDMA, PGPC, PRAMIN, PCCSR), address→block lookup | SDK swref headers + nouveau literals; old envytools NV50 map replaced (`research/evidence/MMIO-MAP-CORRECTIONS.md`) |
| `top` | PTOP device-info table reader (engine bases, reset/intr lines) | nvkm `top/gk104.c` |
| `mmu` | MMU v2 page tables: PDE3→PDE0 (dual) → PTE, 4K/64K PTEs, per-PDE refcounts, walk/PA decode | nvkm `vmm*.c`, SDK `dev_mmu.h` |
| `pushbuf` | method headers (SEC_OP, 12-bit address) and GPFIFO entries | `clc06f.h` |
| `qmd` | QMD v02_01 builder (what NAK and gallium use for Pascal) | `clc0c0qmd.h`/`clc1c0qmd.h`, Mesa |
| `dispatch` | channel instance (VMM join + RAMFC), runlist entry, NVK's compute program + SEND_PCAS, USERD+0x8c GP_PUT kick | nvkm `fifo/`, `nvif/chan506f.c`, NVK `nvk_cmd_dispatch.c` |
| `pbdma` | consumer-side model: GPFIFO walk, method decode, error bits | `dev_pbdma.ref`, `clc06f.h` |
| `ce` | PASCAL_DMA_COPY_B memcpy/2D + semaphore release on the last chunk | `clc1b5.h`, tinygrad |
| `acr` | ACR HS ucode, WPR/LSB header, LS image and signature parsers | nvkm `acr/`, `falcon/fw.c` |
| `acr_boot` | ACR HS boot plan, SEC2 RTOS boot, cmdq/msgq, `BOOTSTRAP_FALCON` for FECS/GPCCS | nvkm `acr/`, `falcon/`, `sec2/gp102.c` |
| `gr_boot` | post-ACR GR init (RMW steps, ZBC, FECS size polls) | nvkm `gr/gf100.c`, `gm200.c`, `gp100.c`, `gp102.c` |
| `vbios_bit` | image chain (PCIR/NPDE), BIT tokens, image0/imaged remap, `I`/`p` tables, DCB/conn/GPIO | nvkm `bios/` |
| `devinit_pmu` | GP106 POST: posted bit 0x2240c, gm200 PMU DEVINIT upload + run | nvkm `devinit/gm200.c`, `gf100.c` |
| `devinit` | host init-script interpreter, scoped to what GP10x uses it for (DCB encoder, IED, DP scripts) | nvkm `bios/init.c` |
| `diag` | 0xbad*/0xdead* readback decoder, liveness probe, falcon fault verdict, WPR window read | pascal-egpu FINDINGS, SDK, nvkm |
| `edid` | E-EDID base + all CTA-861 blocks | VESA E-EDID 1.4, CTA-861 |
| `disp_mode` | mode → core917d/base907c method streams in commit order | nvkm `disp/`, nouveau `dispnv50/` |
| `disp_core` | display engine BAR0 side: init, DMA channels + PUT, SOR route, HDMI, supervisor 1/2/3, DP link training vs a simulated sink | nvkm `disp/gf119.c`, `gp102.c`, `dp.c` |
| `uapi` | nouveau GETPARAM/VM_INIT/VM_BIND/EXEC structs and kernel-matching validators | `nouveau_drm.h`, `nouveau_uvmm.c`, `nouveau_exec.c` |

Cross-module tests: `test_bringup` (one shared mock bus, probe → devinit →
ACR → MMU → display → submit, run twice), `test_mesa_diff` (encoders vs
Mesa's generated emitters, needs `tests/mesa_ref/`), `test_tinygrad_compat`,
`test_uapi_oracle` (live nouveau oracle, skips without a nouveau render node),
`test_vbios_real`/`test_devinit_real` (fetched ROMs, skip when absent),
`test_edid_corpus` (36 real EDIDs, compared with `edid-decode`).

## Build and test

Needs cc (gcc or clang), make, python3. `BUILD_DIR` defaults to `build/` and
may be absolute.

    make -C pascal test BUILD_DIR=/tmp/pg106     # everything below
    make -C pascal all  BUILD_DIR=/tmp/pg106     # core test binaries only
    make -C pascal coverage                      # scripts/coverage.sh
    make -C pascal clean BUILD_DIR=/tmp/pg106

`make test` runs every core test binary, then `kext/` (`test_device_gate`,
`test_core_shim`, `symbol-audit.sh`, `kext-syntax-check.sh`) into
`$BUILD_DIR/kext`, then `bridge/` (`test_bridge`) into `$BUILD_DIR/bridge`,
then `helios300-gp106-simtest.py`. The kext syntax check prints `SKIP`
without clang or MacKernelSDK; `PG106_SDK_FETCH=1` clones the pinned SDK into
`$BUILD_DIR`.

Inputs that are not committed:

| Input | How to get it | Without it |
| --- | --- | --- |
| `tests/mesa_ref/` | `scripts/gen_mesa_ref.sh` (from `third_party/mesa`, else GitLab at the pinned commit); `make all` runs it when missing | `make` stops |
| `research/firmware/vbios/*.rom` | `scripts/fetch_vbios.sh` (Wayback captures, sha256-checked) | real-ROM tests PASS-SKIP |
| MacKernelSDK | `PG106_SDK_FETCH=1` or `PG106_MACKERNELSDK=<path>` | syntax check SKIP |

Committed inputs: `research/firmware/gp106/` (linux-firmware ACR/GR/SEC2,
NVIDIA licence, see its README) and `research/reference/edid/`.

Other scripts: `scripts/coverage.sh` writes
`research/evidence/PHASE0-COVERAGE.{txt,csv}` from a private temp build
(`core/src` only; bridge and kext are not instrumented),
`scripts/edid_corpus_diff.sh` (EDID diff against `edid-decode`),
`scripts/uapi_probe.sh` (read-only nouveau oracle probe).
