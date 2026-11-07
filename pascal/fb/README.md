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
| EDID / modes | DDC bit-bang on CCB 6 (port 6, GM200 hybrid pad 0), HF-EEODB honoured → `pg106_dhw_modes`: base DTDs, CTA DTDs, DisplayID Type I/VII timings, CTA VICs (incl. 4K50/60 VIC 96/97, 4096x2160, 1080p100/120), DMT established; up to 48 modes; refresh as IOFixed1616; preferred = Default + Native; detailed timings in `getTimingInfoForDisplayMode` |
| TMDS limit | nouveau's: **594 MHz** for a sink with HDMI 2.0 scrambling (HF-VSDB/SCDB with SCDC_Present), else 340 MHz, min'd with the sink's Max_TMDS; DVI sinks 165 MHz. So 4K60 (594), 1440p144 (~584), 1080p240 (~571) where the monitor lists them (DISP-PLAN §12) |
| setDisplayMode | `pg106_dhw_modeset`: SOR acquire + route; for HDMI 2.0 the sink's SCDC TMDS_Config (1/40 + scrambling) over DDC 0x54 first, then HDMI ctrl + SOR scrambler, AVI + VSI; base then core pushbuffers, supervisors 1/2/3 (VPLL through devinit, IED scripts from BIT 'U'); then Scrambling_Status read-back and the audio ELD |
| HDMI audio | ELD + presence to the GPU's HDA after the modeset (pin NID 4 + SOR = 7 on the Helios); the AppleHDA pairing properties on this framebuffer; checks (read-only) that PG106 enabled the HDA PCI function; `kConnectionEnableAudio` / `kConnectionAudioStreaming` (see "HDMI audio" below) |
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
- `pg106fb_tmds_khz=N`: offer only modes up to N kHz. `340000` drops every
  HDMI 2.0 (scrambled) mode; `165000` leaves single-link-DVI-class modes.
  The rollback when a > 340 MHz mode is dark.
- The GPU's HDA function (0x488 bit 25 + bus rescan) is PG106's
  (`pg106_hda=0` turns it off there, deedcd5). PG106FB only reads the bit
  and logs when it is clear.

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

## HDMI audio

**Status: implemented, never run on a Mac.** DISP-PLAN.md §13 has the
mechanism and sources; BLOCKERS B16 item 9 lists the unknowns. Pieces:

1. **The HDA function must exist.** The Helios firmware hides 01:00.1
   unless HDMI is plugged at power-on (SSDT11 `MLTF`). Add
   `acpi/SSDT-PG106-HDAU.aml` (source next to it). PG106 also sets the bit
   and rescans (at start and every power-on, before DID_ON), but NMHDA
   counts NVIDIA audio functions only once, when AppleHDAController loads,
   so the SSDT (or HDMI at boot) is what matters.
2. **NMHDA.kext** (`kexts/NMHDA`, PolyForm-NC, a Lilu plugin). It makes
   AppleHDAController accept 10de:10f1 and announces the graphics link
   (0xE3FF8000) for the GPU. PG106FB does neither.
3. **AppleALC + Lilu**, as for the internal codec.
4. **The pairing**: the same `hda-gfx` on the GPU and its HDA function.
   PG106FB copies it onto itself and adds `audio-codec-info` = `00 01 07 00`
   (codec 0, AFG 1, pin NID 4 + SOR; SOR3 is the Helios's only HDA SOR),
   `connector-type` 0x800, `port-number` 0, `av-signal-type` 8,
   `IODisplayEDID`, `nvda-eld`. The override is `PG106FBAudioCodecInfo`
   (4-byte data) in the GPU's DeviceProperties.

OpenCore `config.plist` (add to what step 1 already has; order matters
only within `Kernel → Add`: Lilu, AppleALC, NMHDA, then PG106, PG106FB):

```xml
<key>ACPI</key><dict><key>Add</key><array>
  <dict><key>Enabled</key><true/><key>Path</key><string>SSDT-PG106-HDAU.aml</string>
        <key>Comment</key><string>GP106 HDA function 01:00.1 visible from boot</string></dict>
</array></dict>

<key>DeviceProperties</key><dict><key>Add</key><dict>
  <!-- GTX 1060 Mobile -->
  <key>PciRoot(0x0)/Pci(0x1,0x0)/Pci(0x0,0x0)</key>
  <dict><key>hda-gfx</key><string>onboard-2</string></dict>
  <!-- its HD-audio function -->
  <key>PciRoot(0x0)/Pci(0x1,0x0)/Pci(0x0,0x1)</key>
  <dict><key>hda-gfx</key><string>onboard-2</string></dict>
</dict></dict>

<key>Kernel</key><dict><key>Add</key><array>
  <!-- Lilu.kext, AppleALC.kext (already there for the internal codec) -->
  <dict><key>BundlePath</key><string>NMHDA.kext</string><key>Enabled</key><true/>
        <key>ExecutablePath</key><string>Contents/MacOS/NMHDA</string>
        <key>PlistPath</key><string>Contents/Info.plist</string></dict>
  <!-- then PG106.kext, PG106FB.kext as in OWNER-RUNBOOK step 1 / 4e.0 -->
</array></dict>
```

- Use `onboard-2`, not `onboard-1`. WhateverGreen gives the Intel iGPU and
  its codec `onboard-1`; the two pairs must differ.
- A `layout-id` on 01:00.1 is needed only if AppleALC's device-id spoof
  reaches that function (ioreg shows `device-id` <70 9d 00 00> on it).
  NMHDA's HwFactoryPrefixTranslation needs the layout then; otherwise
  leave it out.
- Leave the internal codec's `layout-id` and WhateverGreen alone.

Checks on the Mac (OWNER-RUNBOOK 4e.0b): the `audio: AppleHDA pairing
published (codec 0 AFG 1 pin NID 7 …)` log line; `sysctl debug.nmhda`;
`ioreg -r -c PG106FB -l | grep -E 'hda-gfx|audio-codec-info'`; the monitor
listed under System Settings → Sound → Output.
