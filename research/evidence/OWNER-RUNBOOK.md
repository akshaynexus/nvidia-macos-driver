# OWNER RUNBOOK — Pascal (GP106M) offload bring-up on the Helios 300, macOS

Phase 6 deliverable (`HANDOFF-PASCAL.md` §6, gate: "the complete owner runbook:
what to build, install, and measure on the Mac — written for a careful human,
with rollback for every step"). Design rationale: `research/evidence/OFFLOAD-DESIGN.md`.

This runbook installs **nothing persistent**. No `/Library/Extensions` copy, no
Auxiliary Kernel Collection rebuild, no NVRAM or OpenCore change, no installer.
Every artifact lives in one working directory and every action has an explicit
rollback. If you follow it exactly, a plain reboot returns the machine to its
prior state because nothing survives a reboot except the files you copied.

Target machine: Acer Predator Helios 300 (2017–2018) with GTX 1060 Mobile
(GP106M, PCI `10DE:1C20` or `10DE:1C60`), macOS booted via OpenCore with working
Intel HD 630 acceleration (`HANDOFF-PASCAL.md` §1; wiring facts in
`HELIOS300-PASCAL-RESEARCH.md` §2).

## Hard rules — read before anything else

- **Never place battery-critical work on the dGPU.** Pascal has no PMU
  firmware, therefore no RTD3: once the kext claims the card, the dGPU stays
  powered at VBIOS boot clocks until reboot
  (`HELIOS300-PASCAL-RESEARCH.md` §3.3; design §3.1). Run all tests on AC
  power. Do not run tests before travel, exams, or anything where a shorter
  battery day matters.
- **One step at a time; measure before advancing.** Each step's "measure" list
  is also the failure triage. If a step's expected result does not appear,
  stop, roll back (per-step rollback below), and record the output.
- **Capture logs BEFORE any reboot.** After a panic or hang, your first action
  is still log capture (§"Capture logs before rebooting"), not restart. Reboot
  destroys the evidence that names the failure.
- **Stop conditions — do not continue if any of these:**
  - A kernel panic at any step. Recovery is safe by construction (nothing
    persistent was installed; see below), but the session is over: capture
    logs, roll back, report.
  - The Mac fails to boot at any point. This should be impossible if the
    runbook was followed (no kernel collection or boot configuration was
    modified), which is itself diagnostic — see §"If the Mac will not boot".
  - The kext load asks you to change security settings (SIP/AMFI) you do not
    already have. Changing them is out of scope for this runbook; record the
    failure and stop.
  - ioreg shows the kext attached to a device that is NOT `10DE:1C20`/`1C60`
    (wrong PCI match). Unload immediately (step 1 rollback).

Why a panic cannot brick this test: the kext is never added to a kernel
collection — you load it manually and it disappears at reboot. The persistent
things that break booting for the Turing+ driver (AuxKC rebuild, OpenCore boot
args, IONDRVSupport exclusion — `docs/HOW-IT-WORKS.md` §2 and §4) are all
explicitly *not* performed here.

## Prerequisites

- Hardware: the Helios 300, AC power connected, and (recommended) a second
  working computer or phone with this runbook open.
- Software: Xcode Command Line Tools (`xcode-select --install` shows when
  present), `git`, `python3` (repo regression simtest uses it —
  `pascal/README.md`, "Build and test").
- macOS on this OpenCore setup must already load unsigned/adjacent kexts the
  same way the nullmoth driver ecosystem requires (SIP value, Secure Boot
  model — documented for the Turing+ driver in `docs/HOW-IT-WORKS.md` §4).
  **Do not change any of these settings for this test.** If SIP is fully on,
  step 1's load will fail with an authentication error — that is a stop
  condition, not something to fix here.
- Free disk space: ~2–5 GB for the build tree and Mesa checkout.
- Nothing from the Turing+ driver needs to be installed or removed. If the
  Turing+ driver *is* installed and active, note its state first (it and the
  Pascal kext are different bundle IDs and match different devices, but see
  `docs/MULTI-GPU.md` on multi-instance caution; the safest baseline is
  `-nvoff` or the Turing driver absent — either is fine, just record which).

**Rollback for prerequisites:** no action needed; nothing was changed.

## Step 0 — Prepare and record the baseline

```sh
mkdir -p ~/pascal-test && cd ~/pascal-test
git clone <this repo, branch pascal-gp106> repo
cd repo
python3 helios300-gp106-simtest.py        # must pass; suite contract: HANDOFF-PASCAL.md §2
```

Record the baseline (all into `~/pascal-test/baseline.txt`):

```sh
sysctl kern.bootargs > ~/pascal-test/baseline.txt
ioreg -r -c IOPCIDevice -d 1 -l -w0 | grep -E 'device-id|vendor-id|pcidebug' >> ~/pascal-test/baseline.txt
ps -p $(pgrep -x WindowServer) -o pid,etime >> ~/pascal-test/baseline.txt
system_profiler SPDisplaysDataType >> ~/pascal-test/baseline.txt 2>&1
```

- **Measure:** `kern.bootargs` recorded (so you can later prove no boot arg was
  changed); the dGPU visible as a PCI device with vendor `de100000` and
  device-id `1c20xxxx` or `1c60xxxx` little-endian (reading pattern per
  `docs/MULTI-GPU.md`); WindowServer PID (must be unchanged through step 4).
- **Rollback:** `rm -rf ~/pascal-test`. Nothing else to undo.

## Step 1 — Build the kext, load it manually, check the probe

Build with the kext build script (path fixed by this branch; the script itself
is produced by the kext work item — if it is absent, stop and report, do not
improvise a build):

```sh
ls pascal/kext/scripts/build-macos.sh     # confirm presence first
bash pascal/kext/scripts/build-macos.sh 2>&1 | tee ~/pascal-test/build.log
```

Load — manually, explicitly, with no boot-time registration:

```sh
# Use the bundle path and CFBundleIdentifier the build script prints.
sudo kmutil load -p <path/to/BuiltPascal.kext>
# If kmutil is unavailable on this OS build: sudo kextload <path/to/BuiltPascal.kext>
```

If macOS shows a "system extension blocked / allow" prompt, approving it is a
per-kext one-time action; if instead it demands a security-config change, stop
(hard rule above).

Check the probe result:

```sh
sudo kmutil showloaded --list-only | grep -i <bundle-id-fragment>
ioreg -l | grep -i <bundle-id-fragment>
log show --last 5m --predicate 'process == "kernel"' > ~/pascal-test/step1.log
sudo dmesg >> ~/pascal-test/step1.log
```

- **Measure (expectations):**
  - The kext appears loaded and matched to exactly one PCI device: the one with
    device-id `0x1C20`/`0x1C60` from your baseline (matching pattern per
    `docs/MULTI-GPU.md`).
  - The kernel log contains the probe reading: BAR0+0 PMC_BOOT_0 decoded as
    GP10x family `0x130`, chipset `0x136` (GP106). A real GTX 1060 reports
    boot0 `0x136000a1` (`pascal/README.md`, "Key facts"; decode logic
    `pascal/core/src/pmc.c`). The ACR/GR boot may not run at this step; a probe
    log alone is a pass for step 1.
  - No ioreg entry claims a framebuffer/IOAccelerator (the kext is
    display-less by design — `research/evidence/OFFLOAD-DESIGN.md` §1).
  - Intel display still live: `system_profiler SPDisplaysDataType` unchanged
    from baseline.
- **Capture:** `step1.log`, `build.log`, the ioreg lines.
- **Rollback:**
  ```sh
  sudo kmutil unload -b <CFBundleIdentifier>      # or: sudo kextunload <path>
  ioreg -l | grep -i <bundle-id-fragment>         # must print nothing
  ```
  If unload fails, a reboot is a guaranteed rollback (nothing is registered in
  any kernel collection) — but capture logs first. Delete nothing from the
  system; all files live under `~/pascal-test`.

## Step 2 — Userspace Client probe

The kext exposes a UserClient in the `NVRMUserClient` shape for userspace
transport (`HANDOFF-PASCAL.md` §3 Phase 3; personality pattern per
`HELIOS300-PASCAL-RESEARCH.md` §8). The build tree provides a small probe tool
that opens the client and reads the card through it (if the build script did
not produce one, that is a stop-and-report condition).

```sh
<path-to-built-probe-tool> 2>&1 | tee ~/pascal-test/step2.log
```

- **Measure (expectations):**
  - Client open succeeds while the kext from step 1 is loaded.
  - The client-side read returns the same boot0 value the kernel probe logged
    (`0x136000a1` class) — one variable changed (userspace path added), same
    answer.
  - Time the open→read round trip (`time <probe>`); record it. No expectations
    are claimed for the absolute number; it is a baseline for later regression.
  - Intel display and WindowServer still healthy (compare with baseline).
- **Capture:** `step2.log`.
- **Rollback:** the probe is a plain process — quitting it releases the client
  (`IOReturn` closes on exit). Verify with `sudo kmutil showloaded` that
  nothing new persisted. No other state exists to undo. If you want the kext
  gone too, apply the step 1 rollback.

## Step 3 — NVK sandbox Vulkan smoke

Goal: prove NVK-on-bridge enumerates the Pascal device and produces correct
pixels/bytes, entirely offscreen (no WindowServer involvement yet). "Sandbox"
= everything in `~/pascal-test`, no install of the Vulkan driver or ICD outside
it (the repo's own GPU probes use the same private-path pattern: "The harness
explicitly loads that binary into its own process; it does not install it" —
`docs/host-copy-coherence-review.md`).

```sh
# Use the ICD/JSON and smoke binary from the build tree (per the bridge item's
# build output; do not copy anything into /usr/local or /Library).
VK_DRIVER_FILES=~/pascal-test/repo/<built-icd>.json \
  <path-to-vulkan-smoke> 2>&1 | tee ~/pascal-test/step3.log
# Acceptable stand-ins if the build tree ships no runner:
#   vulkaninfo (from MoltenVK's toolset or the build tree)
#   any compute demo from Mesa's NVK test set the bridge item documents
```

- **Measure (expectations):**
  - Device enumeration reports a Pascal-class NVIDIA device (NVK supports
    Pascal, sm_60 — `HELIOS300-PASCAL-RESEARCH.md` §3.1).
  - A compute smoke's output matches its CPU reference (the repo's event-wait
    probe used exactly this correctness form: "every result is compared with a
    CPU reference" — `docs/EVENT-WAIT.md`).
  - Run it twice; second run exercises shader caches. Note any messages about
    semaphore/sync surfaces — the mapped-word sync is a specific design
    assumption (`research/evidence/OFFLOAD-DESIGN.md` §2.3, §6 item 5).
  - If the smoke supports a cross-queue test, run it: it validates producer →
    consumer ordering, the same correctness axis the Turing+ driver tests
    ("A cross-queue test committed a consumer's event wait before the
    producer's fill and signal" — `docs/EVENT-WAIT.md`).
- **Capture:** `step3.log`; on mismatch, the smoke's dump files.
- **Rollback:** no system state. If a Mesa shader cache was created under
  `~/.cache/` (mesa shader cache), remove the entries created today or the
  whole `~/.cache/mesa_shader_cache*` directory. Unload the kext (step 1
  rollback) if you are stopping here.

## Step 4 — Render into an IOSurface; Intel composites; WindowServer untouched

Goal: the offload architecture end-to-end — app renders on GP106, frames reach
the screen through the Intel HD 630, and WindowServer never notices a second
display owner (design §1–2). The plugin's headless surface mode is the switch
that enables this ("headless GPU: the display belongs to another GPU —
IOSurfaces are copied out to system memory", `plugin/nvmtl_vk.c:2468–2476`;
the contract is analyzed in `research/evidence/OFFLOAD-DESIGN.md` §2).

```sh
# Load the Metal bundle from a private path only — never /Library/GPUBundles.
# Build/probe per the plugin item's test harness; the repo precedent loads the
# driver bundle into its own process (docs/host-copy-coherence-review.md,
# "GPU regression" section).
<path-to-metal-test-app> ~/pascal-test/repo/<built-NVMTLDriver.bundle> \
  2>&1 | tee ~/pascal-test/step4.log
```

While the app renders, in a second terminal:

```sh
ps -p $(pgrep -x WindowServer) -o pid,etime          # PID must equal baseline
system_profiler SPDisplaysDataType | grep -iE 'chipset|resolution|displays'
log show --last 2m --predicate 'process == "WindowServer"' > ~/pascal-test/step4-ws.log
sudo powermetrics -n 1 -i 2000 2>&1 | tail -40 >> ~/pascal-test/step4-power.log   # best-effort
```

- **Measure (expectations):**
  - The test app's frames are visible/correct (the app's own pixel check, or
    screenshot comparison — screenshot before/after must show the app's content,
    and the rest of the desktop must be unchanged).
  - WindowServer PID identical to baseline (it was never restarted — a restart
    would indicate the Intel stack was disturbed; the Turing+ test harness uses
    exactly this invariant, `docs/EVENT-WAIT.md` "stops on … a changed
    WindowServer PID").
  - `SPDisplaysDataType` shows only the Intel-driven display set from baseline;
    no new display appeared (display-less dGPU).
  - Plugin log contains the headless-surface line from `nvmtl_vk.c:2474` (or
    the Pascal equivalent), proving the offload surface mode engaged.
  - Best-effort battery datapoint: `powermetrics` GPU/package power with the
    kext loaded and idle vs. rendering. The tool may not expose dGPU rails on
    this hardware; any reading is better than none — this answers design
    open question 3 (`research/evidence/OFFLOAD-DESIGN.md` §6). Do not stop the
    test over a missing power reading.
- **Capture:** `step4.log`, `step4-ws.log`, `step4-power.log`, screenshots.
- **Rollback:** quit the app (its bundle was loaded from `~/pascal-test` only);
  then the step 1 kext rollback (`kmutil unload`). Verify with the baseline
  checks (step 0) that nothing differs. Then, if you are done for the day:

## Final teardown

```sh
sudo kmutil unload -b <CFBundleIdentifier>   # if not already done
rm -rf ~/pascal-test                         # removes kext, logs, caches of this test
diff <(sysctl kern.bootargs) <(grep kern.bootargs ~/pascal-test/baseline.txt)  # before the rm, if you want proof
```

The system now has: no Pascal kext loaded, no files outside `~/pascal-test`
(now deleted), unchanged NVRAM, unchanged OpenCore config, unchanged kernel
collections. The dGPU returns to being an unclaimed PCI device at next boot.

## Capture logs BEFORE any reboot

In order, before restarting after any failure (panic, hang, wrong result):

```sh
mkdir -p ~/pascal-test/logs-$(date +%Y%m%d-%H%M%S) && cd ~/pascal-test/logs-*   # adjust
sudo dmesg                                                        > dmesg.txt
log show --last 30m --predicate 'process == "kernel" OR process == "WindowServer"' > logshow.txt
ioreg -l -w0                                                      > ioreg.txt
ls -la /Library/Logs/DiagnosticReports/ | tail -20                > panic-list.txt
cp /Library/Logs/DiagnosticReports/Kernel-*.panic . 2>/dev/null
sysdiagnose -f .                                                  # large; run if the box is healthy enough
```

Kernel panic files land in `/Library/Logs/DiagnosticReports/` as
`Kernel-<date>_<host>.panic`; `sysdiagnose` bundles system state. These are the
artifacts any follow-up debug session needs — a reboot after a panic without
this capture throws away the only record.

## If the Mac will not boot

By construction this runbook leaves no persistent state (no AuxKC/NVRAM/
OpenCore changes), so a boot failure means either a pre-existing condition or
the hard rule was violated. In that case, from macOS Recovery (hold power to
power off, then hold Command-R while starting):

1. Do **not** change SIP, Secure Boot, or boot-args.
2. Utilities → Terminal: confirm no Pascal files exist in persistent locations:
   `ls /Volumes/<Macintosh HD>/Library/Extensions /Volumes/<Macintosh HD>/Library/GPUBundles`
   — you should find nothing Pascal-related (the runbook never wrote there).
3. Reboot normally. If it still fails, the cause predates this runbook; use
   your pre-existing recovery plan (OpenCore backup config, `-nvoff`-style
   safe entry for the Turing driver — `docs/HOW-IT-WORKS.md` §5 documents the
   Turing driver's own safe-mode tool, which is unrelated to this test but is
   your machine's existing safety net).

## Summary table

| Step | Action | Pass evidence | Rollback |
| --- | --- | --- | --- |
| 0 | Baseline capture | files in `baseline.txt` | `rm -rf ~/pascal-test` |
| 1 | Build + `kmutil load`, ioreg probe | boot0 `0x136000a1`-class probe log; correct PCI match; no display claims | `kmutil unload -b <id>`; files deleted |
| 2 | UserClient probe | client open + matching read; timing recorded | quit probe process |
| 3 | NVK Vulkan smoke (offscreen) | device enumerates; compute matches CPU reference; optional cross-queue pass | none (private paths); clear Mesa shader cache |
| 4 | IOSurface render + Intel composite | correct pixels; WindowServer PID unchanged; headless-mode log line; power reading | quit app; step 1 unload |
