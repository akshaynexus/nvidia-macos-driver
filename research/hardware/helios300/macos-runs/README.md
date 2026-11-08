# Helios 300 — PG106 on real macOS (2026-11-07/08)

HARDWARE EVIDENCE, macOS side. The Helios 300 runs macOS 26.7.2 (25G332,
xnu-12377.161.15) with OpenCore from macos_helios300 v0.1.0. Facts below are
from the owner's machine (`log show`, `ioreg`, `kmutil showloaded`, the
OpenCore log, DiagnosticReports), read over SSH; no Apple binary or bytes of
one are kept in this repository (local copies of the kernel collections live
in `~/pascal-build/macos-26.7.2-25G332/`, for `pascal/scripts/kc-preflight.sh`).

## Boot 1: PG106 loads, firmware requests fail (2026-11-07 15:58)

- `kmutil showloaded`: `org.helios300.pg106 (0.1.0)` loaded, 0x43000 bytes,
  OpenCore-injected into the boot collection.
- `ioreg`: `PG106` attached to the GP106's IOPCIDevice: `PG106KifABI` 65536,
  `IOUserClientClass` PG106UserClient, `PG106RenderUsers` Yes,
  **`PG106Ready` No**.
- kernelmanagerd, 15:58:57.803, for all 17 `OSKextRequestResource`
  requests (tags 0-16, `gp106-acr-bl.bin` ... `gp106-gr-sw_method_init.bin`):
  `Invalid argument: Unable to create bundle at path
  /Library/Extensions/PG106.kext`. An OpenCore-injected kext has no bundle
  on disk, so the resource path can never work. Fix: the firmware is built
  into the binary (`pascal/kext/scripts/gen-firmware-embed.py`,
  `pg106_fw_embedded.h`), `OSKextRequestResource` kept only as a fallback.
- `kernelmanager_helper`: "Couldn't load bundle org.helios300.pg106fb from
  history: No bundle at /Library/Extensions/PG106FB.kext" (a stale history
  entry from an earlier copy).
- OpenCore refused PG106FB: "Dependency com.apple.iokit.IOGraphicsFamily
  was not found for kext org.helios300.pg106fb". The boot collection has no
  IOGraphicsFamily; `kmutil showloaded`: IOGraphicsFamily 600, IONDRVSupport
  600, IOAcceleratorFamily2 487.4.4 come from `SystemKernelExtensions.kc`.
  So PG106FB and PG106Accel install into `/Library/Extensions` (auxiliary
  collection); PG106 stays OpenCore-injected.
- The display stack that owns the panel: `AppleIntelKBLGraphicsFramebuffer`
  24.0.5 with three `AppleIntelFramebuffer@0..2` (one lit, 2547000 pixels),
  `IntelAccelerator` (`AppleIntelKBLGraphicsMTLDriver`), an
  `IONDRVFramebuffer` on another IOPCIDevice. The GP106's HDA function
  01:00.1 is visible (`pci10de,10f1`, acpi-path `PEG0@10000/HDAU@1`), so
  the SSDT and PG106's HDA bit work from boot.
- boot-args: `-v debug=0x100 keepsyms=1 -amfipassbeta -igfxblt
  -vi2c-force-polling revpatch=sbvmm`.

## The two panics (15:44, 15:52) are AirportAth10k, not PG106

Both DiagnosticReports are the same page fault at `IOLockLock+0x3`
(CR2 0x658: a NULL lock) from `IONetworkController::start` <-
`IO80211Controller::start` <- `AirportAth10k::start` (the second one via
`AirportAth10k::bringUp`), on the IOConfig thread. PG106 is not in either
backtrace. A third hang (`-wegnoegpu` with PG106 + PG106FB copied to
`/Library/Extensions`) left no log and is unexplained.

## What this changes in pascal/kext (commits after 9878462)

- firmware embedded, sha256-pinned (`firmware.sha256`);
- the bring-up runs on a `thread_call`, not the IOConfig thread; `IOLog`
  breadcrumbs with the time since the bring-up began; `PG106Stage` /
  `PG106Error` registry properties; a failed bring-up unarms interrupts;
- `-pg106off` kill switch;
- `pascal/scripts/kc-preflight.sh`: imports, parent class sizes and vtables
  of PG106, PG106FB and PG106Accel against the real boot + system
  collections of 25G332 (PG106 and PG106FB pass; IOFramebuffer is 0x1d0 on
  the Mac and in the SDK).
