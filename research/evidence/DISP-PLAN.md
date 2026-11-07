# DISP-PLAN — GP106 display mode set (Phase 5B), as nouveau does it on GP102-class hardware

**Status: SIMULATION.** Everything here is a model run against a mock display
engine (`pascal/tests/test_disp_mode.c`) and a passive shared bus
(`pascal/tests/test_bringup.c` stage 5). No hardware was touched; a PASS is a
check that the model follows nouveau, not a device claim (HANDOFF-PASCAL.md §7).

Revised 2026-11-07. The previous revision of this document rested on a false
premise ("EVO core-channel methods ARE the PDISP head/SOR registers") and is
superseded in full; §7 lists what changed and why.

Target: GTX 1060 Mobile (GP106M), Acer Predator Helios 300 (2017). **Wiring:**
no MUX. The internal panel is on the Intel iGPU; **HDMI and miniDP are on the
dGPU**. So the Pascal display engine only drives external TMDS/HDMI and DP
heads.

Upstream: linux master, sparse clone `/tmp/review-ref/linux` (torvalds/linux
d48723711bae, 2026-11-06), `drivers/gpu/drm/nouveau/{nvkm/engine/disp,dispnv50}`.
NVIDIA class headers: `third_party/open-gpu-kernel-modules/src/common/sdk/
nvidia/inc/class/cl917d.h`. Code: `pascal/core/{include/pascal,src}/
disp_mode.*` (method streams) and `disp_core.*` (BAR0 model).

---

## 1. The structural fact (corrected)

GP106 uses `gp102_disp` (`nvkm/engine/disp/gp102.c:173-193`). Its core channel
is `GP102_DISP_CORE_CHANNEL_DMA`, which the DRM side drives with `core917d`
(`dispnv50/core.c:46-56`; head class `head917d`, SOR class `sor907d`).

That core channel is a **DMA channel**: `gp102_disp_core_init`
(`gp102.c:102-127`) writes the pushbuffer address word to `0x611494`, the
control word `0x01000013` to `0x610490` and the initial PUT to `0x640000`,
then waits for `0x610490` bit 31 to clear. Methods are only executed when the
channel **fetches** them from the pushbuffer after a PUT write.

`0x640000 + ctrl*0x1000` is the channel's USER page: `+0x0` is PUT
(`NV507C_PUT_PTR 11:2`, a byte offset), and the rest of the core page mirrors
**ARMED** state. `gf119_head_state` reads ARM at `0x640414 + head*0x300` and
**ASSEMBLY** (pending) state at `0x660414 + head*0x300` (`gf119.c:423-464`).
**A BAR0 write of a method offset into 0x640000+ executes nothing.** The model
asserts that the driver never writes into that page except at PUT.

## 2. Head timing (`dispnv50/head.c:283-326`, `drm_modes.c:1348-1408`)

`drm_mode_set_crtcinfo(CRTC_INTERLACE_HALVE_V)` first (it halves the vertical
values of an interlaced mode). Then:

```
h.active = crtc_htotal       h.synce = hsync_end - hsync_start - 1
h.blanke = hblank_end - hsync_start - 1     h.blanks = h.blanke + hdisplay
(v likewise)
progressive: v.blank2e = 0, v.blank2s = 1                       (head.c:318-321)
interlaced : v.blank2e = v.active + v.blanke, v.blank2s = v.blank2e + vdisplay,
             v.active = 2 * v.active + 1                        (head.c:313-317)
```

`head907d_mode` writes `h/v.active` to `HEAD_SET_RASTER_SIZE`
(`head907d.c:338-340`), so **RASTER_SIZE carries the TOTALS**. An earlier
"fix" that switched it to the active area was wrong and has been reverted;
the ARM readback `0x640414` decodes as vtotal/htotal for the same reason.

Golden values for CTA VIC 16, 1080p60 (148.5 MHz, 2200x1125, hsync
2008..2052, vsync 1084..1089), asserted in `test_disp_mode.c`:

| method | value | note |
|---|---|---|
| RASTER_SIZE (0x414) | `0x04650898` | 2200 \| 1125<<16 |
| RASTER_SYNC_END (0x418) | `0x0004002b` | 43, 4 |
| RASTER_BLANK_END (0x41c) | `0x002800bf` | 191, 40 |
| RASTER_BLANK_START (0x420) | `0x0460083f` | 2111, 1120 |
| RASTER_VERT_BLANK2 (0x424) | `0x00000001` | blank2s = 1 |
| PIXEL_CLOCK_FREQUENCY (0x450) | 148500000 | `m.clock * 1000` |

1080i (VIC 5) halves to 540/542/547/562, giving v.synce 4, v.blanke 19,
v.blanks 559, blank2e 581, blank2s 1121, v.active 1125.

## 3. The core channel method stream (`pg106_disp_push_core_modeset`)

Encoding: `PUSH_HDR` = `count << 18 | method` (`include/nvif/push507c.h`,
`NV507C_DMA_METHOD_COUNT 27:18`, `OFFSET 11:2`). Consecutive methods share one
header, as `PUSH_MTHD(...)` groups them. The order follows
`nv50_disp_atomic_commit_tail` (`disp.c:2168-2340`) for a head being enabled:

1. **Output path** (`nv50_sor_atomic_enable`, `disp.c:1743-1838`):
   `SOR_SET_CONTROL(sor) = proto << 8 | BIT(head)` (`nv50_sor_update`,
   `sor907d_ctrl`). TMDS sublink A is `SINGLE_TMDS_A`. Depth is
   `PIXEL_DEPTH_DEFAULT` (0) for TMDS (`disp.c:1762`); DP uses
   `nv50_dp_bpc_to_depth`.
2. **Head** (`nv50_head_flush_set`, `head.c:69-82`):
   - view: OUTPUT_SCALER 0; VIEWPORT_SIZE_IN; VIEWPORT_SIZE_OUT/_MIN/_MAX
     (scaler NONE and underscan OFF are the connector defaults,
     `nouveau_connector.c:264-269`, so in = out = the mode size);
   - mode: OVERSCAN_COLOR..VERT_BLANK2 (6 words), DEFAULT_BASE_COLOR,
     PIXEL_CLOCK_FREQUENCY/CONFIGURATION (CLK_CUSTOM)/FREQUENCY_MAX;
   - core_set with `head507d_core_calc`'s values (OFFSET, SIZE, STORAGE
     pitch = ALIGN(w,64)*4, PARAMS A8R8G8B8, CONTEXT_DMAS_ISO = the core
     channel's VRAM handle, VIEWPORT_POINT_IN);
   - BASE_CHANNEL_USAGE_BOUNDS = USABLE | BPP_32 | BASE_LUT USAGE_1025
     (`head917d_base`), OVERLAY_USAGE_BOUNDS = BPP_16 (no overlay);
   - DITHER_CONTROL: for 8 bpc on a 24-bit plane, mode AUTO yields 0 and
     depth AUTO yields 8BPC, so the word is `0x2` (dither disabled);
   - PROCAMP: vibrance 150, hue 90 gives sat.cos 1024, so the word is
     `0x00040000`;
   - `head907d_or`: OUTPUT_RESOURCE (polarity, depth) + HEAD_SET_CONTROL
     `0x31ec6000 | head << 25 | STRUCTURE`.
3. **UPDATE** (`core507d_update` with notifier): SET_NOTIFIER_CONTROL
   (write, notify), then `UPDATE = INTERLOCK_WITH_BASE<head>`
   (`0x2 << head*4`, `base917c.c:49`) + SET_NOTIFIER_CONTROL(disable).

**Base channel** (`pg106_disp_push_base_image`): `base907c_image_set`
(PRESENT_CONTROL non-tearing interval 1, CONTEXT_DMAS_ISO(0), SURFACE_SET_
OFFSET/SIZE/STORAGE/PARAMS) then `base507c_update` with INTERLOCK_WITH_CORE.
The base stream is kicked **before** the core stream (`commit_wndw` precedes
`commit_core`). That is the only reason the core UPDATE may carry a BASE
interlock; the model never emits a BASE interlock without a base stream.

**Disable** (`pg106_disp_push_core_disable`): SOR ctrl with the head bit
cleared, which becomes 0 once no owner remains (`disp.c:1545-1548`), then
`head907d_core_clr`, then UPDATE.

## 4. The BAR0 side (`disp_core.c`)

| stage | registers | upstream |
|---|---|---|
| engine init | caps mirror 0x616104/8/c → 0x6101b4/b8/bc per head, SOR caps 0x61c000 → 0x6301c4; VBIOS handoff 0x6100ac bit 8 → 0x6194e8 (poll bit 1); `0x610010 = inst>>8 \| 9`; 0x610090/0x6100a0 = 0, 0x6100b0 = 0x307; underflow mask 0x616308 | `gf119.c:1165-1231` |
| IOR power | per SOR, at **init only**: wait 0x61c004[31], mask 0x80000001, wait again, wait 0x61c030[28] | `base.c:139-142`, `nv50.c:202-228` |
| channels | core ctrl 0 (0x01000013), base ctrl 1 (0x13): 0x611494/8/c, 0x610490, PUT reset, busy poll | `gp102.c:33-60, 102-127` |
| acquire | route `0x612308 + macro*0x100 [4:0] = link<<4 \| (sor+1)` (and 0x612388 for sublink 2); the SOR is any free one (GM200 has a crossbar), the macro link comes from DCB `outp.or` | `gm200.c:98-114`, `outp.c:35-59` |
| HDMI | `gk104_sor_hdmi_ctrl`: 0x6900c0+head*0x400 bit 0 off, 0x6900cc = 0x10, bit 0 on, 0x690080 = 0x82000000, 0x616798 mask 0x401f007f; then `gm200_sor_hdmi_scdc` 0x61c5bc. max_ac_packet = (htotal − hdisplay − 56 − 18)/32 = 6, rekey 56 | `gk104.c:77-104`, `gm200.c:62-79`, `disp.c:777-850` |
| kick | VRAM flush 0x070000 = 1 (poll bit 1), PUT = dwords << 2 | `dispnv50/disp.c:133-156` |
| interrupt | 0x610088 bit 20 → 0x6100ac & 7 = pending, ack | `gf119.c:1128-1141` |
| supervisor 1 | per-head mask 0x6101d4; latch ARM/ASY head + SOR state from 0x640xxx/0x660xxx; 1.0 OffInt1 IED | `gf119.c:1011-1066`, `nv50.c:1333-1363` |
| supervisor 2 | 2.0 OffInt2 IED; 2.1 VPLL `nvkm_devinit_pll_set(PLL_VPLL0+head, khz)` (bus hook; `pg106_init_pll_set` runs the ported gf100 path); 2.2 OnInt2 IED, **rgclk 0x612200 = 0** (SORs never set `asy.rgdiv`; only `nv50_pior_state` does, `nv50.c:126`), DP config, `gf119_sor_clock` | `nv50.c:1263-1331` |
| supervisor 3 | 3.0 OnInt3 IED; ack 0x6101d4 = 0, 0x6101d0 = 0x80000000 | `nv50.c:1138-1155` |

**TMDS clock** (`gf119_sor_clock`, `gf119.c:286-302`): speed 0x14 when
`tmds.high_speed`, else 0x0a, at bits 22:18 of 0x612300+soff. `high_speed` is
set by `gm200_sor_hdmi_scdc` as **khz > 340000** (`gm200.c:70`). The earlier
165 MHz threshold was the gf119 dual-link rule, not GM200+. High speed also
forces **div2 = 1** (`gf119.c:297-298`).

## 5. DisplayPort / miniDP (model form)

`pg106_dp_train` follows `nvkm_dp_train` (`dp.c:487-527`). It records the DP
Info scripts as steps (EnableSpread/DisableSpread, BeforeLinkTraining,
AfterLinkTraining; nouveau runs them through `nvbios_init`), then:
`gf119_sor_dp_links` (0x612300 clksor bw<<18, 0x61c10c lanes/EF),
`g94_sor_dp_power` (0x61c130, 0x61c034 poll), LINK_BW_SET/LANE_COUNT_SET, the
CR loop (pattern 1 via `gm107_sor_dp_pattern` 0x61c110, drive via
`gm200_sor_dp_drive` 0x61c118/120/130/13c, sense 0x202/0x206) and the EQ loop
(TPS3 when DPCD ≥ 1.2 and supported; TPS4 is masked below chipset 0x110), then
pattern 0. The sink is simulated.

Supervisor 2.2 DP config (`nv50_disp_super_2_2_dp`, `nv50.c:1157-1261`;
gm200 has no `activesym`, so TU = 64): for 1080p60 on 2×HBR the model computes
audio_sym h 487 / v 1896 and watermark 15, matching hand derivation.

Not modelled: `nvbios_dpcfg_match` (VBIOS drive-level table; the model drives
the sink's requested levels), LTTPRs, eDP rate tables, MST.

## 6. VBIOS facts that feed this (DCB, `vbios_bit.c`)

`pg106_vbios_dcb_outp` / `pg106_vbios_conn_type` port `dcb_outp_parse` and
`nvbios_connEp`. On the committed desktop GP106 6 GB ROM, the DCB lists HDMI
(TMDS, OR 0x1, conn type 0x61) and three DP connectors (conn type 0x46), each
of which also has a TMDS twin for dual-mode DP. The Helios 300 DCB is still
unknown (owner dump needed). The model does not hard-code "HDMI = SOR 0":
the SOR is acquired, and the route comes from `outp.or`.

## 7. What changed vs the 2026-11-05 plan, and why

| old | new | upstream |
|---|---|---|
| methods written to BAR0 0x640000+ | core/base pushbuffers + PUT kick | `gp102.c:102-127`, `disp.c:133-156` |
| RASTER_SIZE = active area | RASTER_SIZE = totals | `head.c:298/303`, `head907d.c:338-340` |
| VERT_BLANK2 = 0 | blank2s = 1 (progressive), interlace math | `head.c:313-321` |
| HDMI via gf119 0x6167a4/0x6167ac | gk104 0x6900c0/cc + 0x690080 | `gp100.c:30-42` → `gm200_sor_hdmi` → `gk104_sor_hdmi_ctrl` |
| high speed > 165 MHz, no div2 | > 340 MHz, div2 = 1 | `gm200.c:70`, `gf119.c:293-300` |
| SOR power down/up per modeset | power only at disp init, with 0x61c004/0x61c030 waits | `base.c:139-142`, `nv50.c:202-228` |
| rgclk = 1 | rgclk = 0 (rgdiv unset for SORs) | `nv50.c:126, 1295` |
| depth BPP_24 for TMDS | PIXEL_DEPTH DEFAULT | `disp.c:1762` |
| UPDATE with BASE interlock, no base channel | base stream kicked first, then core UPDATE | `disp.c:2310-2320`, `base917c.c:49` |
| no init / supervisor | `gf119_disp_init` + interrupt-driven supervisor 1/2/3 | `gf119.c:1011-1231` |
| SOR 0 pinned | DCB lookup + GM200 macro-link route | `dcb.c:122-190`, `gm200.c:98-114` |
| no DP | DP training model | `dp.c` |

## 8. Open (each needs code or hardware, not just this model)

- **IED scripts.** `nv50_disp_super_ied_on/off` need `nvbios_outp_match` /
  `nvbios_ocfg_match` / `nvbios_oclk_match` (BIOS display table walk; not
  ported). The supervisor calls a hook, and nouveau itself just returns when
  the match fails.
- **HDMI infoframes.** AVI/VSI payloads (`gk104_sor_hdmi_infoframe_avi/vsi`,
  `pack_hdmi_infoframe`) are not emitted. HDA/ELD audio is not modelled.
- **LUTs, cursor, overlay.** `olut_set`/`curs_set` (`nv50_head_flush_set_
  wndw`) and the core/base notifier memory are not modelled.
- **Ctxdma objects.** The handles in CONTEXT_DMAS_ISO/NOTIFIER refer to
  objects in the display hash table at `0x610010`. Building that RAMHT and
  instance memory is not modelled.
- **DP drive levels** from `nvbios_dpcfg_match`; AUX transport itself (I2C/
  AUX subdev).
- **Real timing of waits** (PTIMER); the model counts polls.
- **Everything on silicon**: owner-run only.

## 9. Verification

- `pascal/tests/test_disp_mode.c`: head math (1080p, 1080i), method-stream
  goldens, a full HDMI modeset over the mock engine (init, channels, route,
  HDMI, kicks, supervisors, ARM readback, determinism), TMDS high speed, the
  DP model, super 2.2 DP math. All pass.
- `pascal/tests/test_bringup.c` stage 5 runs the same path on the shared bus
  and decodes the core stream back out of VRAM.
