# OWNER RUNBOOK — Pascal (GP106M) bring-up on the Helios 300, macOS

Phase 6 deliverable (`HANDOFF-PASCAL.md` §6, gate: "the complete owner runbook:
what to build, install, and measure on the Mac — written for a careful human,
with rollback for every step"). Design rationale: `research/evidence/OFFLOAD-DESIGN.md`.

Target machine: Acer Predator Helios 300 (2017–2018) with GTX 1060 Mobile
(GP106M, PCI `10DE:1C20` or `10DE:1C60`), macOS booted via OpenCore with working
Intel HD 630 acceleration (`HANDOFF-PASCAL.md` §1; wiring facts in
`HELIOS300-PASCAL-RESEARCH.md` §2).

**What exists today.** Steps 0–2 can be run. The kext (`pascal/kext`) now
**does bring-up at load**: probe gate, BAR0/BAR1, BOOT_0 identity, VBIOS from
ACPI `_ROM`, firmware from its bundle, then POST (if the GPU is unposted),
PTOP, and the ACR secure boot — which currently stops, by design, at the first
step that needs the not-yet-ported ACR VMM (the HS signature patch). By then
it has written the WPR shadow into VRAM and reset + enabled the SEC2 falcon
the way nouveau does, but has loaded and started no falcon code
(`pascal/kext/README.md` "What is real, what is stubbed"). It has a
DEVICE user client (root, diagnostics) and a RENDER user client (the bridge).
The probe tool is `pg106_probe`. Steps 3 and 4 now have their programs: an NVK
build with the Pascal backend (`nvk/pascal`), the smoke test and the offload
window app (`pascal/tests-mac`). None of them has run on a Mac yet; they were
built and tested on Linux only (`pascal/tests-mac/README.md`). Step 3 also
needs a kext that finishes GPU bring-up. Step 4's Metal variant (4c) also
needs the accelerator kext PG106Accel (`kexts/PG106Accel`), which is written
but has only been cross-built and audited on Linux; 4a/4b do not need it.

## What this runbook changes on the machine — be clear about it

Loading a third-party kext on macOS 11 and later is **not** a "nothing
persistent" operation, whichever way it is done:

- **OpenCore injection (recommended here, step 1A).** PG106 links only IOKit,
  libkern and IOPCIFamily KPIs (`pascal/kext/Info.plist` `OSBundleLibraries`),
  so OpenCore can inject it into the boot kernel collection like
  Lilu/WhateverGreen. The persistent change is one `Kernel → Add` entry and
  the kext file in your EFI partition's `OC/Kexts`. Rollback is removing (or
  disabling) that entry. Nothing in macOS's own kernel collections changes.
- **`kmutil load` (step 1B, only if you already load unsigned kexts this
  way).** On macOS 11+ `kmutil load -p` on a kext outside the system
  collections *stages* it (a copy under `/Library/StagedExtensions`), may
  require approval in System Settings → Privacy & Security, and approval
  leads macOS to rebuild the **Auxiliary Kernel Collection** and ask for a
  reboot before the kext loads. An ad-hoc signed kext (what `build-macos.sh`
  produces) is only accepted if SIP already allows untrusted kexts on this
  install. Check `man kmutil` on your macOS version for the exact behaviour;
  the rollback below covers staging and the AuxKC.

Either way the dGPU stays powered once the kext claims it, unless you enable
ACPI D3cold with the boot-arg `pg106_d3cold=1` (step 1, "Boot-args"). That
path (`PG00._OFF`/`_ON` on PEG0, HELIOS300-HW-PROBE.md "Power") has never run
under macOS; leave it off until steps 1–2 pass.

## Hard rules — read before anything else

- **Back up the EFI partition first** (step 0) and have a bootable USB stick
  with a known-good OpenCore EFI. On OpenCore, Command-R at power-on does
  nothing; recovery goes through the OpenCore picker or that USB stick
  (§"If the Mac will not boot").
- **AC power, nothing battery-critical.** Once the kext claims the card the
  dGPU stays at VBIOS boot clocks until reboot.
- **One step at a time; measure before advancing.** If a step's expected
  result does not appear, stop, roll back, record the output.
- **Capture logs BEFORE any reboot** (§"Capture logs before rebooting").
- **Stop conditions:**
  - a kernel panic at any step — capture logs, roll back, report;
  - the load asks you to change SIP/AMFI/Secure Boot settings you do not
    already have — record and stop;
  - `ioreg` shows PG106 on any device other than your `10DE:1C20`/`1C60` —
    roll back immediately (the probe gate should make this impossible; seeing
    it is a bug report).

## Prerequisites

- Xcode or the Command Line Tools (`xcode-select --install`), `git`, `python3`.
- The repo checked out (branch `pascal-gp106`) with submodules.
- Free disk space: ~2 GB.
- If the Turing+ driver is installed, record its state; it does not match
  Pascal, but the cleanest baseline is with it absent or `-nvoff`.

**Rollback:** nothing was changed.

## Step 0 — Baseline, EFI backup, and "is the dGPU even visible?"

```sh
mkdir -p ~/pascal-test && cd ~/pascal-test
git clone --recurse-submodules -b pascal-gp106 <this repo> repo
cd repo && python3 helios300-gp106-simtest.py      # must pass

sysctl kern.bootargs                         > ~/pascal-test/baseline.txt
nvram boot-args                             >> ~/pascal-test/baseline.txt 2>&1
csrutil status                              >> ~/pascal-test/baseline.txt
ioreg -r -c IOPCIDevice -l -w0 | grep -E '"(vendor-id|device-id|class-code|name|IOName)"' >> ~/pascal-test/baseline.txt
system_profiler SPDisplaysDataType          >> ~/pascal-test/baseline.txt 2>&1
ps -p "$(pgrep -x WindowServer)" -o pid,etime >> ~/pascal-test/baseline.txt
```

Back up the EFI partition (identify it with `diskutil list`; usually
`disk0s1`):

```sh
sudo diskutil mount disk0s1
cp -R /Volumes/EFI/EFI ~/pascal-test/EFI-backup    # also copy it to the USB stick
```

**Check that the dGPU has not been disabled** — common on Hackintosh laptops,
and it makes every later step fail in a confusing way:

```sh
sysctl -n kern.bootargs | tr ' ' '\n' | grep -x -- '-wegnoegpu'     # WhateverGreen: hides all dGPUs
ls /Volumes/EFI/EFI/OC/ACPI | grep -i -E 'dgpu|gpu.*off|nvidia'      # SSDT that calls the dGPU _OFF
plutil -p /Volumes/EFI/EFI/OC/config.plist | grep -i -E 'disable-gpu|disable-external-gpu|dgpu|#display'
```

- **Measure:** the dGPU is listed in `ioreg` with vendor-id `de 10 00 00`,
  device-id `20 1c 00 00` (or `60 1c ...`), and a display class-code
  (`00 00 03 00` VGA or `00 02 03 00` 3D). None of the three checks above
  prints anything.
- **If the dGPU is disabled** (`-wegnoegpu`, an `_OFF` SSDT, or a disable
  property): PG106 cannot work on this config — with the device powered off
  through ACPI, BAR0 reads back `0xffffffff` and `start()` refuses it; with
  `-wegnoegpu` the device is hidden from matching. Re-enabling the dGPU is an
  OpenCore change with its own risks (higher idle power, a display-less dGPU
  macOS has no driver for); make it in a **copy** of `config.plist`, boot
  that copy once from the USB stick first, and record which mechanism you
  removed. If you are not prepared to do that, stop here.
- **Rollback:** `rm -rf ~/pascal-test` (keep `EFI-backup` on the USB stick).

## Step 1 — Build, then load the kext

```sh
cd ~/pascal-test/repo/pascal/kext
bash scripts/build-macos.sh 2>&1 | tee ~/pascal-test/build.log
# → build/xcodebuild/Release/PG106.kext and build/pg106_probe
ls build/xcodebuild/Release/PG106.kext/Contents/Resources/   # 14 gp106-*.bin
```

The build stages the 14 GP106 firmware files into the bundle's `Resources`
(`scripts/stage-firmware.sh`, NVIDIA licence in `licenses/`). A file missing
from `research/firmware/gp106/` becomes a zero-length placeholder and the
kext logs it as missing; `stage-firmware: 14 staged, 0 missing` is the
expected build line.

**Boot-args the kext reads** (all optional; put them in a *copy* of
`config.plist`, `NVRAM → Add → 7C436110-… → boot-args`):

| boot-arg | effect | default |
| --- | --- | --- |
| `pg106_stop_after=N` | stop bring-up after devboot stage N: 0 identify, 1 posted check (no GPU writes at all), 2 POST, 3 PTOP, 4 ACR | run everything |
| `pg106_d3cold=1` | power the dGPU off (ACPI `PG00._OFF`) when no render session is open for `pg106_idle_ms`; next session powers it on and re-runs the bring-up | off |
| `pg106_idle_ms=N` | idle time before D3cold | 5000 (nouveau's autosuspend delay) |
| `pg106_render_users=0` | restrict render connections to root (steps 3–4 then need `sudo`) | open to all local users |

**First load: use safe mode.** Load once with `pg106_stop_after=1`: the kext
identifies the GPU, reads the VBIOS through ACPI and the posted bit, and
writes **no GPU register**. Only after that log looks right, remove the
boot-arg for the full bring-up.

`build-macos.sh` fetches MacKernelSDK (pinned commit) into `repo/MacKernelSDK`
and runs the same SDK syntax check as the Linux CI before `xcodebuild`. If the
build fails, stop and send `build.log`: the Xcode project is hand-written and
has never been opened by Xcode (`pascal/kext/README.md`).

Kexts must be owned by `root:wheel` with no group/other write bits, or macOS
refuses to load them:

```sh
K=~/pascal-test/PG106.kext
sudo rm -rf "$K" && sudo cp -R build/xcodebuild/Release/PG106.kext "$K"
sudo chown -R root:wheel "$K" && sudo chmod -R u=rwX,go=rX "$K"
kmutil print-diagnostics -p "$K" 2>&1 | tee ~/pascal-test/diag.txt   # dependency/permission problems show here
```

### 1A — OpenCore injection (recommended)

1. Copy the kext into the EFI: `sudo cp -R "$K" /Volumes/EFI/EFI/OC/Kexts/`.
2. Edit a **copy** of `config.plist` (keep the original): under
   `Kernel → Add` append an entry `BundlePath = PG106.kext`,
   `ExecutablePath = Contents/MacOS/PG106`, `PlistPath = Contents/Info.plist`,
   `Enabled = true`, `Arch = x86_64`, `MinKernel`/`MaxKernel` empty. It has no
   dependency on Lilu, so its position in the list does not matter.
3. Validate with OpenCore's `ocvalidate` for your OpenCore version, then make
   the copy the active `config.plist` and reboot.

**Rollback 1A:** set that entry's `Enabled` to `false` (or restore the backed
up `config.plist`) and reboot. If macOS does not boot, see §"If the Mac will
not boot".

### 1B — `kmutil load` (only if this install already loads unsigned kexts)

```sh
sudo kmutil load -p "$K" 2>&1 | tee ~/pascal-test/load.log
```

If macOS asks for approval in Privacy & Security, approving it is the
documented flow; it will then rebuild the AuxKC and ask to restart. If it
instead demands a SIP/security change, stop (hard rule).

**Rollback 1B:**

```sh
sudo kmutil unload -b org.helios300.pg106          # if it is loaded
sudo kmutil clear-staging                          # drops the staged copy
kmutil showloaded --list-only | grep -i pg106      # must print nothing
```

If approval already made it part of the AuxKC, also remove any copy you put
in `/Library/Extensions` and rebuild the AuxKC without it (macOS: `sudo
kmutil install --update-all`, or reboot and let macOS rebuild it; check `man
kmutil` on your version), then reboot.

### Check (both paths)

```sh
kmutil showloaded --list-only | grep -i pg106
ioreg -r -c PG106 -l -w0                                       > ~/pascal-test/step1-ioreg.txt
log show --last 10m --predicate 'eventMessage CONTAINS "PG106"' > ~/pascal-test/step1.log
```

- **Measure** — expected log lines, in order (`a=`/`b=` values vary):
  - `PG106: probe 10de:1c20 accepted (GP106M [GeForce GTX 1060 Mobile])` (or
    `1c60`)
  - `PG106: policy: d3cold=0 idle_ms=5000 render_users=1 stop_after=…`
  - `PG106: GP106 identified: BOOT_0 136000a1 (arch 0x136 rev 0xa1), BAR0 16 MiB, BAR1 256 MiB`
  - `PG106: acpi: companion PEGP, parent PEG0; _ROM present, PG00._OFF present`
    (if it says `no ACPI companion`, record the ioreg properties of the dGPU:
    BLOCKERS B12)
  - `PG106: vbios: ACPI _ROM: ret 0 (ok), 168960 bytes, 4 images, 42 reads, PCIR 10de:1c20, score 16`
    and `PG106: vbios: using ACPI _ROM, 168960 bytes` (168960 = 0x29400, the
    Helios ROM; a different size or PCIR id is a finding — send the log)
  - `PG106: fw: 14 resource requests queued`, then one `PG106: fw: gp106-… N bytes`
    line per file, then `PG106: fw: 14 present, 0 missing; starting bring-up`.
    The firmware arrives only once kextd/kernelmanagerd runs; with OpenCore
    injection that is some seconds after boot (B13).
  - `PG106: pm: unbooted -> boot`, then `PG106: boot identify ok …`,
    `boot posted ok …` (`b=1` means POST is needed; the Helios firmware
    normally POSTs the dGPU, so expect `b=0` unless it was powered off)
  - safe mode ends with `PG106: boot: safe mode, stopped after posted`.
  - full mode continues: `boot post skip` (or `boot post ok` after a POST),
    `fb: 6144 MiB VRAM; ACR carve-out at 0x…`, `boot top ok … SEC2 from PTOP`
    (`a=0x87000`), `boot acr begin`, `boot acr info … ACR plan built`, then
    **`PG106: boot acr: HS image space needs the ACR VMM (not ported):
    stopping at the signature patch; SEC2 reset+enabled, no falcon code
    loaded or started`** and `boot acr FAIL ret=-78` with the step's nouveau
    citation (`nvkm/falcon/fw.c:45-61 nvkm_falcon_fw_patch`). **That is the
    expected end of this round**: the WPR shadow was written to the VRAM
    carve-out through PRAMIN, SEC2 went through nouveau's reset/enable
    sequence (engine reset 0x3c0, scrub wait, PMC_ENABLE bit 8, 0x084), and
    no falcon code was loaded or started. If the scrub poll times out
    instead (`boot acr FAIL ret=-60` citing `gm200.c:145-149`), record it:
    that is B4 evidence. `pm: FAILED — render sessions are refused`
    follows.
  - `ioreg -r -c PG106` shows exactly one PG106 node, under your dGPU.
  - No framebuffer/accelerator appeared; `system_profiler SPDisplaysDataType`
    matches the baseline; WindowServer PID unchanged.
- **Diagnostic log lines:** `BOOT_0 read back ffffffff: BAR0 not decoding` =
  the dGPU is powered off (step 0 check); `probe ... rejected` = not a GP106
  mobile id; `start: identification failed` = BOOT_0 refused (send the log);
  `boot posted FAIL ret=-5 … PRI error` = the GPU answers with PRI errors;
  `boot post FAIL ret=-60` = the PMU DEVINIT did not finish in 2 s (record
  it: this is the first hardware POST, B5); `boot acr FAIL ret=-19 … SEC2
  HWCFG unreadable` = SEC2 is gated (B4); `fw: request … failed` = the bundle
  has no Resources (rebuild with `build-macos.sh`).
- **Rollback:** as for 1A/1B. The bring-up writes only volatile GPU state
  (registers, VRAM); a reboot (or D3cold) clears it.

## Step 2 — User client probe

```sh
sudo ~/pascal-test/repo/pascal/kext/build/pg106_probe 2>&1 | tee ~/pascal-test/step2.log
time sudo ~/pascal-test/repo/pascal/kext/build/pg106_probe >/dev/null
```

The client is root-only by design; without `sudo` the open fails.

- **Measure:** `abi 0.3`, `boot0 0x136000a1` (the same word the kernel log
  printed), `flags gp10x gp106`, `result GP106 identified`, exit status 0,
  then the status block: `boot stage 4 ret -78` (ACR, the expected stop of
  this round; `stage 1 ret 0` in safe mode), `vbios 168960 bytes from ACPI
  _ROM`, `firmware present 0x3fff missing 0x0000`, `memory VRAM 6144 MiB
  BAR1 256 MiB`, `flags acpi`, `pm_state 8` (failed: the expected end of
  this round; `pm_state 2`, idle, in safe mode).
  Record the `time` output as a baseline; no target number is claimed.
- **Rollback:** none — the tool is a process; its connection closes when it
  exits.

## Step 3 — NVK Vulkan smoke

What runs: NVK (Mesa) with the Pascal backend `nvk/pascal`, which talks to
PG106 through the bridge's IOKit transport (render user client, type 1).
`pg106_vk_smoke` loads it directly from your build directory. Nothing is
installed in `/Library`, and the Turing+ driver's files are not touched.
Source and Linux-only evidence: `nvk/pascal/README.md`,
`research/evidence/BRIDGE-NVK-REVIEW.md`, `pascal/tests-mac/README.md`.

**Before you start**
- **The GPU must be brought up.** The kext you loaded in step 1 must
  report the GP106 ready (bring-up finished, kext GET_STATUS `READY`). If
  it does not, NVK prints `pg106: the kext has not brought the GPU up` and
  finds no device. Stop there; this step cannot pass until it does.
  *Status 2026-11-07:* bring-up currently stops at the ACR HS signature patch
  and the kext has no hardware channel yet (step 1), so this message **is
  the expected result this round**. 3a (the builds) can still be done and
  checked.
- **No `sudo` needed.** Render connections are open to every local user
  (owner decision 2026-11-07; the trade-off: any local process can reach an
  unreviewed kernel parser and wake the dGPU). The step 3–4 programs run as
  you. If you set `pg106_render_users=0` (step 1 boot-args) to restrict it,
  run them with `sudo` instead.
- **One client at a time.** The v0 kext has a single GPU channel. While one
  program holds it, a second gets
  `pg106: the GP106 is in use by another process`. Run the steps one after
  another.

**3a — build NVK with the Pascal backend** (about 15–30 min; needs the tools
listed in `nvk/pascal/scripts/build-nvk-macos.sh`: Rust, meson/ninja, bindgen,
cbindgen, glslang, mesa_clc):

```sh
cd ~/pascal-test/repo
sh nvk/pascal/scripts/build-nvk-macos.sh ~/pascal-test/nvk 2>&1 | tee ~/pascal-test/step3-build.log
# -> built ~/pascal-test/nvk/out/libvulkan_nouveau.dylib (<sha256 prefix>)
MESA_TREE=~/pascal-test/nvk/mesa sh pascal/tests-mac/build-macos.sh 2>&1 | tee -a ~/pascal-test/step3-build.log
# -> built …/pascal/tests-mac/build/pg106_vk_smoke …/offload_selftest …/pg106_offload
```

(`MESA_TREE` only supplies the Vulkan headers.) If either build fails, stop
and send `step3-build.log`. The NVK build has never been run on macOS with this patch.

**3b — run the smoke test**

```sh
NVK=~/pascal-test/nvk/out/libvulkan_nouveau.dylib
T=~/pascal-test/repo/pascal/tests-mac/build
pgrep -x WindowServer > ~/pascal-test/step3-ws.txt
$T/pg106_vk_smoke --driver $NVK 2>&1 | tee ~/pascal-test/step3.log ; echo "exit $?" >> ~/pascal-test/step3.log
pgrep -x WindowServer >> ~/pascal-test/step3-ws.txt
log show --last 5m --predicate 'eventMessage CONTAINS "PG106"' > ~/pascal-test/step3-kernel.log
```

- **Measure (expected output):**
  ```
  driver: …/libvulkan_nouveau.dylib (ICD, no loader)
  devices: 1
    [0] 10de:1c20 NVIDIA GeForce GTX 1060 Mobile, Vulkan 1.3.x, driver NVK (Mesa …)  <- GP106
    heap 0: 6144 MiB device-local                       (plus a host heap)
  PASS fill     1 MiB fill in VRAM + copy to host: 0 bad words, first 0xa5c3e10f, … ms
  PASS compute  4096 x (3i+7): 0 bad, v[0]=7 v[4095]=12292, … ms
  PASS repeat   200 dispatches: 0 with wrong data, 0 timeouts, mean … ms, worst … ms
  PASS clear    256x256 RGBA8 clear + copy: 0 bad pixels, first 51,102,153,255, … ms
  pg106_vk_smoke: 4/4 passed
  exit 0
  ```
  The two WindowServer PIDs in `step3-ws.txt` must be the same, and
  `system_profiler SPDisplaysDataType` must still match the baseline. The
  timings are a baseline; no target is claimed. The GPU runs at VBIOS boot
  clocks (no PMU firmware).
- **What a failure means:**
  - `pg106: the PG106 user client refused this process (-EACCES)`:
    `pg106_render_users=0` is set; run with `sudo` or remove it.
  - `FAIL no GP106 Vulkan device` with no `pg106:` line: the kext is not
    loaded (step 1).
  - `has not brought the GPU up`: kext bring-up; send `step3-kernel.log`.
  - `error: fence wait = … (10 s bound)`, `VK_ERROR_DEVICE_LOST` or
    `did not complete within NVK_PG106_TIMEOUT_MS`: the GPU did not finish
    the work. Capture the logs (§"Capture logs BEFORE any reboot") and stop.
  - A `FAIL` line with bad words/pixels: wrong results. Send `step3.log`.
  - A hang that the 10 s bounds do not end: Ctrl-C, then capture logs. If
    the machine is unresponsive, see the stop conditions.
- **Rollback:** none. The build lives in `~/pascal-test/nvk` (delete it to
  reclaim ~2 GB), and the GPU connection closes when the program exits.

## Step 4 — IOSurface render, Intel composites

The offload path of `research/evidence/OFFLOAD-DESIGN.md` §1–2: the GP106
renders into IOSurfaces in system memory, and WindowServer composites them on
the Intel HD 630 onto the internal panel. The GP106 drives no display.
Prerequisite: step 3 passed. Run as yourself (no `sudo`).

**4a — the frame path without a window**

```sh
$T/offload_selftest --driver $NVK 2>&1 | tee ~/pascal-test/step4a.log
```

- **Measure:**
  - `PASS zero-copy (host import): 60 frames over 2 targets 512x384 pitch 2112, 0 bad, …`
    shows the GP106 wrote straight into host pages the kext wired
    (BO_IMPORT_HOST).
  - `PASS copy (staging buffer + memcpy): …` is the fallback path.
  - `offload_selftest: all passed`.
  - If the first line says `copy` instead of `zero-copy`, host import was
    refused. That is not a failure of the offload path, but record it: it
    answers OFFLOAD-DESIGN §6 open question 1.
- **Rollback:** none.

**4b — a window on the internal panel**

```sh
pgrep -x WindowServer > ~/pascal-test/step4-ws.txt
$T/pg106_offload --vulkan --driver $NVK --seconds 20 2>&1 | tee ~/pascal-test/step4b.log
$T/pg106_offload --vulkan --driver $NVK --seconds 20 --copy 2>&1 | tee -a ~/pascal-test/step4b.log
pgrep -x WindowServer >> ~/pascal-test/step4-ws.txt
system_profiler SPDisplaysDataType > ~/pascal-test/step4-displays.txt
```

- **Measure:**
  - **On the panel:** a 320×200-point window titled
    `pg106_offload (NVK on GP106)`. It shows a blue/green gradient whose
    red channel changes every frame and a white bar moving left to right,
    for 20 s per run. There must be no garbage, no black frames, and no
    stuck or torn bar. Note anything you see.
  - **stdout, per run:**
    - `vulkan device: NVIDIA GeForce GTX 1060 Mobile, mode: zero-copy (host import)`
      (`copy (staging…)` for the `--copy` run);
    - `frames N, wrong 0, gpu errors 0, seed bumped on N of N unlocks`,
      with N ≈ 1200;
    - a timing line (baseline only);
    - `pg106_offload: PASS`.
  - **System state:**
    - the WindowServer PID is unchanged;
    - `step4-displays.txt` lists the same displays as the baseline (no new
      display, the internal panel still on the Intel GPU);
    - `ioreg -r -c PG106 -l` shows the same single node.
- **Rollback:** none. The window closes and the program exits after
  `--seconds`.

**4c — the same through Metal (needs PG106Accel)**

Metal offers a device only for an accelerator that publishes
`MetalPluginName`. For the GP106 that accelerator is PG106Accel
(OFFLOAD-DESIGN §3.3). It is render-only: it claims no display, and the
panel stays on the Intel GPU. It does no GPU work itself. The plugin still
renders through NVK and PG106's render client, exactly as in 4b.

*Prerequisites:*
- 4b passed, with PG106 loaded (step 1).
- The Metal plugin `/Library/GPUBundles/NVMTLDriver.bundle` and step 3's NVK
  under `/Library/GPUBundles/nvmtl/` (`libvulkan.dylib`,
  `libvulkan_nouveau.dylib`, `nvk_icd.json`). These are the paths the
  plugin loads from (`plugin/nvmtl_vk.c`).
- **Open item:** no Pascal-only installer for those two exists yet. Do not
  run the package's `install.sh` on this machine: it installs the Turing+
  kexts (NVRM, NVAccel, NVRMFB, NVRMAGDC) as well.

*Build and load PG106Accel.*
- Load order: PG106 first, then PG106Accel.
- PG106Accel cannot be injected by OpenCore. It links IOAcceleratorFamily2,
  which exists only in macOS's own kernel collections, so it has to load
  from the Auxiliary Kernel Collection, the 1B mechanism. The same
  approval/SIP hard rule applies: if `kmutil` demands a security change,
  stop.
- After the reboot that approval asks for, the boot order does not matter.
  PG106Accel offers Metal nothing until PG106 has published *and* reports
  `PG106Ready = true`, i.e. bring-up has finished.
- **Expected this round:** bring-up stops at the ACR HS step (step 1), so
  `PG106Ready` stays false. PG106Accel then stays at
  `PG106AccelState = "waiting for PG106Ready"`, and `--list-metal` shows only
  the Intel GPU. That is the correct result until PG106 reaches READY. The
  kext checks are still worth running; the Metal checks are not until then.

```sh
kmutil showloaded --list-only | grep -i pg106            # PG106 must already be loaded
cd ~/pascal-test/repo
bash kexts/PG106Accel/build.sh ~/pascal-test/accel 2>&1 | tee ~/pascal-test/accel-build.log
A=/Library/Extensions/PG106Accel.kext
sudo rm -rf "$A" && sudo cp -R ~/pascal-test/accel/PG106Accel.kext "$A"
sudo chown -R root:wheel "$A" && sudo chmod -R u=rwX,go=rX "$A"
kmutil print-diagnostics -p "$A" 2>&1 | tee ~/pascal-test/accel-diag.txt
sudo kmutil load -p "$A" 2>&1 | tee ~/pascal-test/accel-load.log   # may ask for approval + restart
```

*Check after it loaded* (after the restart, if one was asked for):

```sh
kmutil showloaded --list-only | grep -i -e org.helios300.pg106 -e PG106Accel
ioreg -r -c PG106Accel -l -w0 > ~/pascal-test/step4c-ioreg.txt
grep -E '"(MetalPluginName|PG106AccelState|IOMatchCategory)"' ~/pascal-test/step4c-ioreg.txt
ioreg -r -c PG106 -l -w0 | grep '"PG106Ready"' 
log show --last 10m --predicate 'eventMessage CONTAINS "PG106Accel"' > ~/pascal-test/step4c-kext.log
system_profiler SPDisplaysDataType > ~/pascal-test/step4c-displays.txt
```

- **Measure (kext):**
  - **ioreg.** One `PG106Accel` node, under the same `IOPCIDevice` as the
    PG106 node, with `IOMatchCategory = IOAccelerator`.
    - With PG106's `PG106Ready = Yes`: `PG106AccelState = "Metal offered"`
      and a `MetalPluginName`.
    - With `PG106Ready = No` (this round): `PG106AccelState = "waiting for
      PG106Ready"` and no `MetalPluginName`.
  - **Log.**
    - `PG106Accel: up on … 10de:1c20, headless; …`
    - either `PG106Accel: PG106 ready on …: MetalPluginName set, … (render
      users: everyone)`, or `PG106Accel: PG106 published on …, bring-up not
      finished (PG106Ready false): Metal is not offered yet, …`
    - **Not expected:** `PG106Accel: framebuffer … refused`. That line means
      the family found a framebuffer on the dGPU. PG106Accel refused it, but
      record it.
  - **Displays.** `step4c-displays.txt` matches the baseline. The internal
    panel is still on the Intel GPU, and no new display appeared.

```sh
rm -f /tmp/nvmtl.log
$T/pg106_offload --list-metal 2>&1 | tee ~/pascal-test/step4c.log
$T/pg106_offload --metal --seconds 20 2>&1 | tee -a ~/pascal-test/step4c.log
cp /tmp/nvmtl.log ~/pascal-test/step4c-plugin.log
```

- **Measure (Metal):**
  - **`--list-metal`.** It shows the Intel GPU and the GP106 (its name
    contains `NVMTL over`) with `headless 1`. Record which device is listed
    first and both `lowPower` values (OFFLOAD-DESIGN §6 Q7).
  - **`--metal`.** It prints `metal device: …`, shows a red/yellow triangle
    on an animated background, and ends with `pg106_offload: PASS`.
  - **Plugin log** (`step4c-plugin.log`). It should contain
    `MTLIOAccelDevice's initWithAcceleratorPort: -> …`,
    `Pascal GP106: the display belongs to the Intel GPU`, and
    `hw resource pools: installed 3`.
  - **If a line is missing or the run fails.** Send `step4c-plugin.log`, and
    the `PG106Accel:` lines `video memory refused` / `not supported on the
    GP106`. They show which family paths Metal used on an accelerator that
    has no VRAM (§6 Q10).
  - **A non-allow-listed app** (any Metal app not in `nvmtl-allow.txt`) still
    gets only the Intel GPU.
- **Rollback 4c:**

  ```sh
  sudo kmutil unload -b com.nullmoth.PG106Accel      # if it is loaded
  sudo rm -rf /Library/Extensions/PG106Accel.kext
  sudo kmutil clear-staging
  ```

  Then rebuild the AuxKC without it, as in Rollback 1B, and reboot. For a
  quick off switch without uninstalling, the boot-arg `-pg106acceloff`
  stops it attaching; put it in a *copy* of `config.plist`, as in step 1.
  If the Mac does not boot after the AuxKC rebuild, Safe Mode skips the
  AuxKC (§"If the Mac will not boot", item 2). Unloading PG106 alone is
  safe: PG106Accel withdraws `MetalPluginName` when PG106 goes away.

**4d — power, best-effort**

> **ACPI D3cold note** (`HELIOS300-HW-PROBE.md`, OFFLOAD-DESIGN §3.1):
> this machine's ACPI has a power resource `PG00` (`_ON`/`_OFF`) referenced
> by `_PR0`/`_PR3` on PEG0. So the dGPU *can* be powered off completely
> (D3cold) without NVIDIA PMU firmware, through the platform. Reclocking
> still cannot happen. Power therefore depends on which state the dGPU is
> in, so record that state with every measurement:
>
> - `ioreg -r -c IOPCIDevice -n PEGP -l -w0 | grep -E 'IOPowerManagement|CurrentPowerState|pcidebug'`
>   for the dGPU (use the name step 0's `ioreg` showed for `10de:1c20` if it
>   is not `PEGP`), and the kext's GET_STATUS block (`sudo pg106_probe`
>   prints `pm_state` — 2 idle, 3 active, 6 off — and `power_cycles`);
> - if the kext has powered the GPU off while idle, the first step-3 run
>   after that pays a full re-POST + ACR/GR boot. Time it
>   (`time $T/pg106_vk_smoke --driver $NVK`) and record that time as
>   "first session after D3cold".

Measure each state for at least 2 minutes with the machine otherwise idle
(lid open, panel at a fixed brightness, Wi-Fi as it was for the baseline):

| state | how |
| --- | --- |
| A — kext not loaded | before step 1, or after the 1A/1B rollback |
| B — kext loaded, no render client | after step 2, nothing running |
| B′ — kext loaded, idle long enough for D3cold | only if the kext implements the idle power-off; same as B after its idle timeout |
| C — rendering | during `pg106_offload --vulkan --seconds 120` |

```sh
sudo powermetrics --samplers cpu_power,thermal -i 5000 -n 24 > ~/pascal-test/power-<state>.txt
```

On an Intel Mac `powermetrics` reports CPU package power, not the NVIDIA
GPU's. The dGPU's share shows up only in total system power, so the
comparison that matters is a **wall-socket power meter** on AC (record
watts for A/B/B′/C). Without a meter, the alternative is battery discharge,
and that is the **only** permitted exception to the AC rule:
- start at ≥ 80 %, at least 10 min per state;
- run `ioreg -rn AppleSmartBattery | grep -E '"(InstantAmperage|Voltage)"'`
  every minute;
- reconnect AC as soon as the charge drops below 50 % or anything looks
  wrong.

- **Measure:** watts per state, the dGPU power state per state, and the
  first-session-after-D3cold time if B′ applies. No target exists. These
  numbers answer OFFLOAD-DESIGN §6 open question 3.
- **Rollback:** none (measurement only).

## Final teardown

```sh
# Proof first: these read files inside ~/pascal-test, so run them before deleting it.
diff <(sysctl kern.bootargs) <(grep '^kern.bootargs' ~/pascal-test/baseline.txt) && echo "boot-args unchanged"
diff <(csrutil status) <(grep -i 'integrity protection' ~/pascal-test/baseline.txt) && echo "SIP unchanged"
```

Then, if you did 4c, apply Rollback 4c first (PG106Accel), then undo PG106's
load path (1A: disable/remove the `Kernel → Add` entry and delete
`EFI/OC/Kexts/PG106.kext`; 1B: the 1B rollback), reboot, and confirm
`kmutil showloaded --list-only | grep -i pg106` prints nothing (that also
covers PG106Accel). Only
then, if you want, `sudo rm -rf ~/pascal-test` (keep the EFI backup).

## Capture logs BEFORE any reboot

```sh
d=~/pascal-test/logs-$(date +%Y%m%d-%H%M%S); mkdir -p "$d" && cd "$d"
sudo dmesg                                                              > dmesg.txt
log show --last 30m --predicate 'eventMessage CONTAINS "PG106" OR process == "kernel"' > logshow.txt
ioreg -l -w0                                                            > ioreg.txt
ls -la /Library/Logs/DiagnosticReports/ | tail -20                      > panic-list.txt
cp /Library/Logs/DiagnosticReports/*.panic . 2>/dev/null
sudo sysdiagnose -f . -u                                                # large; only if the box is healthy enough
```

After a panic and reboot, the previous panic report is in
`/Library/Logs/DiagnosticReports/` (`*.panic`); copy it before doing anything
else.

## If the Mac will not boot

OpenCore machines do not get macOS Recovery from Command-R; everything goes
through the OpenCore picker or the USB stick.

1. **Kext suspected (path 1A).** Boot the USB stick with the known-good EFI
   (firmware boot menu: F12 on the Helios 300 if enabled in the BIOS setup).
   From there either boot macOS directly, or mount the internal EFI and set
   the PG106 `Kernel → Add` entry to `Enabled = false` (or copy
   `EFI-backup` back). Reboot from the internal disk.
2. **AuxKC suspected (path 1B).** In the OpenCore picker press Space to show
   auxiliary entries and boot the **Recovery** entry, or boot macOS in Safe
   Mode (hold Shift after selecting the macOS entry), which skips the AuxKC.
   Then apply the 1B rollback (`kmutil clear-staging`, remove the kext from
   `/Library/Extensions`, rebuild the AuxKC) and reboot.
3. **Picker itself broken** (should not happen: neither path edits OpenCore's
   own drivers): the USB stick, then restore `EFI-backup` onto the internal
   EFI.
4. Do not change SIP, Secure Boot model or boot-args as a "fix". If none of
   the above restores booting, the cause is outside this runbook; use your
   existing recovery plan.

## Summary table

| Step | Action | Pass evidence | Rollback |
| --- | --- | --- | --- |
| 0 | Baseline, EFI backup, dGPU-enabled check | `baseline.txt`; dGPU in ioreg; no `-wegnoegpu`/`_OFF` SSDT | delete `~/pascal-test` |
| 1 | Build; root:wheel; load via OpenCore (1A) or `kmutil` (1B); first with `pg106_stop_after=1` | `probe ... accepted`, `GP106 identified: BOOT_0 136000a1`, `vbios: using ACPI _ROM, 168960 bytes`, firmware lines, then (full mode) `boot acr FAIL ret=-78` at the signature patch (ACR VMM not ported); one PG106 ioreg node | 1A: disable `Kernel → Add` entry; 1B: unload, `kmutil clear-staging`, AuxKC rebuild; boot-args: restore `config.plist` |
| 2 | `sudo pg106_probe` | `abi 0.3`, same boot0, exit 0; status block as listed | none |
| 3 | Build NVK (pg106 backend) + test programs; `pg106_vk_smoke --driver …` (as yourself) | `4/4 passed`, exit 0, WindowServer PID unchanged | delete `~/pascal-test/nvk` |
| 4 | `offload_selftest`, `pg106_offload --vulkan`; 4c: install PG106Accel into the AuxKC (after PG106), `pg106_offload --list-metal` / `--metal`; power A/B/B′/C | `all passed`; `pg106_offload: PASS`, `wrong 0`; window on the internal panel; no new display; 4c: one `PG106Accel` node; once `PG106Ready = Yes`, `PG106AccelState = "Metal offered"` and the GP106 listed `headless 1`; watts + dGPU power state per state | 4a/4b/4d: none (programs exit); 4c: unload, delete from `/Library/Extensions`, `kmutil clear-staging`, AuxKC rebuild (or `-pg106acceloff`) |
