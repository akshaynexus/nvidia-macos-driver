# tools/macos: putting the GTX 1060 stack on the Helios 300 (macOS 26.7.2 25G332)

**Verified on macOS so far: only PG106's bring-up and its self-tests.** PG106
(OpenCore-injected) reaches `PG106Ready` Yes, and with `pg106_selftest=2` the CE
and GR self-tests return 0. PG106FB attached once with no monitor. Nothing else
has run on the Mac: sleep/wake, D3cold, an HDMI picture, PG106Accel, the Metal
plugin, the translator, NVK. Every step below is a first run.

| file | what |
| --- | --- |
| `install-gpu.sh` | `MODE=dry-run` (default) / `install` / `revert` / `status` for PG106FB, PG106Accel, the GPUBundles and the allow list |
| `gpu-apps.sh` | which apps get the GP106: `list`, `add`, `remove`, `deny`, `check`, `run` (the prime-run equivalent) |
| `nvmtl-allow.default.txt` | the allow list installed when none exists (system UI denied, no app allowed) |

Rules: one change per boot. Back up `config.plist` before an EFI change.
`-x` (Shift+Enter in the OpenCore picker) or `-pg106off` makes PG106, PG106FB
and PG106Accel stand down; `-pg106acceloff` stops PG106Accel alone and
`pg106fb=0` stops PG106FB alone.

## Install steps

Artifacts come from macos_helios300 `tools/release.sh` (the stage directory
holds `PG106FB.kext`, `PG106Accel.kext`, `NVMTLDriver.bundle`,
`libnvmtl_translate.dylib` and `NVK-Pascal/`). Copy that directory to the Mac,
e.g. `/tmp/gpu`, then:

```
MODE=dry-run SRC=/tmp/gpu bash tools/macos/install-gpu.sh      # checks + test kernel collection; changes nothing
MODE=install SRC=/tmp/gpu ONLY=PG106FB bash tools/macos/install-gpu.sh   # one part per boot
sudo reboot     # approve in System Settings > Privacy & Security if asked, reboot again
MODE=status bash tools/macos/install-gpu.sh
```

What install does, so it can be done by hand:

1. Preconditions: build 25G332; `PG106Ready` = Yes on the PG106 service; no
   NVRM/NVAccel kexts in /Library/Extensions (that stack owns the same
   NVMTLDriver.bundle path).
2. Backup of every target and of `/Library/KernelCollections/AuxiliaryKernelExtensions.kc`
   to `/Library/Helios300/gpu-backup/<time>` (`.absent` markers for what was
   not there).
3. Copy, owner `root:wheel`, no group/other write:
   - `PG106FB.kext`, `PG106Accel.kext` → `/Library/Extensions` (`chmod -R 755`).
     OpenCore cannot inject them: IOGraphicsFamily 600, IONDRVSupport 600 and
     IOAcceleratorFamily2 487.4.4 live only in SystemKernelExtensions.kc.
   - `NVMTLDriver.bundle` → `/Library/GPUBundles/NVMTLDriver.bundle`
     (PG106Accel's `MetalPluginName` is `../../../Library/GPUBundles/NVMTLDriver`).
   - `libnvmtl_translate.dylib` → `/Library/GPUBundles/nvmtl/` (755).
   - `libvulkan_nouveau.dylib` (755) and `nvk_icd.json` (644) →
     `/Library/GPUBundles/nvmtl/` (the plugin dlopens that path for 10de).
   - `nvmtl-allow.txt` → `/Library/GPUBundles/` (644), only if none exists.
4. Signing: everything ad-hoc (`codesign --force --sign -`) if it is not
   signed already. The release signs PG106FB, PG106Accel, the bundle and the
   two dylibs with rcodesign. kmutil and dyld accept ad-hoc signatures here
   (the plugin is loaded into every Metal process, so AMFI must accept ad-hoc
   code: AMFIPass is loaded on this machine, `-amfipassbeta`).
5. Auxiliary kernel collection: `kmutil create -n aux --volume-root /
   --allow-missing-kdk -B <Boot KC> -S <System KC> --repository /Library/Extensions
   -A <tmp> -z`, `kmutil inspect` it, then copy it over
   `AuxiliaryKernelExtensions.kc`. Nothing loads before the next boot. (The
   coordinator's earlier way, `kmutil load -p`, loads the kext at once once it
   is approved: avoid it on a running system.)
6. Approval: at the next boot macOS may report blocked system software
   (System Settings > Privacy & Security): allow it and reboot.

Apps: `sudo tools/macos/gpu-apps.sh add /Applications/Foo.app` (the rule is
the app's `CFBundleExecutable`, which is what `getprogname()` returns), `remove`
takes it away, `gpu-apps.sh run /Applications/Foo.app` starts it once on the
GP106 without a rule (`NVMTL_ENABLE=1`). The plugin reads the list when a
process makes its first Metal device. WindowServer is decided by the plugin
itself: it gets the GP106 only for the HDMI screen.

## Static readiness (run on Linux, 2026-11-08)

`pascal/scripts/kc-preflight.sh` against the owner's 25G332 Boot + System
collections (local copies, never committed):

- PG106 (boot KC): 385 imports, IOService/IOUserClient/IOPCIDevice sizes,
  2 vtables: PASS.
- PG106FB: 378 imports, IOFramebuffer 0x1d0, 350-slot vtable: PASS.
- PG106Accel: 828 imports, 14 class sizes (IOGraphicsAccelerator2 0xdd8 ...),
  14 vtables slot for slot against IOAcceleratorFamily2 487.4.4: PASS.
- PG106Accel's 67 macOS 26 forwarders (`pga_tahoe_fwd.h`, slots kc-audit
  skips because the kext defines them): each lands on its own method in the
  real base vtables (`kexts/PG106Accel/check_tahoe_fwd_kc.py`, run by
  kc-preflight): PASS. Their generation reads a non-25G332 IAF2 reference
  (cross-build.sh `PGA_IAF2`); the real-KC import audit and this check are
  what make the output safe for 25G332.

A pass means the loader will link them; it says nothing about behaviour.

## What will most likely break on the first runs, and how to see it

| # | Symptom | Where to look |
| --- | --- | --- |
| 1 | kexts not loaded after the reboot | `MODE=status` (`kmutil showloaded`), System Settings approval, `kmutil print-diagnostics -p /Library/Extensions/PG106FB.kext` |
| 2 | panic or hang at PG106Accel start (reconstructed IOAcceleratorFamily2 layouts) | boot with `-pg106acceloff`; `/Library/Logs/DiagnosticReports/*panic*`; `PG106AccelState` |
| 3 | `MetalPluginName` never appears | `PG106AccelState` ("waiting for PG106Ready" / "waiting for PG106FB"); `PG106Ready` on PG106 |
| 4 | no Metal device for the app | plugin log (below): `no rule in nvmtl-allow.txt matches` → `gpu-apps.sh add`; `NVMTLDevice +load` absent → the bundle did not load (signature, path) |
| 5 | **every shader fails to compile** | `library: in-memory bitcode reader unavailable` and `no AIR decoder fallback`: the release ships neither `NVMTLDriver.bundle/Contents/MacOS/air-runtime/libLLVM.dylib` nor `/Library/GPUBundles/nvmtl/air-opt`. **Known gap: an LLVM able to read AIR bitcode must be shipped in the bundle** (release work) |
| 6 | translator missing | `translate: dlopen(/Library/GPUBundles/nvmtl/libnvmtl_translate.dylib)` |
| 7 | NVK does not open PG106 | plugin log lines from nvmtl_vk; `PG106UserClient` in ioreg under PG106; `pg106_render_users` |
| 8 | render works, nothing on HDMI | `PG106AccelFramePath`, `PG106AccelStats` (drops), PG106FB checklist below |
| 9 | slow | expected: boot clocks only (no Pascal PMU firmware, no reclocking) |

Plugin log: `log show --last 10m --style compact --predicate 'eventMessage CONTAINS "NVMTL" OR eventMessage CONTAINS "NullMoth"'`.
Kext lines: IOLog for kexts in /Library/Extensions decode in `log show`;
PG106 is OpenCore-injected, so use its registry properties (or `pg106_printf=1`
and `sudo dmesg`).

## On-device checklists

### (a) Sleep / wake (PG106 alone first; sleep is disabled with pmset today)

1. Boot the PG106 build under test (OpenCore). `ioreg -r -c PG106 -d1`:
   `PG106Ready` Yes; with `pg106_selftest=2` both self-test results ret 0.
2. Allow sleep for this test only: if `pmset -g` shows `SleepDisabled 1`,
   `sudo pmset disablesleep 0` (put it back afterwards). Ethernet attached so
   SSH comes back. Boot with `pg106_selftest=2`: the self-tests then re-run
   inside every wake's bring-up and `PG106SelfTest` is rewritten.
3. `pmset sleepnow`. Wait 30 s. Wake with the power button or keyboard.
4. Pass: the Intel panel wakes; `PG106LastSleep` says "quiesced", 0 timed-out
   preempts; `PG106WakeResult` starts "ok: ready", `PG106WakeMs` a few
   seconds; `PG106Ready` Yes; `PG106Error` absent. `PG106LastWake` names the
   wake probe's verdict ("cold (unposted) GPU" expected: S3 cut the power).
5. `PG106SelfTest`: CE and GR ret 0 again. `pg106_probe`
   (`pascal/kext/tools`) shows `generation` +1 and `pm_state` 2 (idle) or 3
   (active).
6. Repeat sleep/wake 3 times. Then with PG106FB installed (DID_ON), then with
   PG106Accel (GPU_OFF/GPU_ON lines in `log show`, `dropped_gpu_off` in
   `PG106AccelStats`).
7. Fail paths to recognise: `PG106WakeResult` "FAILED ... power not lost: WPR
   still locked" (the GPU kept power and PG00 could not cycle it), "no power
   (BOOT_0 all ones)" (PG00._ON missing or ineffective; try
   `pg106_sleep_d3cold=1` next boot), a boot stage name (the bring-up failed:
   `PG106BootLog`). A black screen that never wakes: hard reset, boot
   `pg106_pm=0` (PG106 out of power management) and report.

### (b) HDMI (BLOCKERS B16 has the full order)

1. No monitor: `ONLY=PG106FB` install, reboot. `ioreg -r -c PG106FB -d1`:
   `PG106FBSelfCheck` = display engine up, HPD GPIO 27 active low, sense
   0x00005000 → nothing connected, EDID absent, 1 mode from the fallback,
   aperture BAR1, interrupts routed, power events registered;
   `PG106FBConnected` No. The internal panel and the Intel framebuffer are
   unchanged (`ioreg -r -c AppleIntelFramebuffer`).
2. Plug a 1080p60 HDMI monitor: `PG106FBConnected` Yes,
   `PG106FBModeCount` > 1, the EDID line in the log.
3. Arrange the display in System Settings; pick 1080p60: a picture. Logs:
   `setDisplayMode ... -> 0`, the `modeset #N` block.
4. Audio: `audio: on`, AppleHDA lists an HDMI output; play a sound.
5. A 4K60 / 1440p144 sink: `HDMI: ... SCDC written ... Scrambling_Status 01`.
6. Unplug / replug; monitor power-cycle (`hotplug: SCDC re-arm`).
7. Sleep/wake with the monitor on: the mode comes back after `DID_ON`.

### (c) Accelerator, Metal, NVK

1. `ONLY=PG106Accel` install, reboot. `ioreg -r -c PG106Accel -d1`:
   `PG106AccelState`, `MetalPluginName` present after PG106Ready,
   `PG106AccelVRAMBytes` 6 GiB, `PG106AccelDisplay` Yes once paired with
   PG106FB.
2. `ONLY="plugin xlate nvk allow"` install (no reboot needed for userspace,
   but one change per boot still holds).
3. `system_profiler SPDisplaysDataType` lists a second GPU; `mtlprobe` (or
   `pascal/tests-mac/offload_app`, `pg106_offload`) creates a device on it.
4. Add one Metal app with `gpu-apps.sh add`, launch it; the plugin log shows
   `allow-listed`, the translator and NVK lines; a frame renders.
5. HDMI composited by the GP106: `PG106AccelFramePath` (CE ring expected),
   `PG106AccelStats` flips increasing, few drops.
