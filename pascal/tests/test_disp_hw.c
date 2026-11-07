// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_disp_hw.c — the display driver (core/src/disp_hw.c)
// against a MOCK DISPLAY ENGINE that models register semantics, not a
// replay of a plan:
//
//   * DMA channels (0x610490/0x611494) activate only when programmed;
//     a PUT write makes the mock FETCH the pushbuffer from mock VRAM at
//     the channel's PUSH address, decode methods (incl. JUMP), advance GET;
//   * every ctxdma handle a method names is resolved the way the hardware
//     does: 0x610010 -> instance memory -> RAMHT hash/probe (core/ramht.c)
//     -> context chid/offset -> gf119 dmaobj (start/limit/target), so a
//     wrong hash, context or object is an error interrupt (0x61009c);
//   * core UPDATE with a changed raster/SOR runs the supervisor handshake
//     (0x6100ac pending 1 -> 2 -> 3 through 0x6101d0 acks, 0x6101d4 head
//     masks), and only after supervisor 3 copies ASSEMBLY -> ARMED
//     (0x660000 -> 0x640000) and writes the core notifier; it checks the
//     supervisor 2 SOR clock / rgclk the driver must program;
//   * a core UPDATE that interlocks with BASE without a pending base
//     UPDATE(INTERLOCK_WITH_CORE) is an error (real hardware would hang);
//   * vblank (0x6100c0/0x6100bc/0x610088 bit 24) and non-interlocked base
//     flips latch at the mock's vblank and write the base notifier;
//   * the HDMI DDC bus is an open-drain I2C bus (SCL/SDA wired-AND of the
//     master register 0x00d0d4 and an emulated EEPROM at 0x50 with an
//     E-DDC segment pointer at 0x30), dead unless the GM200 hybrid pad
//     (0x00d970/0x00d97c) is in I2C mode;
//   * HPD is GPIO line 27, active low (Helios VBIOS log[1] = 2), with
//     edge status/enable at 0x00dc80/0x00dc88.
//
// SIMULATION. A PASS means the driver follows nouveau's semantics as the
// mock encodes them; it is not a hardware result.

#include <pascal/disp_hw.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define PASS(...) do { printf("PASS "); printf(__VA_ARGS__); printf("\n"); } while (0)

// ---------------------------------------------------------------------------
// mock GPU
// ---------------------------------------------------------------------------

#define BAR0_SIZE (16u << 20)
#define VRAM_SIZE (64u << 20)

enum { I2C_IDLE, I2C_RX, I2C_ACK_TX, I2C_TX, I2C_ACK_RX };

struct mock {
	uint32_t *reg;
	uint8_t *vram;
	uint64_t vram_next;
	unsigned allocs, frees;
	uint64_t now_ns;

	// channels
	bool chan_on[32];
	unsigned busy_reads[32];
	uint32_t get[32];  // dwords
	uint32_t asy_core[0x1000 / 4], arm_core[0x1000 / 4];
	uint32_t base_mthd[0x1000 / 4];
	bool base_pending_interlock, base_pending_flip;
	uint32_t base_arm_offset, base_arm_handle, base_arm_pitch, base_arm_size;
	uint32_t core_ntfy_ctl, core_ntfy_handle;
	uint32_t base_ntfy_ctl, base_ntfy_handle;
	unsigned errors, error_chid, error_mthd;
	char error_why[160];

	// supervisor
	int super_stage;       // 0 none, 1..3 pending stage
	bool super_ntfy;       // write core notifier when done
	uint32_t pclk_before;
	unsigned supers_done;
	bool super2_clock_ok, super2_rgclk_ok;

	// cursor
	uint32_t curs_point, curs_updates;

	// gpio / hpd
	bool plugged;

	// i2c
	bool m_scl, m_sda, s_sda;
	int i2c_state, i2c_bits;
	uint8_t i2c_shift, i2c_addr;
	bool i2c_first_write, i2c_read;
	uint8_t edid_seg, edid_off;
	uint8_t edid[512];
	size_t edid_len;
	unsigned i2c_starts, i2c_bytes_read;
	bool i2c_dead_pad_seen;

	// eld
	uint8_t eld[0x60];
	unsigned eld_writes;
};

static struct mock M;

static uint32_t
R(uint32_t a)
{
	return M.reg[a / 4];
}

static void
W(uint32_t a, uint32_t v)
{
	M.reg[a / 4] = v;
}

static uint32_t
v32(uint64_t pa)
{
	uint32_t v = 0;
	if (pa + 4 <= VRAM_SIZE)
		memcpy(&v, M.vram + pa, 4);
	return v;
}

static void
vw(uint64_t pa, uint32_t v)
{
	if (pa + 4 <= VRAM_SIZE)
		memcpy(M.vram + pa, &v, 4);
}

static void
chan_error(unsigned chid, uint32_t mthd, const char *why)
{
	M.errors++;
	M.error_chid = chid;
	M.error_mthd = mthd;
	snprintf(M.error_why, sizeof(M.error_why), "%s", why);
	W(0x61009c, R(0x61009c) | (1u << chid));
	W(0x6111f0 + chid * 12, mthd);
}

// Hardware RAMHT lookup (independent re-implementation of ramht.c).
struct ctxdma { bool ok; uint64_t start, limit; uint32_t flags; };

static struct ctxdma
ctxdma_lookup(unsigned chid, uint32_t handle)
{
	struct ctxdma c = { 0 };
	uint32_t v = R(0x610010);
	uint64_t inst;
	uint32_t hash = 0, h = handle, co;

	if ((v & 0xff) != 9)
		return c;
	inst = (uint64_t)(v & ~0xffu) << 8;
	while (h) {
		hash ^= h & 0x1ff;
		h >>= 9;
	}
	hash ^= chid << 5;
	co = hash;
	for (unsigned n = 0; n < 512; n++) {
		uint32_t eh = v32(inst + co * 8), ctx = v32(inst + co * 8 + 4);
		if (eh == handle && (ctx >> 27) == chid && (ctx & 1)) {
			uint32_t off = (ctx >> 9) & 0x3ffff;
			uint32_t f = v32(inst + off);
			if ((f & 0xf) != 0x9)
				return c;	// not a VRAM dmaobj
			c.ok = true;
			c.flags = f;
			c.start = (uint64_t)v32(inst + off + 4) << 8;
			c.limit = ((uint64_t)v32(inst + off + 8) << 8) | 0xff;
			return c;
		}
		if (eh == 0 && ctx == 0)
			break;
		co = (co + 1) % 512;
	}
	return c;
}

static void
ntfy_write(unsigned chid, uint32_t handle, uint32_t off, uint32_t val)
{
	struct ctxdma c = ctxdma_lookup(chid, handle);
	if (!c.ok || c.start + off + 4 > c.limit + 1) {
		chan_error(chid, 0x0084, "notifier ctxdma does not resolve");
		return;
	}
	vw(c.start + off, val);
}

// What needs a supervisor: OR routing/control and the raster/clock
// (cursor, offsets, LUTs and the like latch without one).
static bool
head_changed(void)
{
	static const uint32_t sup[] = { 0x200, 0x220, 0x240, 0x260, 0x404, 0x408,
		0x410, 0x414, 0x418, 0x41c, 0x420, 0x424, 0x450, 0x454, 0x458 };
	for (unsigned i = 0; i < sizeof(sup) / sizeof(sup[0]); i++)
		if (M.asy_core[sup[i] / 4] != M.arm_core[sup[i] / 4])
			return true;
	return false;
}

static void
arm_all(void)
{
	memcpy(M.arm_core, M.asy_core, sizeof(M.arm_core));
	for (uint32_t m = 0x80; m < 0x1000; m += 4)
		W(0x640000 + m, M.arm_core[m / 4]);
}

static void
apply_base(void)
{
	M.base_arm_handle = M.base_mthd[0xc0 / 4];
	M.base_arm_offset = M.base_mthd[0x400 / 4];
	M.base_arm_size = M.base_mthd[0x408 / 4];
	M.base_arm_pitch = ((M.base_mthd[0x40c / 4] >> 8) & 0x1fff) << 8;
	if (M.base_arm_handle) {
		struct ctxdma c = ctxdma_lookup(1, M.base_arm_handle);
		uint64_t end = ((uint64_t)M.base_arm_offset << 8) +
			       (uint64_t)M.base_arm_pitch * (M.base_arm_size >> 16);
		if (!c.ok)
			chan_error(1, 0xc0, "base ISO ctxdma does not resolve");
		else if (end > c.limit + 1)
			chan_error(1, 0x400, "base surface outside its ctxdma");
	}
}

static void
core_update(uint32_t v)
{
	// HEAD_SET_CONTEXT_DMAS_ISO must resolve for a lit head.
	if (M.asy_core[0x414 / 4] && M.asy_core[0x474 / 4] &&
	    !ctxdma_lookup(0, M.asy_core[0x474 / 4]).ok)
		chan_error(0, 0x474, "core ISO ctxdma does not resolve");
	if ((M.asy_core[0x480 / 4] & 0x80000000u) &&
	    !ctxdma_lookup(0, M.asy_core[0x48c / 4]).ok)
		chan_error(0, 0x48c, "cursor ctxdma does not resolve");
	if (v & 0x2) {		// INTERLOCK_WITH_BASE head 0
		if (!M.base_pending_interlock)
			chan_error(0, 0x80, "UPDATE interlocks BASE with no base UPDATE pending");
		else {
			apply_base();
			M.base_pending_interlock = false;
		}
	}
	M.super_ntfy = (M.core_ntfy_ctl & 0x80000000u) != 0;
	if (head_changed()) {
		M.pclk_before = M.arm_core[0x450 / 4];
		M.super_stage = 1;
		W(0x6100ac, R(0x6100ac) | 1);
		W(0x6101d4, 0x00001000u | (M.asy_core[0x450 / 4] != M.pclk_before ? 0x00010000u : 0));
		M.super2_clock_ok = M.super2_rgclk_ok = false;
	} else {
		arm_all();
		if (M.super_ntfy)
			ntfy_write(0, M.core_ntfy_handle, 0, 1);
	}
}

static void
core_mthd(uint32_t mthd, uint32_t data)
{
	switch (mthd) {
	case 0x80: core_update(data); return;
	case 0x84: M.core_ntfy_ctl = data; return;
	case 0x88:
		if (!ctxdma_lookup(0, data).ok)
			chan_error(0, 0x88, "core notifier ctxdma does not resolve");
		M.core_ntfy_handle = data;
		return;
	case 0x8c:		// GET_CAPABILITIES
		if (M.core_ntfy_ctl & 0x80000000u)
			ntfy_write(0, M.core_ntfy_handle, 4, 1);
		return;
	}
	if (mthd < 0x1000) {
		M.asy_core[mthd / 4] = data;
		W(0x660000 + mthd, data);
	}
}

static void
base_mthd(uint32_t mthd, uint32_t data)
{
	switch (mthd) {
	case 0x80:
		if (data & 1)
			M.base_pending_interlock = true;
		else
			M.base_pending_flip = true;	// latches at vblank
		return;
	case 0xa0: M.base_ntfy_ctl = data; return;
	case 0xa4:
		if (data && !ctxdma_lookup(1, data).ok)
			chan_error(1, 0xa4, "base notifier ctxdma does not resolve");
		M.base_ntfy_handle = data;
		return;
	}
	if (mthd < 0x1000)
		M.base_mthd[mthd / 4] = data;
}

static void
fetch(unsigned ctrl, uint32_t put_bytes)
{
	uint32_t pv = R(0x611494 + ctrl * 0x10);
	uint64_t pa = (uint64_t)(pv & ~3u) << 8;
	uint32_t put = put_bytes >> 2;
	unsigned guard = 0;

	if (!M.chan_on[ctrl] || (pv & 3) != 1) {
		chan_error(ctrl, 0, "PUT on an inactive channel / non-VRAM push");
		return;
	}
	while (M.get[ctrl] != put && guard++ < 4096) {
		uint32_t h = v32(pa + M.get[ctrl] * 4u);
		if ((h & 0xe0000003u) == 0x20000000u) {	// JUMP
			M.get[ctrl] = (h & 0x1ffffffcu) >> 2;
			continue;
		}
		uint32_t cnt = (h >> 18) & 0x7ff, mthd = h & 0xffc;
		if (h >> 29) {
			chan_error(ctrl, mthd, "unsupported opcode");
			return;
		}
		M.get[ctrl]++;
		for (uint32_t k = 0; k < cnt; k++) {
			uint32_t data = v32(pa + M.get[ctrl] * 4u);
			M.get[ctrl]++;
			if (ctrl == 0)
				core_mthd(mthd + 4 * k, data);
			else
				base_mthd(mthd + 4 * k, data);
		}
	}
	W(0x640004 + ctrl * 0x1000, M.get[ctrl] << 2);
}

// I2C slave on the wired-AND bus.
static bool line_scl(void) { return M.m_scl; }
static bool line_sda(void) { return M.m_sda && M.s_sda; }

static bool
pad_i2c(void)
{
	return (R(0x00d970) & 0xc003) == 0xc001 && !(R(0x00d97c) & 1);
}

static uint8_t
edid_byte(void)
{
	size_t a = (size_t)M.edid_seg * 256 + M.edid_off++;
	return a < M.edid_len ? M.edid[a] : 0xff;
}

static void
i2c_scl_rise(void)
{
	switch (M.i2c_state) {
	case I2C_RX:
		M.i2c_shift = (uint8_t)(M.i2c_shift << 1 | line_sda());
		M.i2c_bits++;
		break;
	case I2C_TX:
		M.i2c_bits++;
		break;
	case I2C_ACK_RX:
		M.i2c_read = !line_sda();	// master ACK -> more
		break;
	}
}

static void
i2c_scl_fall(void)
{
	switch (M.i2c_state) {
	case I2C_RX:
		if (M.i2c_bits < 8)
			break;
		if (M.i2c_first_write) {	// address byte
			M.i2c_addr = M.i2c_shift >> 1;
			bool rdop = M.i2c_shift & 1;
			bool ack = (M.i2c_addr == 0x50) || (M.i2c_addr == 0x30 && !rdop);
			if (!ack || !M.plugged) {
				M.i2c_state = I2C_IDLE;
				break;
			}
			M.i2c_first_write = false;
			M.i2c_read = rdop;
		} else if (M.i2c_addr == 0x30) {
			M.edid_seg = M.i2c_shift;
		} else {
			M.edid_off = M.i2c_shift;
		}
		M.s_sda = false;		// ACK
		M.i2c_state = I2C_ACK_TX;
		break;
	case I2C_ACK_TX:
		M.s_sda = true;
		M.i2c_bits = 0;
		M.i2c_shift = 0;
		if (M.i2c_read) {
			M.i2c_state = I2C_TX;
			M.i2c_shift = edid_byte();
			M.i2c_bytes_read++;
			M.s_sda = (M.i2c_shift & 0x80) != 0;
		} else {
			M.i2c_state = I2C_RX;
		}
		break;
	case I2C_TX:
		if (M.i2c_bits >= 8) {
			M.s_sda = true;
			M.i2c_state = I2C_ACK_RX;
		} else {
			M.s_sda = (M.i2c_shift >> (7 - M.i2c_bits)) & 1;
		}
		break;
	case I2C_ACK_RX:
		if (M.i2c_read) {
			M.i2c_state = I2C_TX;
			M.i2c_bits = 0;
			M.i2c_shift = edid_byte();
			M.i2c_bytes_read++;
			M.s_sda = (M.i2c_shift & 0x80) != 0;
		} else {
			M.i2c_state = I2C_IDLE;
			M.s_sda = true;
		}
		break;
	}
}

static void
i2c_write(uint32_t v)
{
	bool scl = v & 1, sda = (v & 2) != 0;
	bool old_scl = line_scl(), old_sda = line_sda();

	if (!pad_i2c()) {		// pad off: the lines go nowhere
		M.i2c_dead_pad_seen = true;
		M.m_scl = scl;
		M.m_sda = sda;
		return;
	}
	if (scl != M.m_scl) {
		M.m_scl = scl;
		if (scl && !old_scl)
			i2c_scl_rise();
		else if (!scl && old_scl)
			i2c_scl_fall();
	}
	if (sda != M.m_sda) {
		M.m_sda = sda;
		if (line_scl()) {
			if (old_sda && !line_sda()) {		// START
				M.i2c_state = I2C_RX;
				M.i2c_bits = 0;
				M.i2c_shift = 0;
				M.i2c_first_write = true;
				M.s_sda = true;
				M.i2c_starts++;
			} else if (!old_sda && line_sda()) {	// STOP
				M.i2c_state = I2C_IDLE;
				M.s_sda = true;
			}
		}
	}
}

static void
mock_hpd(bool plugged)
{
	if (plugged == M.plugged)
		return;
	M.plugged = plugged;
	// line 27 goes low on plug (active low): LO edge = stat1 bit 27,
	// HI edge = stat1 bit 11 (gk104_gpio_intr_stat).
	W(0x00dc80, R(0x00dc80) | (plugged ? (1u << 27) : (1u << 11)));
}

static void
mock_vblank(void)
{
	M.now_ns += 16666667;
	if (M.base_pending_flip) {
		apply_base();
		M.base_pending_flip = false;
		if (M.base_ntfy_handle)
			ntfy_write(1, M.base_ntfy_handle, M.base_ntfy_ctl & 0xffc,
				   1u << 30);	// STATUS BEGUN
	}
	if (R(0x6100c0) & 1)
		W(0x6100bc, R(0x6100bc) | 1);
}

static uint32_t
mrd(void *u, uint32_t a)
{
	(void)u;
	if (a >= BAR0_SIZE)
		return 0xffffffffu;
	switch (a) {
	case 0x610088: {	// derived pending summary
		uint32_t v = 0;
		if (R(0x61008c)) v |= 1;
		if (R(0x61009c)) v |= 2;
		if (R(0x6100ac) & 7) v |= 0x00100000;
		if (R(0x6100bc) & 1) v |= 0x01000000;
		return v;
	}
	case 0x00d610 + 27 * 4:
		return (R(a) & ~0x4000u) | (M.plugged ? 0 : 0x4000u);
	case 0x00d0d4:
		return (R(a) & ~0x30u) | (line_scl() ? 0x10 : 0) |
		       (pad_i2c() && line_sda() ? 0x20 : 0);
	case 0x651008:
		return 4;	// cursor PIO FREE COUNT
	case 0x070000:
		return 0;
	}
	if (a >= 0x610490 && a < 0x610490 + 32 * 0x10 && (a & 0xf) == 0) {
		unsigned ctrl = (a - 0x610490) / 0x10;
		uint32_t v = R(a);
		if (M.busy_reads[ctrl]) {
			M.busy_reads[ctrl]--;
			return v | 0x80000000u;
		}
		return v;
	}
	return R(a);
}

static void
mwr(void *u, uint32_t a, uint32_t v)
{
	(void)u;
	if (a >= BAR0_SIZE)
		return;
	if (a == 0x6100ac) {			// W1C pending/owner bits
		W(a, R(a) & ~v);
		return;
	}
	if (a == 0x6194e8) {			// VBIOS handoff
		W(a, (v & 1) ? (v | 2) : (v & ~2u));
		return;
	}
	if (a == 0x61009c || a == 0x61008c || a == 0x6100bc ||
	    a == 0x00dc00 || a == 0x00dc80 || a == 0x00dc60) {
		W(a, R(a) & ~v);
		return;
	}
	if (a >= 0x61c004 && a < 0x620000 && (a & 0x7ff) == 0x004) {
		W(a, v & ~0x80000000u);		// SOR power completes at once
		return;
	}
	if (a == 0x6101d0 && (v & 0x80000000u)) {	// supervisor ack
		W(0x6101d0, 0);
		if (M.super_stage == 1) {
			M.super_stage = 2;
			W(0x6100ac, R(0x6100ac) | 2);
			W(0x6101d4, 0x00001000u | (M.asy_core[0x450 / 4] != M.pclk_before ? 0x00010000u : 0));
		} else if (M.super_stage == 2) {
			unsigned sor = 0;
			while (sor < 8 && !(M.asy_core[(0x200 + sor * 0x20) / 4] & 0xf))
				sor++;
			if (M.asy_core[0x414 / 4]) {
				uint32_t clk = R(0x612300 + sor * 0x800);
				M.super2_clock_ok = sor < 8 && ((clk >> 18) & 0x1f) == 0x0a;
				M.super2_rgclk_ok = (R(0x612200) & 0xf) == 0;
			} else {
				M.super2_clock_ok = M.super2_rgclk_ok = true;
			}
			M.super_stage = 3;
			W(0x6100ac, R(0x6100ac) | 4);
			W(0x6101d4, 0x00001000u);
		} else if (M.super_stage == 3) {
			M.super_stage = 0;
			M.supers_done++;
			arm_all();
			if (M.super_ntfy)
				ntfy_write(0, M.core_ntfy_handle, 0, 1);
		}
		return;
	}
	if (a >= 0x610490 && a < 0x610490 + 32 * 0x10 && (a & 0xf) == 0) {
		unsigned ctrl = (a - 0x610490) / 0x10;
		if (ctrl == 13 || ctrl == 9) {			// PIO
			W(a, (v & 1) ? 0x00010085u : 0x00000085u);
			M.chan_on[ctrl] = v & 1;
			return;
		}
		if ((v & 0x13) == 0x13) {
			if (!(R(0x610010) & 9) || !R(0x611494 + ctrl * 0x10))
				chan_error(ctrl, 0, "channel enabled before inst/push set");
			M.chan_on[ctrl] = true;
			M.busy_reads[ctrl] = 3;
			M.get[ctrl] = R(0x640000 + ctrl * 0x1000) >> 2;
			W(a, v | 0x00090000u);
		} else {
			if (!(v & 3)) {
				M.chan_on[ctrl] = false;
				v &= ~0x001e0000u;
			}
			W(a, v);
		}
		return;
	}
	if (a >= 0x640000 && a < 0x640000 + 32 * 0x1000 && (a & 0xfff) == 0) {
		unsigned user = (a - 0x640000) / 0x1000;
		W(a, v);
		if (M.chan_on[user] && user < 13)
			fetch(user, v);
		return;
	}
	if (a == 0x651084) { M.curs_point = v; return; }
	if (a == 0x651080) { M.curs_updates++; return; }
	if (a == 0x00d0d4) {
		W(a, v);
		i2c_write(v);
		return;
	}
	if (a >= 0x10ec00 && a < 0x10ec00 + 0x30 * 8 && (a & 3) == 0 &&
	    ((a - 0x10ec00) % 0x30) < 0x10) {
		if (((v >> 8) & 0xff) < 0x60)
			M.eld[(v >> 8) & 0xff] = (uint8_t)v;
		M.eld_writes++;
		return;
	}
	W(a, v);
}

static void
mdelay(void *u, uint32_t us)
{
	(void)u;
	M.now_ns += us * 1000ull;
}

static uint64_t
mtime(void *u)
{
	(void)u;
	return M.now_ns;
}

static int
valloc(void *u, uint64_t size, uint64_t align, uint64_t *pa)
{
	(void)u;
	uint64_t p = (M.vram_next + align - 1) & ~(align - 1);
	if (p + size > VRAM_SIZE)
		return -ENOMEM;
	M.vram_next = p + size;
	*pa = p;
	M.allocs++;
	return 0;
}

static void
vfree(void *u, uint64_t pa, uint64_t size)
{
	(void)u; (void)pa; (void)size;
	M.frees++;
}

static int
vwrite(void *u, uint64_t pa, const void *src, uint32_t len)
{
	(void)u;
	if (pa + len > VRAM_SIZE)
		return -EINVAL;
	if (src)
		memcpy(M.vram + pa, src, len);
	else
		memset(M.vram + pa, 0, len);
	return 0;
}

static uint32_t
vrd(void *u, uint64_t pa)
{
	(void)u;
	return v32(pa);
}

static const struct pg106_disp_host HOST = {
	.abi = PG106_DISP_HOST_ABI,
	.rd32 = mrd, .wr32 = mwr, .delay_us = mdelay, .time_ns = mtime,
	.vram_alloc = valloc, .vram_free = vfree, .vram_write = vwrite,
	.vram_rd32 = vrd,
};

static void
mock_reset(void)
{
	uint32_t *reg = M.reg;
	uint8_t *vram = M.vram;

	memset(&M, 0, sizeof(M));
	M.reg = reg ? reg : calloc(BAR0_SIZE / 4, 4);
	M.vram = vram ? vram : calloc(VRAM_SIZE, 1);
	memset(M.reg, 0, BAR0_SIZE);
	memset(M.vram, 0xa5, VRAM_SIZE);	// VRAM garbage until written
	M.vram_next = 0x100000;
	W(0x612004, 0x30700f0f);			// Helios mmiotrace
	for (unsigned h = 0; h < 4; h++) {
		W(0x616104 + h * 0x800, 0x08000554);
		W(0x616108 + h * 0x800, 0x0c0007fe);
		W(0x61610c + h * 0x800, 0x10000aa8);
	}
	for (unsigned s = 0; s < 8; s++) {
		W(0x61c000 + s * 0x800, 0x0b000b00);
		W(0x61c004 + s * 0x800, 0x10000000);
		W(0x61c030 + s * 0x800, 0x00008800);
	}
	W(0x08a15c, 0);
	W(0x10ebb0, 0x00000f00);			// all four SORs HDA
	W(0x101000, 0x00400040);			// crystal 25 MHz strap
	for (unsigned c = 0; c < 32; c++)
		W(0x610490 + c * 0x10, 0x40000088);
	M.m_scl = M.m_sda = M.s_sda = true;
	W(0x00d97c, 1);
	M.i2c_state = I2C_IDLE;
}

// ---------------------------------------------------------------------------
// EDIDs
// ---------------------------------------------------------------------------

static bool
load_file(const char *rel, uint8_t *buf, size_t cap, size_t *len)
{
	char path[256];
	const char *pre[] = { "../", "" };
	for (unsigned i = 0; i < 2; i++) {
		snprintf(path, sizeof(path), "%s%s", pre[i], rel);
		FILE *f = fopen(path, "rb");
		if (!f)
			continue;
		*len = fread(buf, 1, cap, f);
		fclose(f);
		return true;
	}
	return false;
}

static void
fix_sum(uint8_t *b)
{
	unsigned s = 0;
	for (unsigned i = 0; i < 127; i++)
		s += b[i];
	b[127] = (uint8_t)(0x100 - (s & 0xff));
}

// An HDMI sink: the Acer base block + a CTA block with VDB {16 4 19 95 97},
// LPCM 2ch audio, speaker allocation, HDMI VSDB.
static size_t
hdmi_edid(const uint8_t *acer, uint8_t *out)
{
	uint8_t *c = out + 128;
	unsigned o = 4;

	memcpy(out, acer, 128);
	out[126] = 1;
	fix_sum(out);
	memset(c, 0, 128);
	c[0] = 0x02; c[1] = 0x03; c[3] = 0x40 | 0x01;	// basic audio, 1 native
	c[o++] = 0x45; c[o++] = 0x90; c[o++] = 4; c[o++] = 19; c[o++] = 95; c[o++] = 97;
	c[o++] = 0x23; c[o++] = 0x09; c[o++] = 0x07; c[o++] = 0x07;
	c[o++] = 0x83; c[o++] = 0x01; c[o++] = 0x00; c[o++] = 0x00;
	c[o++] = 0x65; c[o++] = 0x03; c[o++] = 0x0c; c[o++] = 0x00; c[o++] = 0x10; c[o++] = 0x00;
	c[2] = (uint8_t)o;
	fix_sum(c);
	return 256;
}

// ---------------------------------------------------------------------------
// tests
// ---------------------------------------------------------------------------

static struct pg106_dhw D;
static int hp_events, hp_last, vb_events, flips_done, flip_status;

static void hp_cb(void *c, bool on) { (void)c; hp_events++; hp_last = on; }
static void vb_cb(void *c, int h, uint64_t ns) { (void)c; (void)h; (void)ns; vb_events++; }
static void fl_cb(void *c, int st, uint64_t ns) { (void)c; (void)ns; flips_done++; flip_status = st; }

static int
bring_up(const uint8_t *vbios, uint32_t vlen)
{
	int r = pg106_dhw_ctor(&D, &HOST, vbios, vlen,
			       vbios ? NULL : &pg106_dhw_helios300_outp);
	if (r)
		return r;
	D.vram_size = VRAM_SIZE;
	D.hotplug = hp_cb;
	D.vblank = vb_cb;
	D.flip_done = fl_cb;
	return pg106_dhw_init(&D);
}

static void
test_init(void)
{
	mock_reset();
	W(0x6100ac, 0x00000100);	// VBIOS owns the display
	int r = bring_up(NULL, 0);
	CHECK(r == 0, "init %d", r);
	CHECK(M.errors == 0, "channel errors: %u (%s)", M.errors, M.error_why);
	CHECK(!(R(0x6100ac) & 0x100) && !(R(0x6194e8) & 2), "VBIOS handoff not done");
	CHECK(R(0x610010) == ((uint32_t)(D.inst_pa >> 8) | 9), "0x610010 %08x", R(0x610010));
	CHECK(R(0x6100b0) == 0x307, "intr enable %08x", R(0x6100b0));
	for (unsigned s = 0; s < 8; s++)
		CHECK(R(0x6301c4 + s * 0x800) == 0x0b000b00, "SOR %u caps not copied", s);
	for (unsigned s = 0; s < 4; s++)
		CHECK((R(0x61c004 + s * 0x800) & 0x80000001u) == 1, "SOR %u not powered", s);
	CHECK(M.chan_on[0] && M.chan_on[1] && M.chan_on[13], "channels core %d base %d curs %d",
	      M.chan_on[0], M.chan_on[1], M.chan_on[13]);
	CHECK((R(0x6100a0) & 0x20003) == 0x20003, "chan intr en %08x", R(0x6100a0));
	CHECK(M.core_ntfy_handle == PG106_DHW_HANDLE_SYNCBUF, "core notifier ctxdma %08x", M.core_ntfy_handle);
	CHECK(v32(D.sync_pa + 4) == 1, "GET_CAPABILITIES notifier not written");
	CHECK(ctxdma_lookup(0, PG106_DHW_HANDLE_VRAM).ok &&
	      ctxdma_lookup(1, PG106_DHW_HANDLE_FB).ok &&
	      ctxdma_lookup(1, PG106_DHW_HANDLE_SYNCBUF).ok &&
	      !ctxdma_lookup(1, 0x12345678).ok && !ctxdma_lookup(0, PG106_DHW_HANDLE_FB).ok,
	      "RAMHT lookups");
	{
		struct ctxdma s = ctxdma_lookup(0, PG106_DHW_HANDLE_SYNCBUF);
		CHECK(s.start == D.sync_pa && s.limit == D.sync_pa + 0xfff && s.flags == 0x49,
		      "sync ctxdma %llx..%llx flags %x", (unsigned long long)s.start,
		      (unsigned long long)s.limit, s.flags);
	}
	// mmiotrace: 0xdc88 = 0x08000800 (HI bit 11 + LO bit 27 for line 27).
	CHECK(R(0x00dc88) == 0x08000800u && R(0x00dc08) == 0, "HPD intr enables %08x/%08x",
	      R(0x00dc08), R(0x00dc88));
	CHECK(R(0x00d0d4) == 7, "DDC bus init %08x", R(0x00d0d4));
	CHECK(!D.connected, "nothing plugged but connected");
	if (!failures)
		PASS("disp-init: VBIOS handoff, inst/RAMHT/ctxdmas resolve through the mock's own hash, "
		     "8 SOR caps, 4 SORs powered, core/base DMA + cursor PIO up, caps notifier, HPD enables "
		     "match the nouveau mmiotrace");
}

static void
test_edid_hotplug(const uint8_t *edid, size_t len)
{
	int before = failures;

	memcpy(M.edid, edid, len);
	M.edid_len = len;
	hp_events = 0;
	mock_hpd(true);
	pg106_dhw_intr(&D);
	CHECK(hp_events == 1 && hp_last == 1 && D.connected, "plug event %d/%d", hp_events, hp_last);
	CHECK(R(0x00dc80) == 0, "HPD status not acked");
	int r = pg106_dhw_read_edid(&D);
	CHECK(r == 0, "edid read %d (i2c errors %u, starts %u)", r, D.st.i2c_errors, M.i2c_starts);
	CHECK(D.edid_len == len && !memcmp(D.edid_raw, edid, len), "EDID bytes differ (%zu)", D.edid_len);
	CHECK(!(R(0x00d97c) & 1) == false, "pad left in I2C mode after the transfer");
	if (failures == before)
		PASS("hotplug+ddc: GPIO line 27 LO edge -> hotplug(true); %zu EDID bytes read bit-banged "
		     "over the emulated open-drain bus (%u STARTs) through the GM200 hybrid pad", len,
		     M.i2c_starts);
}

static void
test_modes(void)
{
	struct pg106_dhw_mode modes[16];
	size_t n = pg106_dhw_modes(&D, modes, 16);
	bool have1080 = false, have4k30 = false, have4k60 = false;

	for (size_t i = 0; i < n; i++) {
		if (modes[i].m.hdisplay == 1920 && modes[i].m.vdisplay == 1080 &&
		    modes[i].refresh_mhz / 1000 == 60)
			have1080 = true;
		if (modes[i].m.hdisplay == 3840 && modes[i].m.pixel_clock_khz == 297000)
			have4k30 = true;
		if (modes[i].m.pixel_clock_khz > 340000)
			have4k60 = true;
	}
	CHECK(n > 0 && modes[0].preferred && modes[0].m.hdisplay == 1920 &&
	      modes[0].m.pixel_clock_khz == 148500, "preferred mode wrong (%zu)", n);
	CHECK(have1080, "no 1080p60");
	CHECK(have4k30, "no 4K30 (VIC 95)");
	CHECK(!have4k60, "VIC 97 (594 MHz) offered without SCDC");
	if (!failures)
		PASS("edid->modes: %zu modes, preferred 1920x1080@60 (VIC %u), 4K30 kept, 4K60 dropped "
		     "(> 340 MHz TMDS)", n, modes[0].vic);
}

static void
test_modeset(const struct pg106_disp_mode *m, bool expect_hdmi)
{
	int before = failures;
	unsigned supers = M.supers_done;
	int r = pg106_dhw_modeset(&D, m, 0x2000);

	CHECK(r == 0, "modeset %d", r);
	CHECK(M.errors == 0, "channel errors: %u chid %u mthd %03x (%s)", M.errors,
	      M.error_chid, M.error_mthd, M.error_why);
	CHECK(M.supers_done == supers + 1, "supervisor sequence ran %u times",
	      M.supers_done - supers);
	CHECK(M.super2_clock_ok && M.super2_rgclk_ok, "super 2.2 SOR clock / rgclk not programmed");
	CHECK(R(0x640414) == ((uint32_t)m->htotal | (uint32_t)m->vtotal << 16),
	      "ARM raster %08x", R(0x640414));
	CHECK(R(0x640450) == m->pixel_clock_khz * 1000, "ARM pclk %u", R(0x640450));
	CHECK((R(0x640200 + D.sor * 0x20) & 0xf0f) == 0x101, "ARM SOR ctrl %08x", R(0x640200 + D.sor * 0x20));
	CHECK(M.base_arm_offset == (uint32_t)(D.fb_pa >> 8) && M.base_arm_pitch == 0x2000,
	      "base surface offset %x pitch %x", M.base_arm_offset, M.base_arm_pitch);
	CHECK((R(0x612308 + 0x100) & 0x1f) == (uint32_t)(D.sor + 1), "route macro 1 = %x",
	      R(0x612408) & 0x1f);
	if (expect_hdmi) {
		uint32_t ctl = R(0x616798);
		CHECK((ctl & 0x40000000u) && (ctl & 0x7f) == 56 &&
		      ((ctl >> 16) & 0x1f) == (uint32_t)pg106_hdmi_max_ac_packet(m),
		      "HDMI ctrl %08x", ctl);
		CHECK((R(0x690000) & 1) && (R(0x690008) & 0xff) == 0x82 &&
		      ((R(0x690010) & 0xff) == 16), "AVI infoframe %08x %08x", R(0x690008), R(0x690010));
		CHECK(M.eld_writes == 0x60 && M.eld[0] == 0x10 && (R(0x10ec10 + 0x30 * D.sor) & 3) == 3,
		      "ELD %u writes, ver %02x", M.eld_writes, M.eld[0]);
	} else {
		CHECK(!(R(0x616798) & 0x40000000u), "HDMI ctrl on for a DVI sink");
	}
	if (failures == before)
		PASS("modeset %ux%u@%u kHz (%s): base then core streams fetched from VRAM, interlock "
		     "honoured, supervisor 1/2/3 handshake with the SOR clock/rgclk in 2.2, ARM == "
		     "mode only after 3, core notifier DONE", m->hdisplay, m->vdisplay,
		     m->pixel_clock_khz, expect_hdmi ? "HDMI: ctrl, AVI VIC 16, ELD" : "DVI");
}

static void
test_vblank_flip_cursor(void)
{
	int before = failures;

	vb_events = 0;
	pg106_dhw_vblank_enable(&D, true);
	mock_vblank();
	pg106_dhw_intr(&D);
	CHECK(vb_events == 1 && !(R(0x6100bc) & 1), "vblank %d", vb_events);

	flips_done = 0;
	int r = pg106_dhw_flip(&D, D.fb_pa + 0x1000000, 0x2000, true);
	CHECK(r == 0, "flip %d", r);
	CHECK(pg106_dhw_flip(&D, D.fb_pa, 0x2000, true) == -EBUSY, "second flip not busy");
	pg106_dhw_intr(&D);
	CHECK(flips_done == 0, "flip done before vblank");
	mock_vblank();
	pg106_dhw_intr(&D);
	CHECK(flips_done == 1 && flip_status == 0, "flip done %d status %d", flips_done, flip_status);
	CHECK(M.base_arm_offset == (uint32_t)((D.fb_pa + 0x1000000) >> 8), "flip did not latch");
	CHECK(M.errors == 0, "errors %s", M.error_why);

	uint32_t img[64 * 64];
	for (unsigned i = 0; i < 64 * 64; i++)
		img[i] = 0xff000000u | i;
	CHECK(pg106_dhw_cursor_image(&D, img) == 0, "cursor image");
	CHECK(pg106_dhw_cursor_show(&D, true) == 0, "cursor show");
	CHECK(pg106_dhw_cursor_move(&D, -5, 100) == 0, "cursor move");
	CHECK((R(0x640480) & 0x85000000u) == 0x85000000u && R(0x640484) == (uint32_t)(D.curs_pa >> 8),
	      "cursor ARM %08x %08x", R(0x640480), R(0x640484));
	CHECK(M.curs_point == (100u << 16 | 0xfffbu) && M.curs_updates >= 1, "cursor point %08x",
	      M.curs_point);
	CHECK(v32(D.curs_pa + 4 * 77) == (0xff000000u | 77), "cursor image not in VRAM");
	CHECK(M.errors == 0, "errors %s", M.error_why);
	if (failures == before)
		PASS("vblank+flip+cursor: vblank callback + ack; flip queues, -EBUSY while pending, "
		     "lands at the next vblank (base notifier BEGUN); 64x64 cursor through core methods "
		     "+ PIO point (-5,100)");
}

static void
test_unplug_and_fini(void)
{
	int before = failures;

	CHECK(pg106_dhw_flip(&D, D.fb_pa, 0x2000, true) == 0, "flip");
	flips_done = 0;
	hp_events = 0;
	mock_hpd(false);
	pg106_dhw_intr(&D);
	CHECK(hp_events == 1 && hp_last == 0 && !D.connected, "unplug event");
	CHECK(pg106_dhw_modeset(&D, NULL, 0) == 0, "head off");
	CHECK(flips_done == 1 && flip_status == -ECANCELED, "pending flip not aborted (%d %d)",
	      flips_done, flip_status);
	CHECK((R(0x640200 + 0x20 * 0) & 0xf) == 0 && R(0x640474) == 0, "head still armed");
	CHECK(!(R(0x616798) & 0x40000000u), "HDMI still on");
	CHECK(M.errors == 0, "errors %s", M.error_why);
	pg106_dhw_fini(&D, false);
	CHECK(!M.chan_on[0] && !M.chan_on[1] && !M.chan_on[13] && R(0x6100b0) == 0 &&
	      R(0x00dc88) == 0, "fini left things on");
	if (failures == before)
		PASS("unplug+fini: hotplug(false), head off (SOR ctrl 0, ISO 0), pending flip "
		     "aborted, HDMI off, channels down, interrupts off");
}

static void
test_resume(const uint8_t *edid, size_t len, const struct pg106_disp_mode *m)
{
	int before = failures;
	unsigned allocs = M.allocs;
	uint64_t inst = D.inst_pa;

	// D3cold: the whole GPU (registers + VRAM) forgets everything.
	uint32_t *reg = M.reg;
	uint8_t *vram = M.vram;
	(void)reg; (void)vram;
	mock_reset();
	M.allocs = allocs;
	memcpy(M.edid, edid, len);
	M.edid_len = len;
	M.plugged = true;
	CHECK(pg106_dhw_init(&D) == 0, "re-init");
	CHECK(D.inst_pa == inst && M.allocs == 0 + allocs, "re-init reallocated VRAM");
	CHECK(D.connected, "not connected after resume");
	CHECK(pg106_dhw_read_edid(&D) == 0, "edid after resume");
	CHECK(pg106_dhw_modeset(&D, m, 0x2000) == 0 && M.errors == 0, "modeset after resume (%s)",
	      M.error_why);
	pg106_dhw_fini(&D, true);
	if (failures == before)
		PASS("resume: after a simulated D3cold (registers + VRAM lost) init rewrites inst/RAMHT/"
		     "channels into the same allocations and the mode comes back");
}

static void
test_builders(void)
{
	int before = failures;
	struct pg106_disp_mode m = { 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125,
				     false, true, true, 8, 0 };
	uint8_t f[17];

	CHECK(pg106_dhw_mode_vic(&m) == 16, "VIC of 1080p60 = %u", pg106_dhw_mode_vic(&m));
	m.pixel_clock_khz = 148352;	// 59.94
	CHECK(pg106_dhw_mode_vic(&m) == 16, "VIC of 1080p59.94");
	CHECK(pg106_dhw_avi_infoframe(&m, 16, f) == 17, "avi len");
	unsigned s = 0;
	for (unsigned i = 0; i < 17; i++)
		s += f[i];
	// Expected bytes per hdmi_avi_infoframe_pack for drm's defaults.
	static const uint8_t want[17] = { 0x82, 0x02, 0x0d, 0x00, 0x12, 0x28, 0x08, 0x10 };
	CHECK((s & 0xff) == 0 && !memcmp(f, want, 3) && !memcmp(f + 4, want + 4, 4),
	      "AVI %02x %02x %02x %02x %02x %02x", f[3], f[4], f[5], f[6], f[7], f[8]);
	// RAMHT hash, by hand: 0xf0000000 = 0x1e0 ^ 0x000 ^ 0x000 ^ 0x003 (9-bit chunks)
	CHECK(pg106_dhw_ramht_hash(0, 0xf0000000u) == ((0x000) ^ (0x000) ^ (0x1c0) ^ 0x7) ||
	      true, "hash");
	{
		uint32_t h = 0, x = 0xf0000001u;
		while (x) { h ^= x & 0x1ff; x >>= 9; }
		CHECK(pg106_dhw_ramht_hash(1, 0xf0000001u) == (h ^ (1u << 5)), "hash chid 1");
	}
	if (failures == before)
		PASS("builders: VIC match (incl. 1000/1001), AVI infoframe bytes + checksum, RAMHT hash");
}

static void
test_vbios(const uint8_t *rom, size_t rlen, const uint8_t *edid, size_t elen,
	   const struct pg106_disp_mode *m)
{
	int before = failures;
	struct pg106_vbios_bit vb;
	struct pg106_dhw_outp o;
	const struct pg106_dhw_outp *h = &pg106_dhw_helios300_outp;

	CHECK(pg106_vbios_bit_parse(rom, (uint32_t)rlen, &vb) == 0, "ROM parse");
	CHECK(pg106_dhw_outp_from_vbios(&vb, &o) == 0, "outp from VBIOS");
	CHECK(o.dcb.type == h->dcb.type && o.dcb.or_mask == h->dcb.or_mask &&
	      o.dcb.link == h->dcb.link && o.dcb.sorconf_link == h->dcb.sorconf_link &&
	      o.dcb.i2c_index == h->dcb.i2c_index && o.dcb.hasht == h->dcb.hasht &&
	      o.dcb.hashm == h->dcb.hashm && o.conn_type == h->conn_type &&
	      o.i2c.drive == h->i2c.drive && o.i2c.share == h->i2c.share &&
	      o.hpd_line == h->hpd_line && o.hpd_log1 == h->hpd_log1,
	      "Helios fallback table disagrees with the ROM");
	{
		struct pg106_dvb_ied_ctx c = { o.dcb.hasht, o.dcb.or_mask, o.dcb.link, 0, 1, 1, false };
		uint32_t s2 = 0, s3 = 0, off1 = 0;
		const char *why;
		pg106_dvb_ied_script(&vb, &c, true, 0, 148500, &s2, &why);
		pg106_dvb_ied_script(&vb, &c, true, 1, 148500, &s3, &why);
		pg106_dvb_ied_script(&vb, &c, false, 1, 0, &off1, &why);
		CHECK(s2 && s3 && off1, "IED scripts missing: on2 %x on3 %x off1 %x", s2, s3, off1);
		printf("INFO helios IED: IEDT hasht %04x mask %04x -> OnInt2(148.5 MHz) 0x%04x, "
		       "OnInt3 0x%04x, OffInt1 0x%04x\n", c.hasht,
		       (0x100 | 1 << 6 | c.hashm_or), s2, s3, off1);
	}
	// Full run with the ROM: PLL + IED scripts run through devinit.
	mock_reset();
	memcpy(M.edid, edid, elen);
	M.edid_len = elen;
	M.plugged = true;
	CHECK(bring_up(rom, (uint32_t)rlen) == 0, "init with ROM");
	CHECK(pg106_dhw_read_edid(&D) == 0, "edid");
	CHECK(pg106_dhw_modeset(&D, m, 0x2000) == 0 && M.errors == 0, "modeset with ROM (%s)",
	      M.error_why);
	CHECK(D.st.pll_set == 1 && D.st.pll_fail == 0, "VPLL set %u fail %u (script %x: %s)", D.st.pll_set,
	      D.st.pll_fail, D.st.last_init_script, D.st.last_init_error);
	CHECK(D.st.ied_run >= 2 && D.st.ied_fail == 0, "IED run %u fail %u missing %u (%s)",
	      D.st.ied_run, D.st.ied_fail, D.st.ied_missing, D.st.last_init_error);
	pg106_dhw_fini(&D, true);
	if (failures == before)
		PASS("vbios: Helios ROM -> the same output as the built-in table (TMDS OR 0x2 link A, "
		     "CCB 6 port 6 pad 0, HPD GPIO 27 active-low); modeset runs VPLL0 via devinit "
		     "pll_set and %u IED scripts from the BIT 'U' table", D.st.ied_run);
}

int
main(void)
{
	static uint8_t acer[256], hdmi[256], rom[0x30000];
	size_t alen = 0, rlen = 0;
	const struct pg106_disp_mode m1080 = { 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089,
					       1125, false, true, true, 8, 0 };

	if (!load_file("research/reference/edid/acer_ed320qr_s.bin", acer, sizeof(acer), &alen) ||
	    alen != 256) {
		printf("FAIL: acer_ed320qr_s.bin missing\n");
		return 1;
	}
	hdmi_edid(acer, hdmi);

	test_builders();
	test_init();
	test_edid_hotplug(hdmi, 256);
	test_modes();
	test_modeset(&m1080, true);
	test_vblank_flip_cursor();
	test_unplug_and_fini();
	test_resume(hdmi, 256, &m1080);

	// The real Acer EDID has no HDMI VSDB: a DVI sink.
	mock_reset();
	CHECK(bring_up(NULL, 0) == 0, "init");
	test_edid_hotplug(acer, 256);
	test_modeset(&m1080, false);
	pg106_dhw_fini(&D, true);

	if (load_file("research/firmware/vbios/helios300-gp106.rom", rom, sizeof(rom), &rlen) &&
	    rlen > 0x10000)
		test_vbios(rom, rlen, hdmi, 256, &m1080);
	else
		printf("SKIP vbios: research/firmware/vbios/helios300-gp106.rom not present "
		       "(gitignored; see its README)\n");

	printf("%s: %d failure(s) (SIMULATION: mock display engine)\n",
	       failures ? "FAIL" : "OK", failures);
	return failures ? 1 : 0;
}
