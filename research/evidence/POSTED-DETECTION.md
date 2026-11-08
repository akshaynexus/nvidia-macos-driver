# POSTED-state detection — how nouveau decides devinit already ran, and what we do on the Helios 300

Date: 2026-11-05. Resolves PRIOR-ART-AUDIT.md gap **P0-3** (companion code:
`pascal/core/src/diag.c` + `pascal/core/include/pascal/diag.h`, tests
`pascal/tests/test_diag.c`). Sources: nouveau linux master (fetched
2026-11-05 from torvalds/linux, cited file+function), the
`research/prior-art/pascal-egpu` submodule (file:line), and
`third_party/open-gpu-kernel-modules` (file:line).

---

## 1. The question

On an Optimus laptop the **system UEFI already executed the dGPU VBIOS at
POST** — that is the one known cut-through of the pascal-egpu devinit
circular dependency (pascal-egpu `FINDINGS.md:78-87`, `FINDINGS.md:137`
records Apple Silicon EFI as the counter-case: it never runs GPU Option
ROMs). By the time our kext attaches on the Helios 300, devinit has
therefore *probably* already run. Re-running it blindly is the one
first-contact step with a documented hardware-lockup precedent
(`FINDINGS.md:120-122`: raw priv-ring writes collapsed the ring, only a
power cycle recovered it, `FINDINGS.md:200-210`; audit §C.6).

So: **how does a driver know whether devinit already ran?**

## 2. What nouveau actually does (cited)

Chain on every GF100+ chip including GP106 (all nouveau citations are
`drivers/gpu/drm/nouveau/...`):

1. **Wiring.** Every GP10x chipset entry in `nvkm/engine/device/base.c`
   instantiates `gm200_devinit_new` (the `0x13x` chipset rows: `.devinit =
   { 0x00000001, gm200_devinit_new }` for chipsets 0x130/0x132/0x134/0x136/
   0x137/0x138/0x13b).
2. **The POST evidence bit.** `nvkm/subdev/devinit/gf100.c`,
   `gf100_devinit_preinit`:
   ```c
   /*
    * This bit is set by devinit, and flips back to 0 on suspend. We
    * can use it as a reliable way to know whether we should run devinit.
    */
   base->post = ((nvkm_rd32(device, 0x2240c) & BIT(1)) == 0);
   ```
   BAR0 offset **0x02240c, bit 1**: set by devinit itself; clear (and post
   needed) after a power-off. `gm200.c` wires this exact function in:
   `gm200_devinit = { .preinit = gf100_devinit_preinit, ... }`. Note the
   polarity: **bit SET ⇒ already POSTed ⇒ do not run devinit.**
3. **Consuming the flag.** `nvkm/engine/device/base.c`, `nvkm_device_preinit`
   runs all subdev preinits (so `init->post` is populated) and then calls
   `nvkm_devinit_post(device->devinit)` (`nvkm/subdev/devinit/base.c`), which
   forwards the flag: `init->func->post(init, init->post)`.
4. **Gating the devinit work.** `nvkm/subdev/devinit/gm200.c`,
   `gm200_devinit_post`: DEVINIT upload to the PMU and execution are wrapped
   in `if (post)` — tables from BIT `I` (+0x14/0x16, +0x18/0x1a) and the
   boot-script execution (`pmu_exec`) happen **only when `post` is true**;
   `pmu_load(..., post, ...)` returns early otherwise. The BIT-`I` offsets
   match pascal-egpu's VBIOS findings (`FINDINGS.md:25,113`).
5. **Overrides and suspend.** `nvkm/subdev/devinit/base.c`:
   `nvkm_devinit_ctor` reads the module config option
   `nvkm_boolopt(device->cfgopt, "NvForcePost", false)`; `nvkm_devinit_preinit`
   applies it on the first call. `nvkm_devinit_fini` forces
   `init->post = true` when `suspend != NVKM_POWEROFF` ("force full reinit on
   resume"). So the module-param story is: **no direct "skip devinit" knob;
   only a force-POST override** — the default decision always comes from the
   hardware latch readback, never from a guess.
6. **Historical (pre-GF100) heuristics — not used on GP106 but documented
   evidence channels.** `nvkm/subdev/devinit/nv50.c`, `nv50_devinit_preinit`:
   (a) no display engine ⇒ assume secondary GPU ⇒ post; (b) "magic to detect
   whether or not x86 vbios code has executed the devinit scripts": read VGA
   registers via `nvkm_rdvgac(device, 0, 0x00)` and `... 0x1a)` — both zero ⇒
   "adaptor not initialised" ⇒ post. On Pascal this is replaced entirely by
   the 0x02240c bit.
7. **Devinit-disable straps.** After the post decision,
   `nvkm/subdev/devinit/gm107.c`, `gm107_devinit_disable` (used by gm200's
   `.disable`) reads strap registers `0x021c00`/`0x021c04` that the VBIOS/
   devinit set to mark engines as absent, and disables the corresponding
   nvkm subdevs per bit. This is the strap channel audit §C.6 pointed at.

### What the NVIDIA SDK adds

- `open-gpu-kernel-modules/src/common/inc/swref/published/nv_ref.h:108,139`:
  `NV_PMC_BOOT_0` = 0x00000000, `NV_PMC_BOOT_1` = 0x00000004 (BOOT_1 bits
  17:16 = vGPU flag; 0 on real silicon).
- The 0xbad* failure readbacks are documented as PRI error codes:
  `maxwell/gm107/dev_pri_ringstation_sys.h:28-42`,
  `NV_PPRIV_SYS_PRI_ERROR_CODE` in bits **[31:8]** (0xBAD001 host PRI
  timeout, 0xBAD0B0 host FB ack timeout, 0xBADF10-0xBADF50 FECS errors…).
  Our decoder (`pascal/core/src/diag.c`) decodes exactly this field, plus the
  three codes only pascal-egpu observed (0xBAD002, 0xBAD0DA, 0xBAD0AC).
- PTIMER: `kepler/gk104/dev_timer.h:35,37` — `NV_PTIMER_TIME_0` = 0x9400,
  `TIME_1` = 0x9410; TIME_0 counts in 32 ns units (field [31:5]). Nouveau's
  stable-read pattern is `nvkm/subdev/timer/nv04.c`, `nv04_timer_read`
  (`regsnv04.h:6-7` defines the same offsets). Real-GP106 confirmation:
  pascal-egpu `hw/mmio.py:72-73` and `hw/gpu_init.py:56-57`.
- The 0x02240c latch is **not named** anywhere in the SDK swref headers
  (searched `src/common/inc/swref/published/`): NVIDIA's RMAPI decides
  "needs POST" through GSP channels instead. Nouveau's gf100.c readback is
  the only public documentation of the bit.

## 2b. What "running devinit" means on GP106 — and the model (added 2026-11-07)

Upstream lines below are from linux master, sparse clone `/tmp/review-ref/linux`
(torvalds/linux d48723711bae). Code: `pascal/core/{include/pascal,src}/
devinit_pmu.*`, test `pascal/tests/test_devinit_pmu.c`.

`nv136_chipset` (`nvkm/engine/device/base.c:2177`) uses `gm200_devinit_new`.
`gm200_devinit_post` (`devinit/gm200.c:123-176`) is the POST. It does **not**
interpret the BIT `I` boot scripts on the host:

1. BIT `I` must be **version 1, length ≥ 0x1c**, else `-EINVAL` "VBIOS PMU
   init data not found" (`gm200.c:134-138`).
2. `pmu_load(0x04)` finds the VBIOS **PMU DEVINIT application** through BIT
   `p` (`bios/pmu.c:74-102`, `nvbios_pmuRm`), even when `post` is false.
   If it is missing, the result is `-EINVAL` "VBIOS PMU/DEVINIT not found".
   With `post`: falcon reset, IMEM boot (0x10a180/0x10a188/0x10a184), secure
   IMEM code, DMEM data (0x10a1c0/0x10a1c4) (`gm200.c:32-112`).
3. `pmu_args(args+8, 0x08)` gives the DMEM address for the **init tables**
   (`I`+0x14 pointer / +0x16 length). `pmu_args(args+8, 0x10)` gives the
   address for the **boot scripts** (`I`+0x18 / +0x1a). Both are uploaded with
   `pmu_data`.
4. `0x10a040 = 0x5000`, `pmu_exec(init)` (0x10a104, 0x10a10c, 0x10a100 = 2),
   then poll 0x10a040 bit 13 for 2 s, else `-ETIMEDOUT`.
5. `gm200_devinit_preos` uploads/execs the PRE_OS app (type 0x01); its
   result is ignored.

A failure here fails `nvkm_device_preinit` (`device/base.c:3016`).
`pg106_devinit_gm200_post` replays this sequence as a step trace over a bus.
The test's mock PMU falcon decodes the IMEM/DMEM port writes and checks that
the uploaded bytes equal the ROM ranges (including the `nvbios_addr` image0 →
0xe0-image remap, `bios/base.c:31-39, 170-178`).

**Real-ROM results** (`research/firmware/vbios/`):

| ROM | result |
|---|---|
| msi-gtx1050ti-mobile (GP107M, full 4-image dump) | full upload: DEVINIT app at 0x16034, boot 0x100 B + code 0x3840 B → IMEM, data 0x4f0 B → DMEM, tables ROM 0x49fb/0x50a → DMEM 0x4f0, scripts ROM 0x8287/0x497c → DMEM 0xa00, PRE_OS present |
| gigabyte-gtx1060-6gb, asus-gtx1050ti, msi-gtx1050 | **"VBIOS PMU/DEVINIT not found"**: the `p` table pointer is 0xf0e4, but these files were carved to the PCIR image length (0xf000), which drops the type-0xe0 images that hold the PMU ucode |
| nvidia-gtx1060-3gb | **"VBIOS PMU init data not found"**: `I` is only 0x12 long; its BIT layout (version 70.26, `p` v1, analog DCB outputs) is pre-Pascal-like, so the image is not a credible GP106 reference |

The genuine Pascal boot scripts also all stop the host interpreter at opcode
**0xac**, which is not in nvkm's host `init_opcode[]` table. That is consistent
with them being PMU-application input, not host-interpreter input.

**Consequences for the owner dump:** dump the **entire** ROM (all images,
not just image 0 / the PCIR length). Without the 0xe0 images the POST path has
nothing to upload.



**Known (sourced):**
- Which register to read (0x02240c bit 1) and its semantics on GF100+
  silicon as nouveau understands it (§2.2).
- That on GP106 the devinit path is gm200's, gated by exactly that bit (§2).
- That system UEFI on an Optimus laptop runs the VBIOS Option ROM (audit §1,
  research doc §2), so the latch *should* be set at macOS attach time.
- The failure signatures a partially-alive GPU produces
  (`FINDINGS.md:29-44`), which the probe classifies before deciding anything.

**Unknown (resolvable only on the Helios 300; §5 resolves them):**
- Whether the Helios firmware actually leaves bit 1 SET at macOS attach, or
  whether an ACPI/_DSM power transition (D3hot on the dGPU, unknown for this
  laptop — audit "Uncertainties") cleared it. The nouveau comment says the
  bit "flips back to 0 on suspend"; whether Optimus power-gating counts is
  **not** documented anywhere public.
- Whether WPR2 keeps a firmware-established value (`FINDINGS.md:54`
  documented `WPR2_HI = 0xccccfcfc`, sticky) on this laptop.
- The raw PSTRAPS value (offset per pascal-egpu `PLAN.md:149` = 0x101000).
- [Resolved 2026-11-05 by MMIO-MAP-CORRECTIONS.md: `mmio.h` and the simtest
  now map PTIMER at 0x009000.] One internal inconsistency we must record: **our `pascal/core/include/
  pascal/mmio.h` maps the PTIMER block at 0x020000-0x020fff** (from
  `helios300-gp106-simtest.py` §2), but every other source puts PTIMER TIME at
  0x9400/0x9410 (nouveau `regsnv04.h:6-7`, SDK `dev_timer.h:35,37`,
  pascal-egpu `mmio.py:72-73`, measured on real GP106). `diag.h` defines
  `PG106_DIAG_PTIMER_TIME_0/1` at 0x9400/0x9410 and `diag.c` uses them. The
  simtest/mmio.h map should be reconciled in a separate, scoped change
  (mmio.h is out of this change's file scope; flagged here so it is not
  lost).

## 4. The decision rule (what the code implements)

`pg106_diag_probe_liveness()` — read-only, in this order:

| # | Read | Classification | Rationale |
|---|------|----------------|-----------|
| 1 | `PMC_BOOT_0` (0x000000) | all-ones/0xdead* → **ABSENT**; 0xbad* → **CLOCK_GATED**; not-GP106 → **ABSENT** | all-ones identity = pascal-egpu's unrecoverable end state (`FINDINGS.md:202`); error-code identity = PRI port alive but domain gated (`FINDINGS.md:31`); identity decoded via `pg106_pmc_probe` (`pmc.h`, nouveau `base.c` decode) |
| 2 | `PBUS_BAR0_WINDOW` (0x1700, `dev_bus.h:43`) | 0xbad* → **CLOCK_GATED** | harmless read-only readback; error here means the PBUS domain itself is gated |
| 3 | PTIMER `TIME_1/TIME_0` (0x9410/0x9400), two samples | `timer_ticked` recorded | nouveau's stable-read pattern (`nv04_timer_read`); corroboration only, never gates |
| 4 | devinit latch (0x02240c) | bit set → **ALIVE_POSTED** (skip devinit); clear → **ALIVE_UNPOSTED** (devinit allowed); unreadable (0xbad*/ff/0xdead*) → **ALIVE_UNVERIFIED** | the nouveau POST evidence bit (§2.2); UNVERIFIED must refuse devinit — that is the documented lockup risk (audit P0-3) |
| 5 | PSTRAPS (0x101000, `PLAN.md:149`); [2026-11-07: the WPR2 lo/hi 0x100ce0/0x100ce4 reads were removed (`454a5ad`) — no GP106 source; the WPR window is read by `pg106_diag_wpr_window` via the 0x100cd4 selector, outside this probe because it writes] | recorded in the log only | hardware-session diff material, per audit §C.6 ("read and log before ever running devinit") |

**Rule for the kext: devinit runs only when state == ALIVE_UNPOSTED.**
POSTED skips it (firmware did it), UNVERIFIED stops and collects logs, every
other state has no working bus to devinit into.

## 5. RECOMMENDED owner procedure — deciding on the real Helios 300 (Linux liveboot, read-only)

Purpose: capture the ground truth of §3's unknowns before any macOS kext
attach. Every step is read-only; **never write a register** (priv-ring
writes are the documented unrecoverable lockup, `FINDINGS.md:120-122`).

1. **Boot a Linux live USB with nouveau blacklisted**, so nothing else
   touches the GPU:
   - At the GRUB menu press `e`, append to the `linux` line:
     `modprobe.blacklist=nouveau`, boot with `Ctrl-X`. (Do not add any
     nvidia driver; do not load the macOS side in between.)
2. **Find the GPU and its BAR0:**
   ```sh
   sudo -i
   lspci -nn | grep -i nvidia          # expect: 10de:1c20 or 10de:1c60
   lspci -v -s 01:00.0 | grep -E "Memory at|Kernel"   # note the adapter
   ```
   Note the first (large) memory region — that is BAR0 (16 MB on GP106).
   Example used below: base `0xb4000000`. **Substitute your own value.**
   If the GPU is absent or shows "rev ff" in `lspci -vvv -s 01:00.0`, the
   dGPU is power-gated or off the bus: try a cold boot (full power-off,
   not reboot), and disable "fast boot" in UEFI if present; record that
   outcome too — it answers the Optimus power-state unknown on its own.
3. **Get a physical-memory reader:** `apt install busybox-static` (live
   sessions with network; on offline media use the `devmem2` binary or a
   one-file C reader) and use `busybox devmem ADDR` (32-bit by default).
4. **Identity sanity:**
   ```sh
   busybox devmem 0xb4000000          # BAR0 + 0x000000 PMC_BOOT_0
   ```
   - `0x136000A1` → GP106 rev A1, bus alive (matches `FINDINGS.md:15` and
     `pascal/tests/test_pmc.c`). Continue.
   - `0xFFFFFFFF` → device not responding (state ABSENT): stop, record,
     retry from cold boot.
   - `0xBAD*`/`0xDEAD*` → decode with the table in
     `pascal/core/src/diag.c`; record the raw word.
5. **The POSTED decision (the whole point):**
   ```sh
   busybox devmem 0xb402240c          # BAR0 + 0x02240c devinit latch
   ```
   Bit 1 (`value & 2`): **1 ⇒ system firmware already ran devinit** (the
   kext must skip devinit); **0 ⇒ the GPU reached the OS un-POSTed** (the
   kext's devinit path is genuinely needed). Record the full word, not just
   the bit.
6. **Timer and corroboration reads** (all read-only; record raw values):
   ```sh
   busybox devmem 0xb4009400          # PTIMER_TIME_0, run twice
   busybox devmem 0xb4009400          #   values must differ (timer ticking)
   busybox devmem 0xb4009410          # PTIMER_TIME_1
   busybox devmem 0xb4100ce0          # + 0x100ce0 PFB WPR2_ADDR_LO
   busybox devmem 0xb4100ce4          # + 0x100ce4 PFB WPR2_ADDR_HI
   busybox devmem 0xb4101000          # + 0x101000 PSTRAPS
   busybox devmem 0xb4001700          # + 0x1700 PBUS_BAR0_WINDOW
   ```
   (Offsets: base `0xb4000000` + `0x100ce4` = `0xb4100ce4`, etc. — recompute
   for your BAR0.)
7. **Repeat across boot states** to separate "POST effect" from "power
   state": (a) warm reboot of the live OS (no power loss), (b) full power
   off → on → liveboot again, (c) if practical, after a Windows/UEFI boot
   that exercised the dGPU. Fill in this table and commit it to
   `research/evidence/` (values + boot state + date):

   | state | BOOT_0 | 0x2240c | timer ticks | WPR2_HI | PSTRAPS |
   |---|---|---|---|---|---|
   | warm reboot | | | | | |
   | cold boot | | | | | |

8. **Interpretation:** if `0x2240c & 2` is stable 1 across warm reboots, the
   kext ships with the POSTED gate defaulting to "skip devinit" on this
   machine; if it is 0 after cold boot, the fallback POST path is
   load-bearing — that is the **gm200 PMU DEVINIT upload**
   (`core/src/devinit_pmu.c`, §2b), not the host script interpreter
   (`core/src/devinit.c` only runs DCB/IED/DP scripts on GP10x) — and the
   WPR2/strap values become its pre-state record. It also needs the full ROM
   dump (§2b). Any 0xbad*/0xffffffff readback: decode with
   `pg106_diag_decode_readback` and keep the raw words — per handoff §7,
   the failed boot's data is the evidence.

## 6. Relation to other gaps

- Closes P0-3 (this doc + `pg106_diag_probe_liveness`).
- Gives P0-2 its decoder (`pg106_diag_decode_readback`, tested in
  `pascal/tests/test_diag.c` against the exact `FINDINGS.md` values); the
  runbook logging step (§5.4) is where the OWNER-RUNBOOK "what the log
  means" hooks in.
- Serves P1-4's discriminator: `pg106_diag_falcon_exec()` encodes the
  UCODE_LEVEL=3 "HALTED is a security exception" warning
  (`FINDINGS.md:164-180`) so the kext log cannot mistake it for success.

## 7. Open questions

0. Does the Helios 300 ROM (all images) carry the BIT `p` DEVINIT app like
   the GP107M dump does? `test_devinit_pmu.c` will report it as soon as the
   file is under `research/firmware/vbios/`.

1. PTIMER block address: `mmio.h`/simtest say 0x020000, everything else says
   0x9400 (§3). Needs a scoped mmio.h/simtest correction + a hardware
   readback from §5.6 (reading 0x9400 twice is harmless either way).
2. Does the 0x02240c latch survive Optimus D3hot cycles on this laptop?
   Only §5.7's state matrix answers it.
3. Is 0x02240c bit 1 set by *system-firmware-driven* devinit identically to
   a nouveau-driven post? Nouveau's comment implies the bit belongs to
   devinit (the firmware), not to the caller — but no second public source
   confirms it; §5.5 is the check.
4. PSTRAPS at 0x101000 (`PLAN.md:149`) is a pascal-egpu table entry, not
   SDK-verified (no pascal strap header in `swref/published/pascal/`);
   treat the §5.6 readback as a record, not a decoded fact.
