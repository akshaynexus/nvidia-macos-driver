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
selectors) and the probe tool `pg106_probe`. Steps 3 and 4 need an NVK ICD on
the bridge and a Metal test app; **neither exists yet**, so those steps are
listed as not available and must not be improvised.

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

## Step 3 — NVK Vulkan smoke (NOT YET AVAILABLE)

Needs an NVK ICD built against `pascal/bridge` with a real user-client
transport, and GPFIFO/GR bring-up in the kext. None of that exists: the bridge
runs only against its mock transport, and the kext stops after identifying
the chip. Do not substitute `vulkaninfo` from MoltenVK or any other ICD — that
would test a different driver. This step will be written when those parts
exist.

## Step 4 — IOSurface render, Intel composites (NOT YET AVAILABLE)

Needs step 3 plus a Pascal build of the Metal bundle and a test app; neither
exists. The planned measurements (WindowServer PID unchanged, no new display,
headless-surface mode engaged, best-effort `powermetrics`) are in
`research/evidence/OFFLOAD-DESIGN.md` §2 and §6.

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
| 3 | NVK smoke | not yet available | — |
| 4 | IOSurface render | not yet available | — |
