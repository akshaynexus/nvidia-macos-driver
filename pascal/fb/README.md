# PG106FB — IOFramebuffer for the GP106's HDMI port (GPL-2.0-only)

`PG106FB.kext` makes the GTX 1060 Mobile's HDMI port a macOS display. On the
Helios 300 that port is the dGPU's only output. The VBIOS DCB has one TMDS
entry: OR mask 0x2, sublink A, connector 2 (HDMI-A), CCB 6. The internal
panel belongs to the Intel iGPU, and this kext never sees it.

**Status: never loaded.** It cross-builds and passes the KPI audit on Linux.
The display code behind it (`pascal/core/src/disp_hw.c`) has only run
against the mock display engine in `pascal/tests/test_disp_hw.c`.

## Layering

```
IOPCIDevice (10de:1c20)
  └─ PG106            pascal/kext   GPU owner: BAR0, VRAM, BAR1, IRQ, power, POST
       └─ PG106FB     pascal/fb     this kext (IOProviderClass PG106)
            ↕  pg106_kif.h (MIT): PG106FBKifABI / INFO / FLIP / FLIP_DONE
           PG106Accel kexts/        Metal accelerator (PolyForm-NC), pairs with PG106FB
```

- PG106 → PG106FB: `callPlatformFunction("org.helios300.pg106.disp_host")`
  fills `struct pg106_disp_host` (`pascal/core/include/pascal/disp_host.h`).
  The kexts have no library dependency and share no C++ symbols. PG106's side
  is `pascal/kext/src/PG106_disp.cpp`, which kdev3 owns. Until it lands,
  PG106FB logs `PG106 does not provide org.helios300.pg106.disp_host` and
  does not attach.
- The display core files are compiled into PG106FB with
  `-fvisibility=hidden`. The build fails if any `pg106_*` symbol is
  exported, which would clash with PG106's copies.

## What it does

| IOFramebuffer duty | how |
|---|---|
| connection / hotplug | GPIO line 27 (active low) edge interrupt → re-probe on a thread call → `kIOFBConnectInterruptType`; `kConnectionCheckEnable` = HPD |
| EDID / modes | DDC bit-bang on CCB 6 (port 6, GM200 hybrid pad 0) → `pg106_dhw_modes`: base DTDs, CTA DTDs, CTA VICs, DMT established; capped at 340 MHz TMDS (no SCDC); detailed timings in `getTimingInfoForDisplayMode` |
| setDisplayMode | `pg106_dhw_modeset`: SOR acquire + route, HDMI ctrl + AVI + ELD, base then core pushbuffers, supervisors 1/2/3 (VPLL through devinit, IED scripts from BIT 'U') |
| scanout / aperture | one 33.75 MiB VRAM surface (4096x2160x4), pitch = ALIGN(w*4, 256). Aperture = BAR1 subrange when PG106 can map it, else a system-memory shadow copied to VRAM 10 times a second (bring-up only) |
| vblank | head 0 vblank interrupt → `kIOFBVBLInterruptType` proc |
| cursor | 64x64 A8R8G8B8 hardware cursor: core methods + cursor PIO channel (ctrl 13 / user 17) |
| display sleep | `kIOPowerAttribute` 0 → head off, then restored |
| D3cold / wake | PG106 `power_notify`: WILL_OFF → `pg106_dhw_fini`; DID_ON (after re-POST) → `pg106_dhw_init`, EDID, mode and cursor restored |
| acceleration | `PG106FBKifABI` = 0x00010000; INFO/FLIP through `callPlatformFunction`; FLIP_DONE from the vblank once the base notifier leaves NOT_BEGUN; pending flips abort with `kIOReturnAborted` |

## Boot-args (rollback)

- `pg106fb=0`: PG106FB does not attach. The GPU still boots for compute and render.
- `pg106fb_accel=0`: no `PG106FBKifABI`, so PG106Accel never pairs. The kext
  stays a plain, unaccelerated IOFramebuffer that WindowServer draws into
  through the aperture.

## How WindowServer composites to it

- **Accelerated (owner decision 2026-11-08, the default).** PG106Accel finds
  PG106FB and hands it to `IOAccelDisplayMachine`. WindowServer composites
  the HDMI display on the GP106 and flips VRAM surfaces through FLIP, so the
  aperture is not on the hot path.
- **Unaccelerated (`pg106fb_accel=0`, or no PG106Accel).** WindowServer
  draws the HDMI display with its software path into the aperture. That is
  CPU stores into BAR1, write-combined through the BAR1 VMM. Intel's GPU
  plays no part. Expect it to be usable but slow at 1080p. Whether macOS 26
  WindowServer still drives a non-Metal framebuffer at all is the main open
  question for hardware.

## Build

```
PG106_MACOS_SDK=…/MacOSX26.5.sdk PG106_LD64=…/x86_64-apple-darwin17-ld \
PG106_XNU_CONFIG=…/xnu-12377.1.9/config  bash pascal/fb/scripts/cross-build-macos.sh
```

`make -C pascal test` runs it too, and skips it (exit 77) when the tools are missing.
