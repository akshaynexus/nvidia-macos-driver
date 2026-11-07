# PG106Accel

The IOAccelerator for the Pascal GP106 (GTX 1060 Mobile) on the Acer Predator Helios 300. It has two jobs:
- **render offload**: Metal offers the GP106 as a second `MTLDevice`, through the existing plugin (`plugin/`);
- **the HDMI display** (owner decision 2026-11-08): WindowServer composites the external HDMI monitor on the GP106.

The internal panel stays on the Intel HD 630. The design is in `research/evidence/OFFLOAD-DESIGN.md` §3.3
(offload) and §3.4 (display-attached mode). The install and test steps are in
`research/evidence/OWNER-RUNBOOK.md` steps 4c and 4e.

**Status.** Cross-built and audited on Linux only. Nothing here has loaded on a Mac, and no frame has reached an
HDMI screen through it.

Licence: PolyForm Noncommercial, like the rest of `kexts/`. Two GPL-2.0-only kexts are on the other side:
- PG106 (`pascal/kext`) drives the GP106;
- PG106FB (`pascal/fb`) is its HDMI IOFramebuffer.

This kext includes and links nothing of either. They meet through IOKit only:
- registry properties: `PG106Ready`, `PG106RenderUsers`, `PG106KifABI`, `PG106FBKifABI`;
- the calls in `pascal/bridge/include/pg106_kif.h`, which are `callPlatformFunction` on the other side's service.

That header is MIT, and it is the only file from `pascal/` in the build. `cross-build.sh` fails if any other file
from there gets in, or if the header stops being MIT.

## What it does

- **It is** an `IOGraphicsAccelerator2` subclass on the GP106's `IOPCIDevice`, in match category `IOAccelerator`
  beside PG106. Its personality carries NVAccel's `MetalPluginName` / `MetalPluginClassName`.
  - `MetalPluginName` is withheld until PG106 on the same PCI device reports `PG106Ready = true`. In display mode
    it is held up to 10 s more, until PG106FB has been offered the display pipe. It is withdrawn if PG106
    terminates.
  - `PG106Ready` is polled every 2 s and latched: D3cold sets it false again, and that does not hide the GPU.
  - `PG106AccelState` says where it is: `waiting for PG106`, `waiting for PG106Ready`, `waiting for PG106FB`,
    `Metal offered, headless`, or `Metal offered, HDMI display attached`.
- **The HDMI display.** The display machine accepts exactly one framebuffer: a `PG106FB` on its own PCI device,
  with a compatible `PG106FBKifABI`, after `PG106Ready`, and only when a frame can reach it (`pga_fb_decide` in
  `pga_logic.h`). Everything else is refused, the Intel panel's framebuffer included. The family then creates the
  display pipe, and PG106Accel publishes `PG106AccelDisplay = true` and `IOAccelDisplayPipeCapabilities`. The
  plugin gives WindowServer the GP106 only then (`plugin/nvmtl_ws_gpu.h`). Each of WindowServer's swaps goes
  through a worker on PG106Accel's own work loop:
  1. The IOSurface (system memory) is mapped into PG106's accelerator VM.
  2. PG106's copy engine copies it into a three-buffer VRAM scanout ring.
  3. PG106FB flips to that buffer at vblank.
  4. `FLIP_DONE` completes the transaction.

  Without the copy channel the CPU fills the ring through BAR1. Without VRAM through the kif (PG106 has VRAM, since PG106FB needs it, but PG106KifABI / CAP_VRAM may not be
  published yet), the CPU copies into PG106FB's
  own scanout. `PG106AccelFramePath` says which path is in use.
- **VRAM, GPU addresses, stamps** come from PG106, as far as its INFO cap bits allow.
  - Family video memory is PG106 VRAM (BAR1-mapped), and the GP106's VRAM is reported (`tmpTotalVRAM`,
    `VRAM,totalMB`).
  - Family GPU VAs come from PG106's accelerator VM. VRAM maps and wired system-memory maps are committed into its
    page tables.
  - Event stamps are semaphore releases on the copy channel, written by the GPU. The CPU writes them when there is
    no channel, and `eventTimeout` completes a release that never lands.
  - Until PG106 publishes `PG106KifABI` (kdev3's provider is not written yet), all of this falls back to what the
    headless version did: no family VRAM, placeholder addresses, CPU stamps.
- **Still stubs**: the family's contexts and legacy surfaces. Their pure slots are not reconstructed well enough to
  submit anything. The plugin renders with NVK through PG106's render user client, as before.
- **Render access** follows PG106's policy. By default every local user may render. `pg106_render_users=0`, or
  `PG106RenderUsers = false` in PG106's personality, limits it to administrators, and PG106Accel then refuses its
  own user clients to everyone else.
- **Counters**: `PG106AccelStats` (frames, flips, copies, drops, stamps, VRAM allocations, VA/map sources),
  refreshed every 2 s.

## Boot-args

| boot-arg | effect |
| --- | --- |
| `-pg106acceloff` | does not attach at all |
| `pg106accel_display=0` | headless: no framebuffer, render offload only (the role before 2026-11-08) |
| `pg106accel_ce=0` | the CPU copies frames instead of the copy engine |
| `pg106accel_gpustamps=0` | the CPU writes event stamps |
| `pg106accel_vram=0` | no family VRAM |
| `pg106accel_aperture=1` | lets the family map VRAM resources through BAR1 (off by default, as in NVAccel) |

On the PG106FB side, `pg106fb_accel=0` keeps PG106FB from offering itself, and `pg106fb=0` turns it off.

The source comment at the top of `pg106-accel.cpp` gives the reason for each part kept from NVAccel.

## Building

On the owner's Mac, macOS 26 only (Tahoe):

```sh
bash kexts/PG106Accel/build.sh ~/pascal-test/accel     # -> ~/pascal-test/accel/PG106Accel.kext
```

On Linux, with the toolchain `pascal/kext/scripts/cross-build-macos.sh` uses:

```sh
PGA_MACOS_SDK=…/MacOSX26.5.sdk PGA_LD64=…/x86_64-apple-darwin17-ld PGA_XNU_CONFIG=…/xnu/config \
  bash kexts/PG106Accel/cross-build.sh
```

The cross-build runs these checks:
0. The host tests of the decision logic (`tests/run.sh`; also runnable on its own with only a C compiler).
1. It regenerates `pga_tahoe_fwd.h` from a bootstrap build. These are forwards for the inherited vtable slots that
   macOS 26 no longer exports (the mechanism NVAccel uses, `kexts/NVRM/accel/gen_tahoe_fwd.py`). It fails if the
   committed copy is stale; `PGA_REGEN=1` rewrites it.
2. The result is an `MH_KEXT_BUNDLE` with `_kmod_info`.
3. Every import resolves to an `OSBundleLibraries` entry. xnu KPIs are checked against the xnu-12377 export lists.
   IOAcceleratorFamily2 is checked against the macOS 26 binary in
   `research/prior-art/meteor-lake-xe/research/IOAcceleratorFamily2-recovery-reference.macho`.
4. From `pascal/`, only `pascal/bridge/include/pg106_kif.h` is in the build, and it is SPDX MIT.
5. Vtable offset +0x8d8, the slot through which `IOAccelDisplayMachine::start` adds the framebuffers it finds, is
   PG106Accel's `found_framebuffer`. It names `PG106FB`, decides through `fbAcceptable` (`pga_fb_decide`), and only
   then calls the family's `found_framebuffer` through IOAccelLegacyDisplayMachine's vtable.
6. The display pipe is PG106Accel's: the accelerator's `newDisplayPipe` (slot 331), and the pipe's
   `performTransaction` / `isTransactionComplete` / `validateTransaction`.

The reconstructed family headers are NVAccel's (`kexts/NVRM/accel/re`, `iofam`). They are shared rather than copied,
so the two accelerators cannot drift apart.

## Load order

PG106 first (OpenCore injection or `kmutil`, runbook step 1), then PG106FB if the HDMI display is wanted, then
PG106Accel. PG106Accel links
IOAcceleratorFamily2, which exists only in macOS's own kernel collections, so it loads from the Auxiliary Kernel
Collection (`/Library/Extensions` + `kmutil`), not by OpenCore injection. Either order at boot works: PG106Accel
publishes `MetalPluginName` when PG106 is ready, and pairs with PG106FB whenever it publishes.
