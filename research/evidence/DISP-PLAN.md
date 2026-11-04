# DISP-PLAN — Pascal display engine mode-set (Phase 5B) and the macOS framebuffer design

**Status: SIMULATION.** Everything below is a paper model validated by unit test
(`pascal/tests/test_disp_mode.c`, over a mock bus recorder). No hardware was
touched and no hardware claim is made (HANDOFF-PASCAL.md §7 evidence rules).
Items that intrinsically require the Helios 300 are listed in **Open questions**.

Target: GTX 1060 Mobile (GP106M) in the Acer Predator Helios 300 (2017).
**Display wiring (HELIOS300-PASCAL-RESEARCH.md §2):** no MUX — the internal
panel is physically wired to the Intel iGPU; **HDMI and miniDP are wired
directly to the dGPU**. The Pascal disp engine therefore only ever drives
HDMI/miniDP heads; the internal panel stays on Intel, always. There is exactly
one modeled head here (head 0 → SOR 0, HDMI, 1080p60).

Upstream basis: nouveau `nvkm/engine/disp/` from torvalds/linux master, fetched
to `/tmp/pascal-ref/disp/` (gf119.c, gp102.c, head.c, head.h, ior.c, ior.h,
outp.c, nv50-head*.c, nv50-sor907d.c, nv50-core507d.c, nv50.c, nvhw/class/
cl907d.h, cl507d.h). Research doc §7 confirms the same inventory
(`gp102.c`, `gf119.c` CPU-driven base, `dp.c`, `outp.c`, `head.c`, `ior.c`).

---

## 1. Key structural fact the model is built on

On GF119 and everything after it (GP106 included, via `gp102_disp` wiring:
`.head = gf119_head_cnt/gf119_head_new`, `.sor = gf119_sor_cnt/gp100_sor_new`,
`gp102.c gp102_disp`), **the EVO core-channel pushbuffer methods are the PDISP
head/SOR registers**:

| EVO method (cl907d.h)              | offset            | hardware readback (gf119.c)                      |
| ---------------------------------- | ----------------- | ------------------------------------------------ |
| `NV907D_HEAD_SET_RASTER_SIZE(a)`   | 0x0414 + a*0x300  | `gf119_head_state`: 0x640414 + hoff (vtotal<<16 \| htotal) |
| `NV907D_HEAD_SET_RASTER_SYNC_END(a)` | 0x0418 + a*0x300 | `gf119_head_state`: 0x640418 (vsynce<<16 \| hsynce) |
| `NV907D_HEAD_SET_RASTER_BLANK_END(a)` | 0x041C + a*0x300 | `gf119_head_state`: 0x64041c                    |
| `NV907D_HEAD_SET_RASTER_BLANK_START(a)` | 0x0420 + a*0x300 | `gf119_head_state`: 0x640420                  |
| `NV907D_HEAD_SET_PIXEL_CLOCK_FREQUENCY(a)` | 0x0450 + a*0x300 | `gf119_head_state` `state->hz`: 0x640450     |
| `NV907D_HEAD_SET_CONTROL_OUTPUT_RESOURCE(a)` | 0x0404 + a*0x300 | `gf119_head_state` or.depth from 0x640404 bits 9:6 |
| `NV907D_SOR_SET_CONTROL(a)`        | 0x0200 + a*0x20   | `gf119_sor_state`: 0x640200 + id*0x20 (proto 11:8, head 3:0) |

(`hoff = id*0x300` armed / `+0x20000` async in `gf119_head_state`; head count
read from `0x612004` bits 7:4 in `gf119_head_cnt` — 4 heads.)

So the simulated sequence writes the method offsets directly; on real hardware
the same values arrive either through the EVO core channel pushbuffer
(nouveau's way: `nv50-head907d.c head907d_mode`, `head907d_or`,
`nv50-sor907d.c sor907d_ctrl`) or are readable back at `0x640xxx` exactly as
`gf119_head_state`/`gf119_sor_state` do. Both sides are cited per step.

## 2. Timing arithmetic (nouveau's own, nv50-head.c `nv50_head_atomic_check_mode`)

```
m.h.active = crtc_htotal                       m.v.active = crtc_vtotal
m.h.synce  = hsync_end - hsync_start - 1       m.v.synce  = vsync_end - vsync_start - 1
m.h.blanke = hblank_end - hsync_start - 1      m.v.blanke = vblank_end - vsync_start - 1
m.h.blanks = h.blanke + hdisplay               m.v.blanks = v.blanke + vdisplay
```

`drm_mode_set_crtcinfo` sets `crtc_hblank_end == crtc_htotal` (progressive),
hence `hblank_end = 2200`, `vblank_end = 1125`. The `-1` bias is explained in
the source: the hardware raster counter repeats over an interval starting one
unit into the sync pulse, not at active start.

**Golden values for CEA-861 1080p60** (pixel clock 148500 kHz, htotal 2200,
vtotal 1125; hblank 280 = 88+44+148, vblank 45 = 4+5+36 — the numbers a
1080p60 EDID DTD carries; hard-coded in `test_disp_mode.c` with a cross-check
note against the sibling parser `pascal/core/src/edid.c`):

| derived   | value | absolute sanity                                |
| --------- | ----- | ---------------------------------------------- |
| hsynce    | 43    | 2052−2008−1                                     |
| hblanke   | 191   | 2200−2008−1                                     |
| hblanks   | 2111  | (2008+2111) mod 2200 = 1919 = hdisplay−1 ✓      |
| vsynce    | 4     | 1089−1084−1                                     |
| vblanke   | 40    | 1125−1084−1                                     |
| vblanks   | 1120  | (1084+1120) mod 1125 = 1079 = vdisplay−1 ✓      |

The two "mod" identities are asserted in the test as an internal-consistency
check of the whole conversion.

## 3. The modeled sequence (23 steps, `pg106_disp_modeset_plan`)

Phase structure mirrors nouveau's atomic flow (disable first, program, enable
last; the final UPDATE mechanism is `nv50-core507d.c core507d_update`):

| #  | step name                    | kind | reg          | value        | source (file : function) |
| -- | ---------------------------- | ---- | ------------ | ------------ | ------------------------ |
| 1  | sor0.owner-none              | EVO  | 0x0200       | 0x00000100   | nv50-sor907d.c sor907d_ctrl; cl907d.h OWNER_MASK_NONE (decode: gf119.c gf119_sor_state) |
| 2  | sor0.power-down              | MMIO | 0x61C004     | 0x80000000   | nv50.c nv50_sor_power (gf119.c gf119_sor .power) |
| 3  | core.update-disable          | EVO  | 0x0080       | 0x00000002   | nv50-core507d.c core507d_update; cl507d.h INTERLOCK_WITH_BASE0 |
| 4  | vpll0.clock                  | block| VPLL0        | 148500 kHz   | nv50.c nv50_disp_super_2_1 (from gf119.c gf119_disp_super) |
| 5  | head0.raster-size            | EVO  | 0x0414       | 0x046508A8   | nv50-head907d.c head907d_mode; readback gf119.c gf119_head_state 0x640414 |
| 6  | head0.raster-sync-end        | EVO  | 0x0418       | 0x0004002B   | head907d_mode; gf119_head_state 0x640418 |
| 7  | head0.raster-blank-end       | EVO  | 0x041C       | 0x002800BF   | head907d_mode; gf119_head_state 0x64041c |
| 8  | head0.raster-blank-start     | EVO  | 0x0420       | 0x0460083F   | head907d_mode; gf119_head_state 0x640420 |
| 9  | head0.raster-vert-blank2     | EVO  | 0x0424       | 0            | head907d_mode (0 for progressive) |
| 10 | head0.pixel-clock-frequency  | EVO  | 0x0450       | 0x08D9EE20   | head907d_mode; gf119_head_state state->hz 0x640450 |
| 11 | head0.pixel-clock-configuration | EVO | 0x0454     | 0x00200000   | head907d_mode; cl907d.h MODE_CLK_CUSTOM |
| 12 | head0.pixel-clock-frequency-max | EVO | 0x0458     | 0x08D9EE20   | head907d_mode |
| 13 | head0.output-resource        | EVO  | 0x0404       | 0x00000140   | nv50-head907d.c head907d_or; depth readback gf119.c gf119_head_state 0x640404 bits 9:6 |
| 14 | head0.control                | EVO  | 0x0408       | 0x31EC6000   | head907d_or (0x31ec6000 \| head<<25 \| STRUCTURE) |
| 15 | sor0.owner                   | EVO  | 0x0200       | 0x00000101   | nv50-sor907d.c sor907d_ctrl; routing decode gf119.c gf119_sor_state; supervisor nv50.c nv50_disp_super_2_2 |
| 16 | head0.rgclk                  | MMIO | 0x612200     | 0x00000001   | gf119.c gf119_head_rgclk (from nv50.c nv50_disp_super_2_2) |
| 17 | sor0.clock                   | MMIO | 0x612300     | 0x00280000   | gf119.c gf119_sor_clock (TMDS 0x0a<<18; 148.5 MHz < 165 MHz → not high_speed) |
| 18 | sor0.hdmi-infoframe-gate-off | MMIO | 0x6167A4     | 0x00000000   | gf119.c gf119_sor_hdmi_ctrl |
| 19 | sor0.hdmi-infoframe-unk10    | MMIO | 0x6167AC     | 0x00000010   | gf119.c gf119_sor_hdmi_ctrl ("??? InfoFrame?") |
| 20 | sor0.hdmi-infoframe-gate-on  | MMIO | 0x6167A4     | 0x00000001   | gf119.c gf119_sor_hdmi_ctrl |
| 21 | sor0.hdmi-ctrl               | MMIO | 0x616798     | 0x401F0000   | gf119.c gf119_sor_hdmi_ctrl (HDMI_CTRL: enable \| max_ac_packet<<16 \| rekey) |
| 22 | sor0.power-on                | MMIO | 0x61C004     | 0x80000001   | nv50.c nv50_sor_power |
| 23 | core.update-enable           | EVO  | 0x0080       | 0x00000002   | nv50-core507d.c core507d_update — **always the last step** |

Depth encoding: 24 bpp → `PIXEL_DEPTH = BPP_24_444 (5) << 6` (cl907d.h:123);
the reverse mapping is `gf119_head_state` (case 5 → 24). Sync polarity: CEA
1080p60 is +H/+V → both `POSITIVE_TRUE (0)`. The UPDATE interlock value 0x2 is
the BASE0 interlock bit (cl507d.h `NV507D_UPDATE_INTERLOCK_WITH_BASE0`).

**Deterministic diff gate:** `pg106_disp_modeset_diff` is exercised in the test
— two runs of the same mode diff clean; a one-bit perturbation is caught at the
exact step index.

## 4. Mapping onto the future macOS framebuffer (NVRMFB-style)

Design sketch for the GPL-2.0 kext's display half (`pascal/kext/`), display
only on the dGPU-wired HDMI/miniDP heads:

1. **Controller bring-up.** The kext maps BAR0 (PDISP at +0x610000, per the
   repo's MMIO map, `helios300-gp106-simtest.py`) and runs the disp init
   equivalent of `gf119.c gf119_disp_init`: capability mirror loops
   (0x616104/8/c → 0x6101b4/b8/bc; SOR caps 0x61c000 → 0x6301c4), the
   "steal display away from vbios" step (0x6100ac bit 8 → mask 0x6194e8),
   and `nvkm_wr32(0x610010, (inst >> 8) | 9)` to point at the display RAMHT.
   *(Simulation provides the step list; the init sequence itself is not yet
   planner-emitted — see open questions.)*
2. **Channel model (from `gp102.c`, the GP106 glue).** The kext hosts the
   userspace-visible channel classes exactly as `gp102_disp.user` wires them:
   core channel `GP102_DISP_CORE_CHANNEL_DMA` (ctrl 0, user 0), base channel
   `GK110_DISP_BASE_CHANNEL_DMA` (ctrl 1, user 1), cursor
   `GK104_DISP_CURSOR` (ctrl 13, user 17), overlay-imm `GK104_DISP_OVERLAY`
   (ctrl 9, user 13), overlay `GK104_DISP_OVERLAY_CONTROL_DMA` (ctrl 5, user
   5). Core-channel hardware init = `gp102.c gp102_disp_core_init`
   (0x611494/8/c, 0x610490 = 0x01000013, suspend-put at 0x640000).
3. **Framebuffer = scanout surface behind the base channel.** An IOKit
   framebuffer (NVRMFB-style: one per active head) exposes the mode; on
   `setMode` the kext runs the §3 planner and pushes the EVO methods through
   the core-channel pushbuffer, ending in `UPDATE` (0x0080) — which is what
   actually latches a coherent mode-set at vblank. Scans-out BO: the base
   channel's `SET_CONTEXT_DMA` target, i.e. an IOSurface-backed GPU allocation
   that NVK writes and PDISP reads — the same offload IOSurface the Phase 6
   design hands to the Intel side for compositing when the dGPU is not the
   presenter.
4. **Vblank for the framebuffer's compositing heartbeat.** Head vblank
   enable/disable is `gf119.c gf119_head_vblank_get/put` (bit 0 of
   0x6100c0 + head*0x800); interrupt state at 0x6100bc.
5. **Supervisor/owner changes.** IOR (re)assignment at runtime goes through
   the supervisor interrupt path (`gf119.c gf119_disp_super`: pending at
   0x6100ac, per-head mask at 0x6101d4 + id*0x800, ack 0x6101d0 bit 31), which
   calls `nv50_disp_super_2_1` (VPLL) and `nv50_disp_super_2_2` (rgclk, SOR
   clock) — the exact steps the planner emits at 4, 16 and 17.

On this machine the framebuffer's total possible audience is **external HDMI
and miniDP only** (research §2); the internal panel is never a dGPU head, so
the kext registers no panel framebuffer and never touches LVDS protocol
(which is why the LVDS special-case in `nv50_disp_super_2_2` is out of scope).

## 5. Simulation vs hardware-needed

| Item | Status |
| ---- | ------ |
| Step order, register/method offsets, field encodings | **Simulation, derived from source** (every step cited; offsets cross-checked between cl907d.h write side and gf119.c readback side) |
| Timing arithmetic | **Simulation**, exact nouveau math (nv50-head.c), round-trip-tested against CEA-861 1080p60 numbers |
| Determinism gate | **Simulation**, unit-tested |
| Anything on real GP106M silicon | **Hardware-needed** — owner-run macOS validation only; a PASS here is a plan-shape check, not a device claim |

## 6. Open questions (explicitly open; each blocks a hardware step, not the model)

- **DDC/i2c EDID read.** Connector detection needs the i2c subdev (nouveau
  `subdev/i2c`, not in the fetched disp set — research §7 lists `dp.c`/`outp.c`
  but i2c is a separate subdev). Until ported, modes come from hard-coded CEA
  numbers (test note; cross-check vs `pascal/core/src/edid.c`).
- **DP AUX for miniDP.** The miniDP head goes through the `gf119_sor_dp` path
  (gf119.c: `gf119_sor_dp_links`, `nvkm_ior_func_dp` in ior.h) which needs
  AUX transactions (`struct nvkm_i2c_aux` in ior.h) and link training
  (`dp.c`, 17K, not fetched). HDMI-first is deliberate.
- **Audio.** HDMI audio needs the HDA SOR path (`gf119_sor_hda`,
  `gf119_sor_hda_hpd/eld`, gf119.c; HDA presence flag read from 0x08a15c /
  0x101034 in `gm200.c gm200_sor_new`) plus ELD contents — unmodeled.
- **Infoframe content.** The planner enables the HDMI block
  (`gf119_sor_hdmi_ctrl`) but not AVI/VSI infoframe payloads
  (`gf119.c gf119_sor_hdmi_infoframe_avi/vsi`) — those need real EDID vendor
  data and a packed-infoframe helper (`hdmi.h pack_hdmi_infoframe`).
- **Cursor/overlay planes.** Classes exist in the gp102 user table (see §4.2)
  but no step models them; the NVRMFB-style framebuffer can start
  base-channel-only (flattened compositing), cursor later.
- **SCDC/HDMI 2.x.** Irrelevant at 148.5 MHz (< 340 MHz SCDC threshold);
  `gm200_sor_hdmi_scdc` exists for higher clocks.
- **VPLL actual programming.** `nv50_disp_super_2_1` calls
  `nvkm_devinit_pll_set(PLL_VPLL0 + head)` — the devinit PLL tables are not
  ported; the sim records only the target frequency (148500 kHz).
- **Hardware waits.** `nv50_sor_power` polls 0x61C004 busy bit and 0x61C030
  bit 28 (`nvkm_msec`); channel init polls 0x610490 bit 31. These need PTIMER
  — modeled as no-ops in the recorder, must be real in the kext.
- **HPD / connector table.** Which SOR is HDMI vs miniDP on the GP106M comes
  from the DCB in the VBIOS (`pascal/core/src/vbios_bit.c` BIT parser exists;
  DCB 4.x connector decode not done) — the model pins SOR 0 arbitrarily.
- **gf119_disp_init step list.** The capability-mirror and steal-from-VBIOS
  steps (§4.1) are cited but not yet planner-emitted; a future step should
  add a `pg106_disp_init_plan()`.

## 7. Verification

- `cc -std=c11 -Icore/include -o <bin> tests/test_disp_mode.c core/src/*.c` →
  3 PASS groups (`plan-shape-and-order`, `timing-roundtrip-cea1080p`,
  `mock-bus-and-diff-gate`), exit 0.
- `python3 helios300-gp106-simtest.py` stays green (5/5 PASS).
- `make test` was blocked at build time by an unrelated sibling compile error
  (`core/include/pascal/dispatch.h` / `dispatch.c`, in-progress concurrent
  work — not a disp_mode issue); verification was therefore run directly per
  the Phase 5B fallback. No Makefile change needed (wildcard build picked up
  the new test automatically).
