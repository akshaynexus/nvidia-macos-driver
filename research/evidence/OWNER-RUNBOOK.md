# OWNER RUNBOOK — Pascal (GP106M) bring-up on the Helios 300, macOS

Phase 6 deliverable (`HANDOFF-PASCAL.md` §6, gate: "the complete owner runbook:
what to build, install, and measure on the Mac — written for a careful human,
with rollback for every step"). Design rationale: `research/evidence/OFFLOAD-DESIGN.md`.

Target machine: Acer Predator Helios 300 (2017–2018) with GTX 1060 Mobile
(GP106M, PCI `10DE:1C20` or `10DE:1C60`), macOS booted via OpenCore with working
Intel HD 630 acceleration (`HANDOFF-PASCAL.md` §1; wiring facts in
`HELIOS300-PASCAL-RESEARCH.md` §2).

**What exists today.** Steps 0–2 can be run: the kext (`pascal/kext`: probe
gate, BAR0 map, BOOT_0 identity, a root-only user client with two read-only
selectors) and the probe tool `pg106_probe`. Steps 3 and 4 now have their programs: an NVK
build with the Pascal backend (`nvk/pascal`), the smoke test and the offload
window app (`pascal/tests-mac`). None of them has run on a Mac yet; they were
built and tested on Linux only (`pascal/tests-mac/README.md`). Step 3 also
needs a kext that finishes GPU bring-up. Step 4's Metal variant (4c) needs
PG106Accel, which does not exist yet; 4a/4b do not.

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

Either way the dGPU stays powered once the kext claims it: Pascal has no PMU
firmware, so no RTD3 (`HELIOS300-PASCAL-RESEARCH.md` §3.3).

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
```

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

- **Measure:**
  - `PG106: probe 10de:1c20 accepted (GP106M [GeForce GTX 1060 Mobile])` (or
    `1c60`), then `PG106: GP106 identified: BOOT_0 136000a1 (arch 0x136 rev
    0xa1)`. A real GTX 1060 reports `0x136000a1`
    (`pascal/core/include/pascal/pmc.h`).
  - `ioreg -r -c PG106` shows exactly one PG106 node, under your dGPU.
  - No framebuffer/accelerator appeared; `system_profiler SPDisplaysDataType`
    matches the baseline; WindowServer PID unchanged.
- **Diagnostic log lines:** `BOOT_0 read back ffffffff: BAR0 not decoding` =
  the dGPU is powered off (step 0 check); `probe ... rejected` = not a GP106
  mobile id; `bring-up failed: 0x...` = an IOKit call failed (send the log).

## Step 2 — User client probe

```sh
sudo ~/pascal-test/repo/pascal/kext/build/pg106_probe 2>&1 | tee ~/pascal-test/step2.log
time sudo ~/pascal-test/repo/pascal/kext/build/pg106_probe >/dev/null
```

The client is root-only by design; without `sudo` the open fails.

- **Measure:** `abi 0.2`, `boot0 0x136000a1` (the same word the kernel log
  printed), `flags gp10x gp106`, `result GP106 identified`, exit status 0.
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
- **Root, or the render boot-arg.** The render user client is
  administrator-only by default. That is a security choice left to you
  (BRIDGE-NVK-REVIEW.md §7.1). Pick one:
  - **3-root (recommended, nothing persistent).** Run the step 3–4
    programs with `sudo`.
  - **3-bootarg.** Add `pg106_render_users=1` to the boot-args in a
    **copy** of `config.plist` (`NVRAM → Add → 7C436110-… → boot-args`,
    alongside what is already there). Validate it with `ocvalidate`, make
    it active, reboot. Any local user can then submit GPU work to the
    GP106. Rollback: restore the previous `config.plist` and reboot. Check
    with `sysctl -n kern.bootargs`.
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
sudo $T/pg106_vk_smoke --driver $NVK 2>&1 | tee ~/pascal-test/step3.log ; echo "exit $?" >> ~/pascal-test/step3.log
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
  - `pg106: the PG106 user client refused this process (-EACCES)`: run with
    `sudo` or set the boot-arg.
  - `FAIL no GP106 Vulkan device` with no `pg106:` line: the kext is not
    loaded (step 1).
  - `has not brought the GPU up`: kext bring-up; send `step3-kernel.log`.
  - `error: fence wait = … (10 s bound)`, `VK_ERROR_DEVICE_LOST` or
    `did not complete within NVK_PG106_TIMEOUT_MS`: the GPU did not finish
    the work. Capture the logs (§"Capture logs BEFORE any reboot") and stop.
  - A `FAIL` line with bad words/pixels: wrong results. Send `step3.log`.
  - A hang that the 10 s bounds do not end: Ctrl-C, then capture logs. If
    the machine is unresponsive, see the stop conditions.
- **Rollback:** none for 3-root. The build lives in `~/pascal-test/nvk`
  (delete it to reclaim ~2 GB), and the GPU connection closes when the
  program exits. For 3-bootarg, the rollback above.

## Step 4 — IOSurface render, Intel composites

The offload path of `research/evidence/OFFLOAD-DESIGN.md` §1–2: the GP106
renders into IOSurfaces in system memory, and WindowServer composites them on
the Intel HD 630 onto the internal panel. The GP106 drives no display.
Prerequisite: step 3 passed. Same root / boot-arg choice.

**4a — the frame path without a window**

```sh
sudo $T/offload_selftest --driver $NVK 2>&1 | tee ~/pascal-test/step4a.log
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
sudo $T/pg106_offload --vulkan --driver $NVK --seconds 20 2>&1 | tee ~/pascal-test/step4b.log
sudo $T/pg106_offload --vulkan --driver $NVK --seconds 20 --copy 2>&1 | tee -a ~/pascal-test/step4b.log
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
- If no window appears under `sudo` (a GUI program run as root in your
  session usually shows its window, but some setups refuse it), use the
  boot-arg variant and run without `sudo`.
- **Rollback:** none. The window closes and the program exits after
  `--seconds`.

**4c — the same through Metal (only once PG106Accel exists)**

```sh
$T/pg106_offload --list-metal
sudo $T/pg106_offload --metal --seconds 20
```

Today the expected result is the Intel GPU only in the list, and
`FAIL no GP106 MTLDevice …` with exit 2. Metal offers a device only for an
accelerator that publishes `MetalPluginName`. The GP106's would be
PG106Accel (OFFLOAD-DESIGN §3.3), and it has not been written. Once it
exists, and the plugin (`NVMTLDriver.bundle`) and this NVK are installed
by the package:
- the list shows the GP106 with `headless 1`;
- `--metal` prints `metal device: …`, shows a red/yellow triangle on an
  animated background, and ends with `pg106_offload: PASS`.

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
>   is not `PEGP`), and the kext's own GET_STATUS power state if `pg106_probe`
>   prints it;
> - if the kext has powered the GPU off while idle, the first step-3 run
>   after that pays a full re-POST + ACR/GR boot. Time it
>   (`time sudo $T/pg106_vk_smoke --driver $NVK`) and record that time as
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

Then undo the load path you used (1A: disable/remove the `Kernel → Add` entry
and delete `EFI/OC/Kexts/PG106.kext`; 1B: the 1B rollback), reboot, and
confirm `kmutil showloaded --list-only | grep -i pg106` prints nothing. Only
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
| 1 | Build; root:wheel; load via OpenCore (1A) or `kmutil` (1B) | `probe ... accepted`, `GP106 identified: BOOT_0 136000a1`; one PG106 ioreg node | 1A: disable `Kernel → Add` entry; 1B: unload, `kmutil clear-staging`, AuxKC rebuild |
| 2 | `sudo pg106_probe` | `abi 0.2`, same boot0, exit 0 | none |
| 3 | Build NVK (pg106 backend) + test programs; `sudo pg106_vk_smoke --driver …` | `4/4 passed`, exit 0, WindowServer PID unchanged | delete `~/pascal-test/nvk`; boot-arg rollback if 3-bootarg was used |
| 4 | `offload_selftest`, `pg106_offload --vulkan` (+ `--metal` once PG106Accel exists), power A/B/B′/C | `all passed`; `pg106_offload: PASS`, `wrong 0`; window on the internal panel; no new display; watts + dGPU power state per state | none (programs exit); boot-arg rollback if used |
