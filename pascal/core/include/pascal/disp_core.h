// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/disp_core.h — GP102-class display engine
// register model: engine init, DMA channel init + PUT kick, SOR macro-link
// route, HDMI control, interrupt-driven supervisor stages, and a DP link
// training model.
//
// SIMULATION ONLY: every function drives a caller-supplied bus and records
// a step trace. Nothing here has run on a GPU.
//
// Upstream (linux master, /tmp/review-ref/linux d48723711bae):
//   nvkm/engine/disp/gp102.c:33-60   gp102_disp_dmac_init
//   nvkm/engine/disp/gp102.c:102-127 gp102_disp_core_init: pushbuffer
//                                    0x611494, PUT 0x640000, ctrl 0x610490
//   nvkm/engine/disp/gp102.c:173-193 gp102_disp: gf119_disp_init /
//                                    gf119_disp_intr / gf119_disp_super,
//                                    SORs = gp100_sor_new
//   nvkm/engine/disp/gp100.c:30-42   gp100_sor: gm200 route, nv50 power,
//                                    gf119 clock, gm200 hdmi, gm200 dp
//   nvkm/engine/disp/gf119.c:1165-1231 gf119_disp_init
//   nvkm/engine/disp/gf119.c:1101-1155 gf119_disp_intr (supervisor part)
//   nvkm/engine/disp/gf119.c:1011-1066 gf119_disp_super
//   nvkm/engine/disp/gf119.c:286-302 gf119_sor_clock
//   nvkm/engine/disp/gf119.c:416-464 gf119_head_rgclk / gf119_head_state
//   nvkm/engine/disp/gf119.c:304-336 gf119_sor_state
//   nvkm/engine/disp/gk104.c:77-104  gk104_sor_hdmi_ctrl
//   nvkm/engine/disp/gm200.c:62-85   gm200_sor_hdmi_scdc (high_speed =
//                                    khz > 340000)
//   nvkm/engine/disp/gm200.c:98-114  gm200_sor_route_set (0x612308)
//   nvkm/engine/disp/gm200.c:31-54   gm200_sor_dp_drive
//   nvkm/engine/disp/nv50.c:202-228  nv50_sor_power + waits
//   nvkm/engine/disp/nv50.c:1138-1360 nv50_disp_super_1 .. 3_0
//   nvkm/engine/disp/base.c:130-145  nvkm_disp_init: init, then power
//                                    every IOR (normal, pu)
//   nvkm/engine/disp/dp.c:92-527     link training
//   dispnv50/disp.c:133-156          nv50_dmac_kick (0x070000 flush, PUT)
//
// Register facts this model is built on (and the reason the old
// "EVO methods are BAR0 registers" plan was wrong): on GP102 the core
// channel is a DMA channel; methods are fetched from the pushbuffer whose
// address is written to 0x611494. 0x640000 + ctrl*0x1000 is the channel's
// USER area: +0x0 PUT (NV507C_PUT_PTR 11:2), and the rest of the core page
// mirrors ARMED state — gf119_head_state reads ARM at 0x640414 + head*0x300
// and ASY (assembly, pending) at 0x660414 + head*0x300 (gf119.c:426).
// Writing a method offset into 0x640000+ does not execute it.

#ifndef PASCAL_DISP_CORE_H
#define PASCAL_DISP_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- registers --------------------------------------------------------------

#define PG106_DISP_CHAN_PUSH(ctrl)  (0x611494u + (ctrl) * 0x10u)
#define PG106_DISP_CHAN_UNK498(ctrl) (0x611498u + (ctrl) * 0x10u)
#define PG106_DISP_CHAN_UNK49C(ctrl) (0x61149cu + (ctrl) * 0x10u)
#define PG106_DISP_CHAN_CTRL(ctrl)  (0x610490u + (ctrl) * 0x10u)
#define PG106_DISP_CHAN_PUT(user)   (0x640000u + (user) * 0x1000u)
#define PG106_DISP_ARM_BASE         0x640000u // ARMED-state mirror (core)
#define PG106_DISP_ASY_BASE         0x660000u // ASSEMBLY-state mirror
#define PG106_DISP_INTR             0x610088u
#define PG106_DISP_SUPER_STAT       0x6100acu // also the VBIOS-owner bit
#define PG106_DISP_SUPER_HEAD(h)    (0x6101d4u + (h) * 0x800u)
#define PG106_DISP_SUPER_ACK        0x6101d0u
#define PG106_DISP_HEAD_RGCLK(h)    (0x612200u + (h) * 0x800u)
#define PG106_DISP_SOR_CLOCK(s)     (0x612300u + (s) * 0x800u)
#define PG106_DISP_SOR_ROUTE(m)     (0x612308u + (m) * 0x100u) // m = macro link
#define PG106_DISP_SOR_PWR(s)       (0x61c004u + (s) * 0x800u)
#define PG106_DISP_SOR_PWR_STAT(s)  (0x61c030u + (s) * 0x800u)
#define PG106_DISP_HDMI_CTRL(h)     (0x616798u + (h) * 0x800u)
#define PG106_DISP_SOR_SCDC(s)      (0x61c5bcu + (s) * 0x800u)
#define PG106_DISP_HDMI_AVI(h)      (0x690000u + (h) * 0x400u)
#define PG106_DISP_HDMI_GCP(h)      (0x6900c0u + (h) * 0x400u)
#define PG106_DISP_HDMI_GCP_DATA(h) (0x6900ccu + (h) * 0x400u)
#define PG106_DISP_HDMI_UNK080(h)   (0x690080u + (h) * 0x400u)
#define PG106_DISP_HDMI_VSI(h)      (0x690100u + (h) * 0x400u)

// Core = ctrl/user 0, base(head) = ctrl/user 1 + ... (gp102.c:94-100,
// 139-145: base uses ctrl 1 user 1 for head 0; nvkm adds head index).
#define PG106_DISP_CHID_CORE 0u
#define PG106_DISP_CHID_BASE(h) (1u + (h))

// --- trace --------------------------------------------------------------------

enum pg106_dreg_kind {
	PG106_DREG_WR = 0, // nvkm_wr32
	PG106_DREG_MASK,   // nvkm_mask: val = new value, mask = mask
	PG106_DREG_RD,     // nvkm_rd32: val = value read
	PG106_DREG_POLL,   // nvkm_msec wait: val = polls, mask = 1 if met
	PG106_DREG_BLOCK,  // non-MMIO step (VPLL, IED/DP-Info script): see name
	PG106_DREG_AUX_WR, // DPCD write (addr = DPCD address, val = byte)
	PG106_DREG_AUX_RD, // DPCD read
};

enum pg106_dphase {
	PG106_DPH_INIT = 0,   // gf119_disp_init + IOR power
	PG106_DPH_CHAN,       // gp102 core/dmac init
	PG106_DPH_ACQUIRE,    // outp acquire: route set
	PG106_DPH_HDMI,       // nvif_outp_hdmi
	PG106_DPH_DP,         // DP link training
	PG106_DPH_KICK,       // nv50_dmac_kick
	PG106_DPH_INTR,       // gf119_disp_intr
	PG106_DPH_SUPER1,
	PG106_DPH_SUPER2,
	PG106_DPH_SUPER3,
};

struct pg106_dreg {
	uint8_t kind;
	uint8_t phase;
	uint32_t addr;
	uint32_t mask;
	uint32_t val;
	const char *name; // BLOCK steps / citations; may be NULL for MMIO
};

struct pg106_dbus {
	uint32_t (*rd32)(void *user, uint32_t addr);
	void (*wr32)(void *user, uint32_t addr, uint32_t val);
	// Optional: VPLL programming for supervisor 2.1
	// (nvkm_devinit_pll_set(PLL_VPLL0 + head, khz), nv50.c:1308-1316).
	// NULL = recorded as a BLOCK step only.
	int (*pll_set)(void *user, uint32_t type, uint32_t khz);
	// Optional: run an IED script (nv50_disp_super_ied_on/off): `id`
	// = 0 OnInt2/OffInt2-style index as in nv50.c. NULL = BLOCK step
	// "missing IEDT" (the IED table walk is not modelled; nouveau too
	// just returns when nvbios_outp_match fails).
	void (*ied)(void *user, int on, int id, int head, int sor, uint32_t khz);
	void *user;
};

struct pg106_dtrace {
	struct pg106_dreg *rec;
	size_t cap, len;
	bool overflow;
	uint8_t phase;
};

// --- engine init ----------------------------------------------------------------

struct pg106_disp_init_cfg {
	uint32_t inst_addr;  // disp->inst->addr (hash table/objects), 256-aligned
	unsigned heads_mask; // from 0x612004 & 0xf if 0 (gf119_head_cnt)
	unsigned sors_mask;  // from (0x612004 & 0xff00) >> 8 if 0
};

// gf119_disp_init (gf119.c:1165-1231) + nvkm_disp_init's IOR power loop
// (base.c:139-142 -> nv50_sor_power(normal, pu), nv50.c:212-228).
// Returns 0, or -EBUSY when the VBIOS-ownership handoff (0x6194e8 bit 1)
// never clears.
int pg106_disp_init(const struct pg106_dbus *bus,
		    const struct pg106_disp_init_cfg *cfg,
		    struct pg106_dtrace *t);

// gp102_disp_core_init (ctrl 0, final ctrl 0x01000013) /
// gp102_disp_dmac_init (ctrl n, 0x00000013). `push` is chan->push:
// (pushbuffer address >> 8) | target (1 VRAM, 2 NCOH, 3 HOST)
// (nv50.c:684-703). -EBUSY when 0x610490 bit 31 stays set.
int pg106_disp_chan_init(const struct pg106_dbus *bus, unsigned ctrl,
			 unsigned user, uint32_t push, uint32_t suspend_put,
			 bool core, struct pg106_dtrace *t);

// nv50_dmac_kick (dispnv50/disp.c:133-156): VRAM flush (0x070000 = 1,
// wait bit 1 clear) when the pushbuffer is in VRAM, then PUT =
// cur_dwords << 2 (NV507C_PUT_PTR 11:2).
void pg106_disp_kick(const struct pg106_dbus *bus, unsigned user,
		     uint32_t cur_dwords, bool vram, struct pg106_dtrace *t);

// gm200_sor_route_set (gm200.c:98-114). or_mask = DCB outp OR mask (the
// macro link), sublink = DCB sorconf.link, sor = SOR id or -1 (release),
// asy_link = SOR asy.link.
void pg106_disp_sor_route(const struct pg106_dbus *bus, uint8_t or_mask,
			  int sublink, int sor, int asy_link,
			  struct pg106_dtrace *t);

// nvkm_uoutp_mthd_hdmi enable path (uoutp.c:243-273): gk104_sor_hdmi_ctrl
// then gm200_sor_hdmi_scdc. Returns tmds.high_speed (khz > 340000).
bool pg106_disp_hdmi_enable(const struct pg106_dbus *bus, int head, int sor,
			    uint8_t max_ac_packet, uint8_t rekey,
			    uint32_t khz, bool scdc, bool scrambling,
			    bool scrambling_low_rates,
			    struct pg106_dtrace *t);

// gk104_sor_hdmi_ctrl disable path (gk104.c:86-92).
void pg106_disp_hdmi_disable(const struct pg106_dbus *bus, int head,
			     struct pg106_dtrace *t);

// --- interrupt + supervisor --------------------------------------------------

// gf119_disp_intr supervisor branch (gf119.c:1128-1141): returns the
// pending mask (bits 0..2 = supervisor 1/2/3), acks 0x6100ac.
uint32_t pg106_disp_intr(const struct pg106_dbus *bus, struct pg106_dtrace *t);

// Per-IOR facts the supervisor needs that nvkm keeps in software
// (ior->asy.outp, tmds.high_speed, dp link config).
struct pg106_disp_sor_sw {
	int sor;            // SOR id this head is routed to (-1 = none)
	bool high_speed;    // from pg106_disp_hdmi_enable
	// DP (proto DP): ior->dp.{bw,nr,ef}
	uint8_t dp_bw, dp_nr;
	bool dp_ef;
	bool outp_attached; // ior->asy.outp != NULL
};

// What supervisor 1 decoded (gf119_head_state / gf119_sor_state).
struct pg106_disp_head_state {
	uint32_t htotal, vtotal, hsynce, vsynce, hblanke, vblanke;
	uint32_t hblanks, vblanks, hz;
	uint32_t depth;
};
struct pg106_disp_sor_state {
	uint32_t proto_evo;
	int proto; // 0 LVDS, 1 TMDS, 2 DP, -1 unknown
	int link;
	uint32_t head;
};

struct pg106_disp_super_result {
	uint32_t mask[4];
	struct pg106_disp_head_state asy[4], arm[4];
	struct pg106_disp_sor_state sor_asy[8], sor_arm[8];
	uint32_t dp_h, dp_v, dp_watermark; // nv50_disp_super_2_2_dp output
};

// gf119_disp_super (gf119.c:1011-1066) for `pending`, with the
// nv50_disp_super_* stages. heads_mask/sors_mask as in init. sw[sor]
// carries nvkm's software IOR state.
void pg106_disp_super(const struct pg106_dbus *bus, uint32_t pending,
		      unsigned heads_mask, unsigned sors_mask,
		      const struct pg106_disp_sor_sw *sw,
		      struct pg106_disp_super_result *res,
		      struct pg106_dtrace *t);

// gf119_sor_clock (gf119.c:286-302) on its own (also used by DP links).
void pg106_disp_sor_clock(const struct pg106_dbus *bus, int sor,
			  bool tmds, int link, bool high_speed,
			  struct pg106_dtrace *t);

// --- DP link training (MODEL) -------------------------------------------------

// A simulated DP sink: DPCD bytes plus a policy deciding when clock
// recovery / channel EQ lock. SIMULATION: real sinks are not this simple.
struct pg106_dp_sink {
	uint8_t dpcd[0x700];
	unsigned cr_after;   // CR_DONE once this many drive attempts passed
	unsigned eq_after;   // EQ/SYMBOL/ALIGN once this many EQ polls passed
	unsigned drive_writes, eq_polls;
	int fail_aux;        // nonzero: every AUX transfer fails
};

struct pg106_dp_link {
	int sor, link;       // SOR id, asy.link (1 = A)
	uint8_t bw, nr;      // link bw code (0x06/0x0a/0x14), lanes
	bool ef;
};

// nvkm_dp_train (dp.c:487-527) without the DP-Info scripts (recorded as
// BLOCK steps; nouveau runs them through nvbios_init — devinit.c can
// execute them given an IEDT/DP-table walk, which is not modelled):
// train_init scripts -> gf119_sor_dp_links -> g94_sor_dp_power ->
// LINK_BW_SET/LANE_COUNT_SET -> CR loop -> EQ loop -> pattern 0 ->
// train_fini script. Drive levels come straight from the sink's adjust
// requests (nvbios_dpcfg_match is not modelled: drive() programs the
// lane's request as pc/dc/pe = 0/vs/pe). Returns 0 trained, -1 failed.
int pg106_dp_train(const struct pg106_dbus *bus, struct pg106_dp_sink *sink,
		   const struct pg106_dp_link *l, struct pg106_dtrace *t);

const char *pg106_dphase_name(uint8_t phase);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_DISP_CORE_H
