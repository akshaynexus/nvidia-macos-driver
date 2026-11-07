# PG106Accel

The IOAccelerator for the Pascal GP106 (GTX 1060 Mobile) on the Acer Predator Helios 300. It exists so that
Metal can offer the GP106 as a second, render-only `MTLDevice` through the existing plugin (`plugin/`). The design
is in `research/evidence/OFFLOAD-DESIGN.md` §3.3, and the install steps are in `research/evidence/OWNER-RUNBOOK.md`
step 4c.

Licence: PolyForm Noncommercial, like the rest of `kexts/`. The GP106 itself is driven by PG106 (`pascal/kext`,
GPL-2.0-only). This kext includes and links nothing from `pascal/`. The two kexts meet only in the I/O registry:
PG106Accel waits for a `PG106` service to publish on the same PCI device, and reads PG106's `PG106RenderUsers`
property. `cross-build.sh` fails if any file under `pascal/` ends up in the build.

## What it does and does not do

- **It is** an `IOGraphicsAccelerator2` subclass on the GP106's `IOPCIDevice`, in match category `IOAccelerator`
  beside PG106. Its personality carries NVAccel's `MetalPluginName` / `MetalPluginClassName`, but
  `MetalPluginName` is withheld until PG106 publishes and withdrawn if PG106 terminates. The plugin recognises the
  class name `PG106Accel` (`plugin/nvmtl_vk.c`) and then runs NVK's pg106 backend in headless mode.
- **It is not** a display driver. It never claims a framebuffer: its display machine refuses every one, and it has no
  display pipe. It cannot become WindowServer's GPU, and the plugin refuses WindowServer on Pascal as well.
- **It does no GPU work.** Its family objects keep CPU-side bookkeeping only:
  - event stamps are written by the CPU;
  - GPU addresses are placeholders that are never written into the GP106's MMU;
  - video memory is refused, so only system memory is available;
  - surfaces and contexts are stubs.

  The plugin renders through Vulkan, which reaches PG106's render user client.
- **Render access** follows PG106's policy. By default every local user may render. `pg106_render_users=0`, or
  `PG106RenderUsers = false` in PG106's personality, limits rendering to administrators, and PG106Accel then refuses
  its own user clients to everyone else.
- **Boot-arg `-pg106acceloff`** stops it from attaching.

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

The cross-build runs five checks:
1. It regenerates `pga_tahoe_fwd.h` from a bootstrap build. These are forwards for the inherited vtable slots that
   macOS 26 no longer exports (the mechanism NVAccel uses, `kexts/NVRM/accel/gen_tahoe_fwd.py`). It fails if the
   committed copy is stale; `PGA_REGEN=1` rewrites it.
2. The result is an `MH_KEXT_BUNDLE` with `_kmod_info`.
3. Every import resolves to an `OSBundleLibraries` entry. xnu KPIs are checked against the xnu-12377 export lists.
   IOAcceleratorFamily2 is checked against the macOS 26 binary in
   `research/prior-art/meteor-lake-xe/research/IOAcceleratorFamily2-recovery-reference.macho`.
4. No file under `pascal/` is in the build.
5. Vtable offset +0x8d8, the slot through which `IOAccelDisplayMachine::start` adds the framebuffers it finds, is
   PG106Accel's refusing `found_framebuffer`.

The reconstructed family headers are NVAccel's (`kexts/NVRM/accel/re`, `iofam`). They are shared rather than copied,
so the two accelerators cannot drift apart.

## Load order

PG106 first (OpenCore injection or `kmutil`, runbook step 1), then PG106Accel. PG106Accel links
IOAcceleratorFamily2, which exists only in macOS's own kernel collections, so it loads from the Auxiliary Kernel
Collection (`/Library/Extensions` + `kmutil`), not by OpenCore injection. Either order at boot works: PG106Accel
publishes `MetalPluginName` when PG106 appears.
