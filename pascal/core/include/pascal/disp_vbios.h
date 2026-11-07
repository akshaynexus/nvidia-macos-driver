// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/disp_vbios.h — the VBIOS tables the display
// driver needs beyond vbios_bit.h: the DCB I2C (CCB) table, full connector
// entries (HPD), GPIO function lookup with polarity, and the BIT 'U'
// display table walk that selects IED scripts for supervisor 1/2/3.
//
// Pure parsing over a pg106_vbios_bit; nothing touches a GPU. Tested on
// the Helios 300 ROM (tests/test_disp_hw.c, skipped when the gitignored ROM
// is absent).
//
// Upstream (torvalds/linux d48723711bae, drivers/gpu/drm/nouveau/):
//   nvkm/subdev/bios/i2c.c:28-133   dcb_i2c_table / entry / parse
//   nvkm/subdev/bios/conn.c:28-98   nvbios_connTe / connEe / connEp
//   nvkm/engine/disp/conn.c:44-75   connector HPD bit -> GPIO function
//                                   { 07 08 51 52 5e 5f 60 }
//   nvkm/subdev/bios/gpio.c:28-150  dcb_gpio_table / parse / match
//   nvkm/subdev/gpio/base.c:96-109  nvkm_gpio_get (sense == log[1] & 1)
//   nvkm/subdev/bios/disp.c:28-174  nvbios_disp_table, outp/ocfg/oclk match
//   nvkm/engine/disp/nv50.c:1012-1106 nv50_disp_super_iedt / ied_on /
//                                   ied_off (script selection)
//   nvkm/subdev/bios/dcb.c:108-118  dcb_outp_hasht / hashm

#ifndef PASCAL_DISP_VBIOS_H
#define PASCAL_DISP_VBIOS_H

#include <pascal/vbios_bit.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// enum dcb_i2c_type (include/nvkm/subdev/bios/i2c.h).
#define PG106_DCB_I2C_NV04_BIT 0x00u
#define PG106_DCB_I2C_NV4E_BIT 0x04u
#define PG106_DCB_I2C_NVIO_BIT 0x05u
#define PG106_DCB_I2C_NVIO_AUX 0x06u
#define PG106_DCB_I2C_PMGR     0x80u
#define PG106_DCB_I2C_UNUSED   0xffu

struct pg106_dcb_i2c {
	uint8_t type;  // PG106_DCB_I2C_*
	uint8_t drive; // bit-bang port (0xff unused)
	uint8_t sense;
	uint8_t share; // hybrid pad index (0xff = none)
	uint8_t auxch; // DP AUX channel (0xff unused)
	uint8_t ver;   // CCB version
};

// dcb_i2c_parse. 0, or -ENOENT when idx is past the table / no table.
// CCB >= 0x42 is unsupported upstream (warning, no table): -ENOTSUP.
int pg106_dvb_i2c(const struct pg106_vbios_bit *vb, uint8_t idx,
		  struct pg106_dcb_i2c *out);

struct pg106_dcb_conn {
	uint8_t type;     // DCB_CONNECTOR_* (0x61 HDMI-A, 0x46 DP, ...)
	uint8_t location;
	uint8_t hpd;      // raw HPD bit mask (connEp)
	uint8_t dp, di, sr, lcdid;
	uint8_t hpd_func; // GPIO function for hpd (0xff = none), conn.c map
};

// nvbios_connEp + the conn.c HPD function mapping.
int pg106_dvb_conn(const struct pg106_vbios_bit *vb, uint8_t idx,
		   struct pg106_dcb_conn *out);

struct pg106_dcb_gpio {
	uint8_t line, func;
	uint8_t log[2]; // log[0] = off state, log[1] = on state (dir<<1|out)
	bool param;
	uint32_t entry; // image offset
};

// dcb_gpio_match on GPIO table 0. func/line 0xff = any. -ENOENT none.
int pg106_dvb_gpio_match(const struct pg106_vbios_bit *vb, uint8_t func,
			 uint8_t line, struct pg106_dcb_gpio *out);

// One ior/head context for an IED script, as nvbios_init receives it.
struct pg106_dvb_ied_ctx {
	uint16_t hasht, hashm_or; // DCB outp hasht, outp->info.or
	uint8_t link;             // DCB outp->info.link (sublink mask)
	int head;
	uint8_t proto_evo;        // ior->asy.proto_evo (SOR_SET_CONTROL proto)
	int ior_link;             // ior->asy.link (3 = dual)
	bool lvds24;              // LVDS with head depth 24 (never on HDMI)
};

// nv50_disp_super_iedt: the IEDT entry for (hasht, 0x100<<head |
// ffs(link)<<6 | or). Returns the entry offset (0 = missing) and the
// OffInt scripts in script[0..2].
uint32_t pg106_dvb_iedt(const struct pg106_vbios_bit *vb,
			const struct pg106_dvb_ied_ctx *c, uint8_t *hdr,
			uint8_t *cnt, uint8_t *len, uint16_t script[3]);

// The script nv50_disp_super_ied_on (on = true; id 0 = OnInt2, 1 = OnInt3,
// selected by khz through ocfg/oclk) or ied_off (on = false; id 1 =
// OffInt1, 2 = OffInt2) would run. *script = 0 when upstream returns
// early ("missing IEDT ..."); `why` names the missing step.
int pg106_dvb_ied_script(const struct pg106_vbios_bit *vb,
			 const struct pg106_dvb_ied_ctx *c, bool on, int id,
			 uint32_t khz, uint32_t *script, const char **why);

// dcb_outp_hasht / hashm (dcb.c:108-118).
uint16_t pg106_dvb_outp_hasht(const struct pg106_dcb_outp *o);
uint16_t pg106_dvb_outp_hashm(const struct pg106_dcb_outp *o);

// VPLL limits from the BIT 'C' PLL limits table (bios/pll.c:81-98
// pll_limits_table; the 0x40 / 0x50 entry layouts of nvbios_pll_parse,
// pll.c:352-381). type 0x80 = PLL_VPLL0 (+head). kHz. The output range
// of the gf100 VPLL is vco_min / max_p .. vco_max / min_p
// (gt215_pll_calc picks P = vco_max / freq, clamped). Helios 300 ROM:
// ver 0x40, ref 27 MHz, VCO 800..1620 MHz, P 1..31.
struct pg106_dvb_pll {
	uint8_t ver;
	uint32_t refclk_khz, vco_min_khz, vco_max_khz;
	uint8_t min_p, max_p;
};
int pg106_dvb_pll(const struct pg106_vbios_bit *vb, uint8_t type,
		  struct pg106_dvb_pll *out);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_DISP_VBIOS_H
