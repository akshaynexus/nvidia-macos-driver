// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_bringup.c — END-TO-END BRINGUP INTEGRATION TEST
// (Phase 0-5 pipeline composed over ONE shared mock BAR0/VRAM bus).
//
// ******************************************************************************
// **  SIMULATION, NOT HARDWARE PROOF.                                          **
// **  Every module below is a model; the "bus" is a byte array; the "firmware" **
// **  is real only in the sense that the ACR blobs are the actual files under  **
// **  ../research/firmware/gp106 parsed through acr.c. A PASS here asserts     **
// **  CROSS-MODULE COHERENCE OF THE STORY (probe -> devinit -> ACR boot ->     **
// **  MMU -> display -> compute submit), never device behaviour.               **
// **  (HANDOFF-PASCAL.md §7 evidence rules.)                                   **
// ******************************************************************************
//
// Composition under test (every step lands its writes in the SAME bus):
//   1. POWER-ON   pmc.c decodes a synthetic GP106 BOOT_0 staged in the bus.
//   2. DEVINIT    vbios_bit.c locates the init script in a synthetic VBIOS
//                 image; devinit.c executes INIT_ZM_REG/INIT_TIME/INIT_DONE
//                 through bus hooks — its writes must stay visible to every
//                 later reader of the same BAR0 state.
//   3. ACR BOOT   acr.c parses the real blobs; acr_boot.c builds the boot
//                 plan; the plan is replayed so falcon MMIO lands in BAR0 and
//                 WPR/IMG streams land in VRAM, inside the region the plan
//                 names.
//   4. MEMORY     mmu.c maps pushbuffer, QMD, GPFIFO ring, channel instance,
//                 USERD and a 64K big page; walks must resolve, and NO
//                 MMU-aperture region may overlap the ACR WPR region.
//   5. DISPLAY    disp_mode.c plans a 1080p60 mode set; the plan is applied
//                 to the bus; raster registers must read back 2200x1125-
//                 derived values and ACR's PFB-window writes must never have
//                 touched the PDISP range (register-map non-interference;
//                 mmio.h is the range authority).
//   6. SUBMIT     dispatch.c joins the step-4 VMM into the channel
//                 instance and builds NVK's pushbuffer (compute state +
//                 SEND_PCAS_A at the mapped QMD VA) + GPFIFO entry; the QMD
//                 upload must land at the MMU-walked PA, the GPFIFO entry
//                 must point at the pushbuffer through its MMU-resolved bus
//                 address, and the USERD GPPut kick must land AFTER the
//                 GPFIFO append in the shared bus trace.
//   7. DETERMINISM (main): the whole scenario runs twice from scratch on two
//                 identical buses; the complete write traces must diff clean
//                 byte-for-byte; the total trace length is printed.
//
// The test is PURE: it writes no files. `make test` captures this stdout into
// research/evidence/BRINGUP-TRACE.txt once (deterministic, no timestamps).
//
// Run: cd pascal && make test   (or build/test_bringup from pascal/)

#include <pascal/pmc.h>
#include <pascal/mmio.h>
#include <pascal/mmu.h>
#include <pascal/pushbuf.h>
#include <pascal/qmd.h>
#include <pascal/acr.h>
#include <pascal/acr_boot.h>
#include <pascal/devinit.h>
#include <pascal/vbios_bit.h>
#include <pascal/disp_mode.h>
#include <pascal/dispatch.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Shared mock bus: sparse 4K-page store for two address spaces (BAR0 MMIO and
// VRAM), plus an ordered write trace. Deterministic: pages start zeroed, no
// time, no RNG.
// ---------------------------------------------------------------------------

enum bus_space {
	SP_BAR0 = 1,
	SP_VRAM = 2,
};

struct brec {
	uint64_t addr;
	uint32_t value;
	uint8_t space;
};

#define BUS_BUCKETS 1024u
#define TRACE_CAP (64u * 1024u)

struct bus_page {
	uint32_t tag; // addr >> 12
	uint32_t w[1024];
};

struct bus {
	struct bus_page *pg[BUS_BUCKETS];
	struct brec *tr;
	size_t tcap;
	size_t tlen;
	int overflow;
	uint64_t clock_us; // fake clock (devinit INIT_TIME)
};

static struct bus *g_bus; // current scenario bus (single-threaded)

static uint32_t *
bus_page_of(struct bus *b, uint64_t addr)
{
	uint32_t tag = (uint32_t)(addr >> 12);
	unsigned start = tag & (BUS_BUCKETS - 1u);

	for (unsigned k = 0; k < BUS_BUCKETS; k++) {
		unsigned j = (start + k) & (BUS_BUCKETS - 1u);

		if (b->pg[j] == NULL) {
			b->pg[j] = calloc(1, sizeof(*b->pg[j]));
			if (b->pg[j] == NULL)
				return NULL;
			b->pg[j]->tag = tag;
			return b->pg[j]->w;
		}
		if (b->pg[j]->tag == tag)
			return b->pg[j]->w;
	}
	return NULL; // page table exhausted
}

static void
bus_wr32(struct bus *b, uint8_t space, uint64_t addr, uint32_t val)
{
	uint32_t *pg = bus_page_of(b, addr);

	if (pg == NULL) {
		b->overflow = 1;
		return;
	}
	pg[(addr >> 2) & 0x3ffu] = val;
	if (b->tlen < b->tcap) {
		b->tr[b->tlen].addr = addr;
		b->tr[b->tlen].value = val;
		b->tr[b->tlen].space = space;
	}
	b->tlen++;
	if (b->tlen > b->tcap)
		b->overflow = 1;
}

static uint32_t
bus_rd32(struct bus *b, uint64_t addr)
{
	uint32_t *pg = bus_page_of(b, addr);

	return pg == NULL ? 0u : pg[(addr >> 2) & 0x3ffu];
}

// First trace index where (space, addr) is written, or (size_t)-1.
static size_t
trace_find(const struct bus *b, uint8_t space, uint64_t addr)
{
	for (size_t i = 0; i < b->tlen && i < b->tcap; i++)
		if (b->tr[i].space == space && b->tr[i].addr == addr)
			return i;
	return (size_t)-1;
}

// ---------------------------------------------------------------------------
// Firmware files (real blobs, loaded once and shared by both scenario runs).
// ---------------------------------------------------------------------------

struct fw {
	const char *dir;
	uint8_t *bl, *hs, *fecs_sig, *gpccs_sig;
	size_t bl_size, hs_size, fecs_sig_size, gpccs_sig_size;
	struct pg106_acr_bl_ucode bl_view;
	struct pg106_acr_hs_ucode hs_view;
	struct pg106_acr_lsf_signature_v1 fecs, gpccs;
};

static uint8_t *
load_blob(const char *dir, const char *rel, size_t *size_out)
{
	char path[512];
	long len;
	uint8_t *buf;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, rel);
	f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len < 0) {
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)len ? (size_t)len : 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
		fclose(f);
		free(buf);
		return NULL;
	}
	fclose(f);
	*size_out = (size_t)len;
	return buf;
}

static const char *
fw_dir(void)
{
	static const char *cand[] = {
		"../research/firmware/gp106",
		"../../research/firmware/gp106",
		"research/firmware/gp106",
	};

	for (size_t i = 0; i < sizeof(cand) / sizeof(cand[0]); i++) {
		char path[512];
		FILE *f;

		snprintf(path, sizeof(path), "%s/README.md", cand[i]);
		f = fopen(path, "rb");
		if (f) {
			fclose(f);
			return cand[i];
		}
	}
	return NULL;
}

static int
fw_setup(struct fw *fw)
{
	memset(fw, 0, sizeof(*fw));
	fw->dir = fw_dir();
	if (fw->dir == NULL) {
		printf("FAIL bringup: firmware dir not found\n");
		return -1;
	}
	fw->bl = load_blob(fw->dir, "acr/bl.bin", &fw->bl_size);
	fw->hs = load_blob(fw->dir, "acr/ucode_load.bin", &fw->hs_size);
	fw->fecs_sig = load_blob(fw->dir, "gr/fecs_sig.bin",
				 &fw->fecs_sig_size);
	fw->gpccs_sig = load_blob(fw->dir, "gr/gpccs_sig.bin",
				  &fw->gpccs_sig_size);
	if (!fw->bl || !fw->hs || !fw->fecs_sig || !fw->gpccs_sig) {
		printf("FAIL bringup: missing ACR blobs under %s\n", fw->dir);
		return -1;
	}
	if (pg106_acr_ucode_parse_bl(fw->bl, fw->bl_size, &fw->bl_view) != 0 ||
	    pg106_acr_ucode_parse_hs(fw->hs, fw->hs_size, &fw->hs_view) != 0 ||
	    pg106_acr_lsf_sig_parse(fw->fecs_sig, fw->fecs_sig_size,
				    &fw->fecs) != 0 ||
	    pg106_acr_lsf_sig_parse(fw->gpccs_sig, fw->gpccs_sig_size,
				    &fw->gpccs) != 0) {
		printf("FAIL bringup: acr.c rejected the firmware blobs\n");
		return -1;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Synthetic VBIOS image for the devinit composition (BIT 'I' token -> script
// 0 -> INIT_ZM_REG / INIT_TIME / INIT_DONE). Little-endian builders.
// ---------------------------------------------------------------------------

static void
put08(uint8_t *img, size_t off, uint8_t v)
{
	img[off] = v;
}

static void
put16(uint8_t *img, size_t off, uint16_t v)
{
	img[off] = (uint8_t)v;
	img[off + 1] = (uint8_t)(v >> 8);
}

static void
put32(uint8_t *img, size_t off, uint32_t v)
{
	for (unsigned i = 0; i < 4; i++)
		img[off + i] = (uint8_t)(v >> (8 * i));
}

#define VB_SIZE 0x300u
#define VB_SCRIPT_TABLE 0x0160u
#define VB_SCRIPT_OFF 0x0200u

// devinit write targets, chosen inside blocks mmio.h enumerates (PFB, PBUS):
// the PFB value echoes the BOOT_0 word so chip identity visibly flows from
// step 1 into step 2's bus state.
#define VB_REG_PFB 0x00101000u  // PFB block [0x100000, 0x1fffff]
#define VB_REG_PFB_VAL 0x136000a1u
#define VB_REG_PBUS 0x00001040u // PBUS block [0x001000, 0x001fff]
#define VB_REG_PBUS_VAL 0x600df00du
#define VB_TIME_US 50u

static void
build_vbios(uint8_t *img)
{
	memset(img, 0, VB_SIZE);
	// ROM header: x86 signature; u16 at 0x18 = 0 -> no PCIR (vbios_bit.h).
	put08(img, 0x00, 0x55);
	put08(img, 0x01, 0xaa);
	// BIT header at 0x100: 5-byte sig, entry_size @+9, entry_count @+10.
	memcpy(&img[0x100], PG106_VBIOS_BIT_SIG, PG106_VBIOS_BIT_SIG_LEN);
	put08(img, 0x109, 12); // token-directory stride
	put08(img, 0x10a, 1);  // one token: 'I'
	// Token directory at 0x10c (12-byte entry): id 'I', version 2,
	// length 2 (the init table holds one u16), offset of the init table.
	put08(img, 0x10c, 'I');
	put08(img, 0x10d, 2);
	put16(img, 0x10e, 2);
	put16(img, 0x110, 0x0140);
	// Init table 0x140: u16 pointer to the script table (init.c:399).
	put16(img, 0x140, VB_SCRIPT_TABLE);
	// Script table 0x160: script 0 -> 0x0200 (init.c:422-424).
	put16(img, VB_SCRIPT_TABLE, VB_SCRIPT_OFF);
	// Script, encoding per init.c handlers: 0x7a + u32 reg + u32 val;
	// 0x74 + u16 usec (bi_rd16 — that is the master operand width);
	// 0x71 terminator.
	put08(img, 0x200, PG106_INIT_ZM_REG);
	put32(img, 0x201, VB_REG_PFB);
	put32(img, 0x205, VB_REG_PFB_VAL);
	put08(img, 0x209, PG106_INIT_TIME);
	put16(img, 0x20a, VB_TIME_US);
	put08(img, 0x20c, PG106_INIT_ZM_REG);
	put32(img, 0x20d, VB_REG_PBUS);
	put32(img, 0x211, VB_REG_PBUS_VAL);
	put08(img, 0x215, PG106_INIT_DONE);
}

// devinit vtable hooks: the BAR0 side of the SHARED bus. rd32/wr32 are the
// integration point — devinit's writes land in the same array every later
// step reads. IO hooks are deterministic stubs (the script never uses them).
static uint32_t
vi_rd32(struct pg106_init_env *env, uint32_t addr)
{
	(void)env;
	return bus_rd32(g_bus, addr);
}

static void
vi_wr32(struct pg106_init_env *env, uint32_t addr, uint32_t val)
{
	(void)env;
	bus_wr32(g_bus, SP_BAR0, addr, val);
}

static uint8_t
vi_rd8(struct pg106_init_env *env, uint16_t port, uint8_t index)
{
	(void)env;
	(void)port;
	(void)index;
	return 0;
}

static void
vi_wr8(struct pg106_init_env *env, uint16_t port, uint8_t index, uint8_t val)
{
	(void)env;
	(void)port;
	(void)index;
	(void)val;
}

static uint8_t
vi_rdport(struct pg106_init_env *env, uint16_t port)
{
	(void)env;
	(void)port;
	return 0;
}

static void
vi_wrport(struct pg106_init_env *env, uint16_t port, uint8_t val)
{
	(void)env;
	(void)port;
	(void)val;
}

static uint64_t
vi_time(struct pg106_init_env *env, uint32_t delay_us)
{
	(void)env;
	g_bus->clock_us += delay_us;
	return g_bus->clock_us;
}

static const struct pg106_init_vt vi_vt = {
	vi_rd32, vi_wr32, vi_rd8, vi_wr8, vi_rdport, vi_wrport, NULL,
	vi_time, NULL,
};

// ---------------------------------------------------------------------------
// Address map (one constant table; steps 4-6 share these exact values — no
// parallel constants downstream).
//
//   VRAM (physical / bus) space:              channel VM (VA) space:
//     WPR alloc  0x0F0000000 (ACR cfg)          0x10000000 channel instance
//     tables     0x18000000 (MMU table_base)    0x20000000 USERD
//     QMD        0x10020000                     0x30000000 GPFIFO ring
//     pushbuffer 0x10100000                     0xA1000000 QMD (256-align)
//     GPFIFO     0x10070000                     0xB0000000 pushbuffer
//     instance   0x10050000                     0xC0000000 64K big page
//     USERD      0x10060000
//     big page   0x10200000 (64K)
// ---------------------------------------------------------------------------

#define PA_TABLE_BASE 0x18000000ull
#define PA_TABLE_CAP 0x40000ull // page tables handed out sequentially
#define PA_INST 0x10050000ull
#define PA_USERD 0x10060000ull
#define PA_GPFIFO 0x10070000ull
#define PA_QMD 0x10020000ull
#define PA_PUSH 0x10100000ull
#define PA_BIG 0x10200000ull
#define PA_BIG_SIZE 0x10000ull

#define VA_INST 0x0000000010000000ull
#define VA_USERD 0x0000000020000000ull
#define VA_GPFIFO 0x0000000030000000ull
#define VA_QMD 0x00000000A1000000ull
#define VA_PUSH 0x00000000B0000000ull
#define VA_BIG 0x00000000C0000000ull

// ACR simulated geometry (mirrors test_acr_boot.c's stand-ins; the falcon
// base is runtime-discovered on real HW via nvkm_top — see acr_boot.h).
#define ACR_FALCON_BASE 0x003c0000u
#define ACR_VMA 0x100000000ull
#define ACR_WPR_ALLOC_BASE 0x0f0000000ull
#define ACR_BIND_ADDR 0x0e0000000ull

// EVO method window (see FINDING F1 in the output): disp_mode EVO_MTHD steps
// carry cl907d method offsets (0x0414 + head*0x300, ...); gf119.c reads the
// same state back at 0x640414 + head*0x300, i.e. the methods latch at
// BAR0 0x640000 + method offset. pg106_mmio_map does not enumerate that
// window, so the adapter pins it explicitly (documented, thin).
#define EVO_WINDOW_BASE 0x640000ull

// ---------------------------------------------------------------------------
// Scenario state / PASS machinery
// ---------------------------------------------------------------------------

struct S {
	struct bus bus;
	int verbose;
	size_t pass;
	size_t fail;
	// per-phase counters
	size_t pre_disp_pdisp_writes; // PDISP-range BAR0 writes before step 5
	size_t acr_mmio_writes;
	size_t acr_wpr_writes;
	size_t n_block_ops; // BLOCK-kind (VPLL) steps: no bus write
	size_t disp_pdisp_writes;
	size_t disp_evo_writes;
};

#define PASS(...)                                                              \
	do {                                                                   \
		s.pass++;                                                      \
		if (s.verbose)                                                 \
			printf("PASS " __VA_ARGS__);                           \
	} while (0)

#define FAIL(...)                                                              \
	do {                                                                   \
		s.fail++;                                                      \
		printf("FAIL test_bringup.c:%d: ", __LINE__);                  \
		printf(__VA_ARGS__);                                           \
	} while (0)

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (cond) {                                                    \
			PASS(__VA_ARGS__);                                     \
		} else {                                                       \
			FAIL(": %s\n", #cond);                                 \
		}                                                              \
	} while (0)

struct range {
	uint64_t lo, hi; // [lo, hi)
};

static int
range_disjoint(struct range a, struct range b)
{
	return a.hi <= b.lo || b.hi <= a.lo;
}

static uint32_t
le32(const uint8_t *b)
{
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
	       ((uint32_t)b[3] << 24);
}

// --- ACR replay (mock falcon: same canned responses as tests/test_acr_boot.c)

#define OFF_BINDSTAT 0x0dcu
#define OFF_CPUCTL 0x100u
#define OFF_MBOX0 0x040u
#define OFF_MBOX1 0x044u
#define OFF_MEM_SCRUB 0x10cu

struct acr_mock {
	size_t n_bind;
	size_t n_halt;
	size_t n_mbox;
	size_t n_fail;
	uint32_t min_wpr; // min/max WPR object-relative offsets seen
	uint32_t max_wpr;
	bool have_wpr;
	uint64_t max_img; // max IMG-space byte offset seen
};

// WPR geometry of the current run (set in step 3 before the replay): the
// gp102_acr_wpr_alloc shape (gp102.c:121-129) — shadow at the alloc base,
// WPR at base + half.
static uint64_t g_wpr_alloc_base;
static uint64_t g_wpr_alloc_size;

static uint64_t
cur_wpr_start(void)
{
	return g_wpr_alloc_base + g_wpr_alloc_size / 2;
}

static uint64_t
cur_wpr_end(void)
{
	return g_wpr_alloc_base + g_wpr_alloc_size;
}

static uint32_t
acr_mock_read(struct S *s, struct acr_mock *m,
	      const struct pg106_acrboot_step *st)
{
	uint32_t off;

	if (st->addr == PG106_PFB_WPR_CHECK) {
		// gm200_acr_wpr_check (gm200.c:57-61): selector 2 -> start
		// window, 3 -> limit - 0x20000, in 256-byte units. The
		// selector itself was staged in the shared BAR0 array.
		uint32_t sel = bus_rd32(&s->bus, PG106_PFB_WPR_CHECK);

		if (sel == 2)
			return (uint32_t)((cur_wpr_start() >> 8) & 0xffffff00u);
		return (uint32_t)(((cur_wpr_end() - 0x20000ull) >> 8) &
				  0xffffff00u);
	}
	off = (uint32_t)(st->addr - ACR_FALCON_BASE);
	switch (off) {
	case OFF_MEM_SCRUB:
		return 0x00000006u; // code+data scrub done (gm200.c:143-148)
	case OFF_BINDSTAT:
		return ++m->n_bind == 1 ? 0x00005000u : 0x0u; // bind done
	case OFF_CPUCTL:
		m->n_halt++;
		return 0x00000012u; // bit 4 HALTED
	case OFF_MBOX0:
		m->n_mbox++;
		return 0x00000000u; // boot_mbox0 == 0 (gp102.c:239)
	case OFF_MBOX1:
		return 0x00000000u;
	}
	return 0;
}

static void
acr_replay(struct S *s, const struct pg106_acrboot_plan *plan,
	   const struct pg106_acrboot_info *info, struct acr_mock *m)
{
	for (size_t i = 0; i < plan->nsteps; i++) {
		const struct pg106_acrboot_step *st = &plan->steps[i];

		switch (st->kind) {
		case PG106_ACRBOOT_WRITE_REG:
			if (st->space == PG106_ACRBOOT_MMIO) {
				bus_wr32(&s->bus, SP_BAR0, st->addr, st->value);
				s->acr_mmio_writes++;
			} else if (st->space == PG106_ACRBOOT_WPR) {
				bus_wr32(&s->bus, SP_VRAM,
					 info->shadow_start + st->addr,
					 st->value);
				s->acr_wpr_writes++;
				if (!m->have_wpr || st->addr < m->min_wpr)
					m->min_wpr = (uint32_t)st->addr;
				if (!m->have_wpr || st->addr + 4 > m->max_wpr)
					m->max_wpr = (uint32_t)(st->addr + 4);
				m->have_wpr = true;
			} else { // IMG
				bus_wr32(&s->bus, SP_VRAM, ACR_VMA + st->addr,
					 st->value);
				if (st->addr + 4 > m->max_img)
					m->max_img = st->addr + 4;
			}
			break;
		case PG106_ACRBOOT_WRITE_BLOB: {
			bool wpr = st->space == PG106_ACRBOOT_WPR;
			bool img = st->space == PG106_ACRBOOT_IMG;
			uint64_t base = wpr ? info->shadow_start + st->addr
				    : img ? ACR_VMA + st->addr
					  : st->addr;

			if ((st->blob_len & 3u) != 0)
				m->n_fail++;
			for (uint32_t o = 0; o + 4 <= st->blob_len; o += 4) {
				uint32_t v = st->blob ? le32(st->blob + o) : 0;
				uint64_t rel = st->addr + o;

				bus_wr32(&s->bus, SP_VRAM, base + o, v);
				if (wpr) {
					s->acr_wpr_writes++;
					if (!m->have_wpr || rel < m->min_wpr)
						m->min_wpr = (uint32_t)rel;
					if (!m->have_wpr ||
					    rel + 4 > m->max_wpr)
						m->max_wpr =
							(uint32_t)(rel + 4);
					m->have_wpr = true;
				}
				if (img && rel + 4 > m->max_img)
					m->max_img = rel + 4;
			}
			break;
		}
		case PG106_ACRBOOT_POLL_REG:
		case PG106_ACRBOOT_READ_REG: {
			uint32_t v = acr_mock_read(s, m, st);

			if (st->kind == PG106_ACRBOOT_POLL_REG) {
				bool ok =
					st->poll_op == PG106_ACRBOOT_POLL_EQ
					? (v & st->mask) == st->value
					: (v & st->mask) != 0;
				if (!ok)
					m->n_fail++;
			} else if ((v & st->mask) != (st->value & st->mask)) {
				m->n_fail++;
			}
			break;
		}
		}
	}
}

// ---------------------------------------------------------------------------
// The scenario. Runs twice (main); verbose only on run 1. Returns the failure
// count and the bus-trace length through *tlen_out.
// ---------------------------------------------------------------------------

static size_t
run_scenario(const struct fw *fw, struct brec *trace, size_t *tlen_out,
	     int verbose)
{
	struct S s;
	uint8_t vbios[VB_SIZE];
	struct pg106_mmu *mmu = NULL;
	struct pg106_acrboot_plan plan;
	struct pg106_acrboot_info info;

	memset(&s, 0, sizeof(s));
	memset(&info, 0, sizeof(info));
	s.verbose = verbose;
	s.bus.tr = trace;
	s.bus.tcap = TRACE_CAP;
	g_bus = &s.bus;
	g_wpr_alloc_base = ACR_WPR_ALLOC_BASE;
	g_wpr_alloc_size = 0;

	if (verbose) {
		printf("=====================================================================\n");
		printf(" GP106 BRINGUP INTEGRATION TRACE — SIMULATION, NOT HARDWARE PROOF\n");
		printf(" (pmc+vbios_bit+devinit+acr+acr_boot+mmu+disp_mode+dispatch composed\n"
		       "  over one shared mock BAR0/VRAM bus; HANDOFF-PASCAL.md Phase 0-5)\n");
		printf("=====================================================================\n");
		printf("firmware: %s (bl %zu B, ucode_load %zu B, fecs_sig %zu B, "
		       "gpccs_sig %zu B)\n",
		       fw->dir, fw->bl_size, fw->hs_size, fw->fecs_sig_size,
		       fw->gpccs_sig_size);
	}

	// ------------------------------------------------------------------
	// STEP 1 — POWER-ON: stage BOOT_0/BOOT_1 in the bus, decode via pmc.
	// ------------------------------------------------------------------
	if (verbose)
		printf("\nSTEP 1 POWER-ON (pmc over the shared BAR0 array)\n");
	{
		const uint32_t boot0 = 0x136000a1u; // synthetic GP106 boot0

		bus_wr32(&s.bus, SP_BAR0, PG106_PMC_BOOT_0, boot0);
		bus_wr32(&s.bus, SP_BAR0, PG106_PMC_BOOT_1, PG106_PMC_BOOT_1_LE);

		// Coherence: probe the word the bus actually holds, not the
		// local constant.
		uint32_t boot0_bus = bus_rd32(&s.bus, PG106_PMC_BOOT_0);
		struct pg106_pmc_probe p = pg106_pmc_probe(boot0_bus);

		CHECK(p.status == PG106_PMC_OK && boot0_bus == boot0,
		      "bringup1: probe(bus BOOT_0=0x%08x) -> status OK\n",
		      boot0_bus);
		CHECK(p.is_gp106 && p.is_gp10x &&
			      p.architecture == PG106_ARCH_GP106,
		      "bringup1: arch 0x%03x is GP106, GP10x family\n",
		      p.architecture);
		CHECK(p.revision == 0xa1u && p.implementation == 0u &&
			      !p.legacy_nv04,
		      "bringup1: rev 0x%02x impl 0x%x (modern decode)\n",
		      p.revision, p.implementation);
	}

	// ------------------------------------------------------------------
	// STEP 2 — DEVINIT: vbios_bit locates the script; devinit executes it
	// through the SHARED bus (its writes must stay visible).
	// ------------------------------------------------------------------
	if (verbose)
		printf("\nSTEP 2 REAL DEVINIT (vbios_bit BIT-walk + devinit "
		       "interpreter over the shared bus)\n");
	{
		struct pg106_vbios_bit vb;
		struct pg106_vbios_rom rom;
		struct pg106_vbios_bit_entry bit_i;
		struct pg106_vbios_bit_entry miss;
		uint32_t script_table = 0, script_off = 0;
		struct pg106_init_env env;
		struct pg106_init_trace_rec rec[8];
		int rc;

		build_vbios(vbios);

		CHECK(pg106_vbios_rom_probe(vbios, VB_SIZE, &rom) == 0 &&
			      rom.rom_sig_ok && !rom.pcir_ok,
		      "bringup2: ROM header 0xaa55 accepted, no PCIR "
		      "(synthetic image)\n");
		CHECK(pg106_vbios_bit_parse(vbios, VB_SIZE, &vb) == 0,
		      "bringup2: BIT signature located at 0x%x\n",
		      vb.bit_offset);
		CHECK(pg106_vbios_bit_entry(&vb, 'I', &bit_i) == 0 &&
			      bit_i.version == 2,
		      "bringup2: token 'I' v%u found\n", bit_i.version);
		CHECK(pg106_vbios_init_script_table(&vb, &bit_i,
						    &script_table) == 0 &&
			      script_table == VB_SCRIPT_TABLE,
		      "bringup2: init script table at 0x%04x\n",
		      script_table);
		CHECK(pg106_vbios_init_script(&vb, &bit_i, 0, &script_off) ==
				      0 &&
			      script_off == VB_SCRIPT_OFF,
		      "bringup2: script 0 at 0x%04x\n", script_off);
		CHECK(pg106_vbios_bit_entry(&vb, 'Z', &miss) == -ENOENT,
		      "bringup2: absent token correctly -ENOENT\n");

		pg106_init_env_init(&env, &vi_vt, vbios, VB_SIZE, rec, 8);
		env.table_script = script_table;
		rc = pg106_init_run(&env, script_off);
		if (rc != PG106_INIT_OK)
			printf("bringup2: devinit error: %s @0x%04x op 0x%02x\n",
			       env.error_msg, env.error_offset,
			       env.error_opcode);
		CHECK(rc == PG106_INIT_OK && env.warnings == 0,
		      "bringup2: pg106_init_run OK (%s), warnings 0\n",
		      pg106_init_strerr(rc));
		CHECK(env.trace_len == 3,
		      "bringup2: trace holds 3 records (got %zu)\n",
		      env.trace_len);
		if (env.trace_len == 3) {
			CHECK(rec[0].kind == PG106_INIT_TRACE_MMIO &&
				      rec[0].address == VB_REG_PFB &&
				      rec[0].value == VB_REG_PFB_VAL &&
				      rec[0].opcode == PG106_INIT_ZM_REG,
			      "bringup2: rec0 INIT_ZM_REG PFB 0x%08x=0x%08x\n",
			      rec[0].address, rec[0].value);
			CHECK(rec[1].kind == PG106_INIT_TRACE_TIME &&
				      rec[1].value == VB_TIME_US,
			      "bringup2: rec1 INIT_TIME %u us (fake clock now "
			      "%llu us)\n",
			      rec[1].value,
			      (unsigned long long)s.bus.clock_us);
			CHECK(rec[2].kind == PG106_INIT_TRACE_MMIO &&
				      rec[2].address == VB_REG_PBUS &&
				      rec[2].value == VB_REG_PBUS_VAL &&
				      rec[2].opcode == PG106_INIT_ZM_REG,
			      "bringup2: rec2 INIT_ZM_REG PBUS 0x%08x=0x%08x\n",
			      rec[2].address, rec[2].value);
		}
		// THE integration point: devinit's writes are visible to the
		// whole later pipeline through the same bus array.
		CHECK(bus_rd32(&s.bus, VB_REG_PFB) == VB_REG_PFB_VAL &&
			      bus_rd32(&s.bus, VB_REG_PBUS) == VB_REG_PBUS_VAL,
		      "bringup2: devinit writes visible in shared BAR0 "
		      "state\n");
		CHECK(pg106_mmio_contains(PG106_MMIO_PFB, VB_REG_PFB) &&
			      pg106_mmio_contains(PG106_MMIO_PBUS,
						  VB_REG_PBUS),
		      "bringup2: both devinit targets inside their mmio.h "
		      "blocks (PFB/PBUS)\n");
	}

	// ------------------------------------------------------------------
	// STEP 3 — ACR BOOT: plan from the real blobs, replayed against the
	// shared bus (falcon MMIO -> BAR0, WPR/IMG streams -> VRAM).
	// ------------------------------------------------------------------
	memset(&plan, 0, sizeof(plan));

	if (verbose)
		printf("\nSTEP 3 ACR BOOT (acr_boot plan over real blobs, "
		       "replayed on the shared bus)\n");
	{
		struct pg106_acr_lsfw lsfws[2];
		const struct pg106_acr_lsf_signature_v1 *sigs[2];
		struct pg106_acrboot_config cfg;
		struct acr_mock mock;

		// LS falcon descriptions: the r352 shapes from test_acr_boot.c.
		lsfws[0] = (struct pg106_acr_lsfw){
			.id = PG106_ACR_LSF_FECS,
			.bootloader_size = 0x100,
			.bootloader_imem_offset = 0x0,
			.app_size = 0x2b00,
			.app_start_offset = 0x100,
			.app_imem_entry = 0x100,
			.app_resident_code_offset = 0x0,
			.app_resident_code_size = 0x2800,
			.app_resident_data_offset = 0x2900,
			.app_resident_data_size = 0x100,
			.ucode_size = 0x2a00,
			.data_size = 0x100,
			.bld_size = 0x4c,
			.img_size = 0x2b00,
		};
		lsfws[1] = lsfws[0];
		lsfws[1].id = PG106_ACR_LSF_GPCCS;
		sigs[0] = &fw->fecs;
		sigs[1] = &fw->gpccs;

		cfg = (struct pg106_acrboot_config){
			.falcon_base = ACR_FALCON_BASE, // SIM stand-in
			.code_limit = 0x8000,		// SIM stand-in
			.vma_addr = ACR_VMA,
			.wpr_alloc_base = ACR_WPR_ALLOC_BASE,
			.bootstrap_owner = PG106_ACR_LSF_SEC2, // gp102.c:89
			.rtos_id = PG106_ACR_LSF_GPCCS,
			.boot_mbox0 = 0,
			.intr_clear = 0x10,
			.pmc_readback = 0x13f,
			.bind_target = 0, // NVKM_MEM_TARGET_VRAM
			.bind_addr = ACR_BIND_ADDR,
		};
		CHECK(pg106_acrboot_plan_build(&cfg, &fw->bl_view,
					       &fw->hs_view, lsfws, 2, sigs,
					       &plan, &info) == 0,
		      "bringup3: plan built, %zu steps (wpr_size 0x%x, "
		      "alloc 0x%llx)\n",
		      plan.nsteps, info.wpr_size,
		      (unsigned long long)info.alloc_size);
		CHECK(info.nsteps == plan.nsteps,
		      "bringup3: info nsteps matches plan\n");

		// Where the plan SAYS the ACR region lands (gp102.c:121-129):
		// shadow at the alloc base, WPR at base + half.
		g_wpr_alloc_size = info.alloc_size;
		CHECK(info.shadow_start == ACR_WPR_ALLOC_BASE &&
			      info.wpr_start == cur_wpr_start() &&
			      info.wpr_end == cur_wpr_end() &&
			      info.alloc_size / 2 >= info.wpr_size,
		      "bringup3: shadow 0x%llx, wpr [0x%llx, 0x%llx) "
		      "(alloc half 0x%llx >= wpr_size)\n",
		      (unsigned long long)info.shadow_start,
		      (unsigned long long)info.wpr_start,
		      (unsigned long long)info.wpr_end,
		      (unsigned long long)(info.alloc_size / 2));

		memset(&mock, 0, sizeof(mock));
		acr_replay(&s, &plan, &info, &mock);
		CHECK(mock.n_fail == 0 && s.bus.overflow == 0,
		      "bringup3: boot completes, every poll/read satisfied "
		      "(halt=%zu bind=%zu mbox=%zu)\n",
		      mock.n_halt, mock.n_bind, mock.n_mbox);
		CHECK(mock.n_halt == 1 && mock.n_mbox == 1 && mock.n_bind == 2,
		      "bringup3: boot handshake shape (1 halt poll, 1 mailbox "
		      "read, 2 bind polls)\n");
		CHECK(mock.have_wpr && mock.max_wpr <= info.wpr_size,
		      "bringup3: all WPR writes inside [shadow+0, shadow+"
		      "0x%x): max end off 0x%x\n",
		      info.wpr_size, mock.max_wpr);
		CHECK(mock.max_img > 0 && mock.max_img <= fw->hs_view.img_size,
		      "bringup3: all IMG writes inside the %zu-byte HS "
		      "image\n",
		      fw->hs_view.img_size);
		CHECK(s.acr_mmio_writes > 0 && s.acr_wpr_writes > 0,
		      "bringup3: %zu MMIO + %zu VRAM writes from the ACR "
		      "replay\n",
		      s.acr_mmio_writes, s.acr_wpr_writes);
		// The WPR header of the first LSF (FECS) opens the shadow
		// region: falcon_id 2 is its first word (gp102.c:87).
		CHECK(bus_rd32(&s.bus, info.shadow_start) == PG106_ACR_LSF_FECS,
		      "bringup3: shadow[0] = wpr_header.falcon_id = FECS\n");
		// PFB WPR-check window register: the one PFB BAR0 address the
		// ACR path touches, enumerated by mmio.h.
		CHECK(trace_find(&s.bus, SP_BAR0, PG106_PFB_WPR_CHECK) !=
				      (size_t)-1 &&
			      pg106_mmio_block_at((uint32_t)
						  PG106_PFB_WPR_CHECK) ==
				      PG106_MMIO_PFB,
		      "bringup3: WPR window check staged in PFB (0x%x is "
		      "block %s)\n",
		      (unsigned)PG106_PFB_WPR_CHECK,
		      pg106_mmio_block_name((uint32_t)PG106_PFB_WPR_CHECK));
		// Cross-module non-interference groundwork: PDISP must still be
		// untouched after the ACR boot.
		for (size_t i = 0; i < s.bus.tlen && i < s.bus.tcap; i++)
			if (s.bus.tr[i].space == SP_BAR0 &&
			    pg106_mmio_contains(PG106_MMIO_PDISP,
						(uint32_t)s.bus.tr[i].addr))
				s.pre_disp_pdisp_writes++;
		CHECK(s.pre_disp_pdisp_writes == 0,
		      "bringup3: zero PDISP-range writes so far (ACR WPR/PFB "
		      "window did not bleed into display)\n");
		pg106_acrboot_plan_free(&plan);
	}

	// ------------------------------------------------------------------
	// STEP 4 — MEMORY: MMU init + maps; walks resolve; no overlap with WPR.
	// ------------------------------------------------------------------
	if (verbose)
		printf("\nSTEP 4 MEMORY (mmu maps: pushbuffer/QMD/GPFIFO/"
		       "instance/USERD + 64K big page)\n");
	{
		struct pg106_mmu_cfg cfg = { .table_target = PG106_TARGET_VRAM,
					     .table_base = PA_TABLE_BASE };
		static const struct {
			uint64_t va, pa, size;
			unsigned shift;
		} maps[] = {
			{ VA_INST, PA_INST, 0x1000, PG106_PAGE_4K_SHIFT },
			{ VA_USERD, PA_USERD, 0x1000, PG106_PAGE_4K_SHIFT },
			{ VA_GPFIFO, PA_GPFIFO, 0x1000, PG106_PAGE_4K_SHIFT },
			{ VA_QMD, PA_QMD, 0x1000, PG106_PAGE_4K_SHIFT },
			{ VA_PUSH, PA_PUSH, 0x1000, PG106_PAGE_4K_SHIFT },
			{ VA_BIG, PA_BIG, PA_BIG_SIZE, PG106_PAGE_64K_SHIFT },
		};
		struct range wpr = { info.shadow_start, info.wpr_end };
		struct range bufs[] = {
			{ PA_INST, PA_INST + 0x1000 },
			{ PA_USERD, PA_USERD + 0x1000 },
			{ PA_GPFIFO, PA_GPFIFO + 0x1000 },
			{ PA_QMD, PA_QMD + 0x1000 },
			{ PA_PUSH, PA_PUSH + 0x1000 },
			{ PA_BIG, PA_BIG + PA_BIG_SIZE },
		};
		uint64_t tmin = 0, tmax = 0;
		bool have_t = false, disjoint;

		CHECK(pg106_mmu_init(&mmu, &cfg) == 0,
		      "bringup4: VMM rooted at 0x%llx (VRAM tables)\n",
		      (unsigned long long)pg106_mmu_root_addr(mmu));
		if (mmu == NULL)
			goto step6; // cannot continue without a VMM
		for (size_t i = 0; i < sizeof(maps) / sizeof(maps[0]); i++) {
			struct pg106_map m = { .va = maps[i].va,
					       .pa = maps[i].pa,
					       .size = maps[i].size,
					       .page_shift = maps[i].shift,
					       .target = PG106_TARGET_VRAM,
					       .kind = PG106_KIND_PITCH };

			if (pg106_mmu_map(mmu, &m) != 0)
				FAIL(": pg106_mmu_map(va 0x%llx)\n",
				     (unsigned long long)maps[i].va);
		}
		if (s.fail == 0)
			PASS("bringup4: 6 mappings created (5x4K + 1x64K big "
			     "page)\n");

		for (size_t i = 0; i < sizeof(maps) / sizeof(maps[0]); i++) {
			struct pg106_pte_query q;

			if (!pg106_mmu_walk(mmu, maps[i].va, &q)) {
				FAIL(": walk(0x%llx) unmapped\n",
				     (unsigned long long)maps[i].va);
				continue;
			}
			CHECK(q.pa == maps[i].pa && q.aperture == 0u &&
				      q.page_shift == maps[i].shift &&
				      q.big == (maps[i].shift == 16u),
			      "bringup4: walk(0x%011llx) -> pa 0x%llx, %uK "
			      "pages, VRAM aperture\n",
			      (unsigned long long)maps[i].va,
			      (unsigned long long)q.pa, q.page_shift >> 10);
			// q.pa is the decoded page-FRAME base (the PTE address
			// field << 12), so it equals the mapping base for any
			// byte of the mapping, including the last.
			CHECK(pg106_mmu_walk(mmu, maps[i].va + maps[i].size -
						       1,
					     &q) &&
				      q.pa == maps[i].pa &&
				      q.page_shift == maps[i].shift,
			      "bringup4: walk(last byte of 0x%011llx) "
			      "resolves to frame 0x%llx\n",
			      (unsigned long long)maps[i].va,
			      (unsigned long long)q.pa);
		}

		// Every allocated page-table page must sit in the table region.
		for (size_t i = 0; i < sizeof(maps) / sizeof(maps[0]); i++) {
			static const enum pg106_table tables[] = {
				PG106_TABLE_PDE3_ROOT, PG106_TABLE_PDE2,
				PG106_TABLE_PDE1, PG106_TABLE_DUAL_PDE,
				PG106_TABLE_PTE_4K, PG106_TABLE_PTE_64K,
			};

			for (size_t t = 0;
			     t < sizeof(tables) / sizeof(tables[0]); t++) {
				uint64_t ta;

				if (!pg106_mmu_table_addr(mmu, maps[i].va,
							  tables[t], &ta))
					continue;
				if (!have_t || ta < tmin)
					tmin = ta;
				if (!have_t || ta + 0x1000 > tmax)
					tmax = ta + 0x1000;
				have_t = true;
				if (ta < PA_TABLE_BASE ||
				    ta >= PA_TABLE_BASE + PA_TABLE_CAP)
					FAIL(": table 0x%llx outside table "
					     "region\n",
					     (unsigned long long)ta);
			}
		}
		CHECK(have_t && tmin >= PA_TABLE_BASE &&
			      tmax <= PA_TABLE_BASE + PA_TABLE_CAP,
		      "bringup4: page tables live in [0x%llx, 0x%llx)\n",
		      (unsigned long long)tmin, (unsigned long long)tmax);

		// MMU-aperture regions vs the ACR WPR region: pairwise disjoint.
		disjoint = have_t;
		if (disjoint) {
			struct range tbl = { tmin, tmax };

			disjoint = range_disjoint(wpr, tbl);
			for (size_t i = 0;
			     disjoint && i < sizeof(bufs) / sizeof(bufs[0]);
			     i++) {
				disjoint = range_disjoint(wpr, bufs[i]) &&
					   range_disjoint(tbl, bufs[i]);
				for (size_t j = i + 1;
				     disjoint &&
				     j < sizeof(bufs) / sizeof(bufs[0]);
				     j++)
					disjoint = range_disjoint(bufs[i],
								  bufs[j]);
			}
		}
		CHECK(disjoint,
		      "bringup4: NO overlap between MMU-aperture regions "
		      "(6 buffers + tables) and ACR WPR [0x%llx, 0x%llx)\n",
		      (unsigned long long)wpr.lo, (unsigned long long)wpr.hi);
	}

	// ------------------------------------------------------------------
	// STEP 5 — DISPLAY: 1080p60 mode set applied to the shared bus;
	// raster readback + PFB-vs-PDISP non-interference.
	// ------------------------------------------------------------------
	if (verbose)
		printf("\nSTEP 5 DISPLAY (disp_mode 1080p60 plan applied to "
		       "the shared bus)\n");
	{
		struct pg106_disp_mode mode = {
			.pixel_clock_khz = 148500,
			.hdisplay = 1920,
			.hsync_start = 1920 + 88,
			.hsync_end = 1920 + 88 + 44,
			.htotal = 2200, // 2200x1125 total raster
			.vdisplay = 1080,
			.vsync_start = 1080 + 4,
			.vsync_end = 1080 + 4 + 5,
			.vtotal = 1125,
			.bpc = 24,
			.head = PG106_DISP_HEAD,
			.sor = PG106_DISP_SOR,
			.hdmi_max_ac_packet = 0x1f,
			.hdmi_rekey = 0,
			.hsync_positive = true,
			.vsync_positive = true,
		};
		struct pg106_disp_step steps[PG106_DISP_STEPS_MAX];
		size_t n = pg106_disp_modeset_plan(&mode, steps,
						   PG106_DISP_STEPS_MAX);
		uint32_t hsynce = mode.hsync_end - mode.hsync_start - 1;
		uint32_t vsynce = mode.vsync_end - mode.vsync_start - 1;

		CHECK(n == 23, "bringup5: plan emits 23 steps (got %zu)\n", n);

		for (size_t i = 0; i < n; i++) {
			const struct pg106_disp_step *st = &steps[i];

			if (st->kind == PG106_EVO_MTHD) {
				// Thin documented adapter: EVO methods latch at
				// BAR0 0x640000 + method offset (the gf119
				// readback addresses; disp_mode.h header
				// comment). See FINDING F1.
				bus_wr32(&s.bus, SP_BAR0,
					 EVO_WINDOW_BASE + st->reg, st->value);
				s.disp_evo_writes++;
			} else if (st->kind == PG106_MMIO_BAR0) {
				CHECK(pg106_mmio_contains(PG106_MMIO_PDISP,
							  st->reg),
				      "bringup5: direct BAR0 step %s inside "
				      "mmio.h PDISP block\n",
				      st->name);
				bus_wr32(&s.bus, SP_BAR0, st->reg, st->value);
				s.disp_pdisp_writes++;
			} else {
				s.n_block_ops++; // BLOCK (VPLL): not MMIO
			}
		}
		PASS("bringup5: plan applied to shared bus (%zu EVO writes, "
		     "%zu direct PDISP writes, %zu VPLL block ops not "
		     "MMIO)\n",
		     s.disp_evo_writes, s.disp_pdisp_writes, s.n_block_ops);

		// Raster registers, values derived from the mode struct (no
		// parallel constants), read back from the bus through the EVO
		// window adapter. RASTER_SIZE holds the ACTIVE area
		// (1920x1080); the 2200x1125 TOTALS live in the blank/sync
		// fields (BLANK_END = VBLANKE<<16|HBLANKE with hblanke =
		// htotal - hsync_start - 1 = 191, vblanke = 40).
		CHECK(bus_rd32(&s.bus, EVO_WINDOW_BASE +
					  PG106_NV907D_HEAD_SET_RASTER_SIZE(0)) ==
			      (((uint32_t)mode.vdisplay << 16) |
			       mode.hdisplay),
		      "bringup5: raster-size readback 0x%08x = %ux%u active "
		      "(BAR0 0x%x)\n",
		      bus_rd32(&s.bus, EVO_WINDOW_BASE +
				       PG106_NV907D_HEAD_SET_RASTER_SIZE(0)),
		      mode.hdisplay, mode.vdisplay,
		      (unsigned)(EVO_WINDOW_BASE +
				 PG106_NV907D_HEAD_SET_RASTER_SIZE(0)));
		CHECK(bus_rd32(&s.bus,
			       EVO_WINDOW_BASE +
				       PG106_NV907D_HEAD_SET_RASTER_BLANK_END(
					       0)) ==
			      (((uint32_t)(mode.vtotal - mode.vsync_start -
					   1)
				<< 16) |
			       (mode.htotal - mode.hsync_start - 1)),
		      "bringup5: raster-blank-end 0x%08x derived from the "
		      "%ux%u totals\n",
		      bus_rd32(&s.bus,
			       EVO_WINDOW_BASE +
				       PG106_NV907D_HEAD_SET_RASTER_BLANK_END(
					       0)),
		      mode.htotal, mode.vtotal);
		CHECK(bus_rd32(&s.bus,
			       EVO_WINDOW_BASE +
				       PG106_NV907D_HEAD_SET_RASTER_SYNC_END(
					       0)) ==
			      ((vsynce << 16) | hsynce),
		      "bringup5: raster-sync-end readback matches 1080p60 "
		      "sync\n");
		CHECK(bus_rd32(&s.bus,
			       EVO_WINDOW_BASE +
				       PG106_NV907D_HEAD_SET_PIXEL_CLOCK_FREQUENCY(
					       0)) ==
			      mode.pixel_clock_khz * 1000u,
		      "bringup5: pixel clock readback %u Hz\n",
		      bus_rd32(&s.bus,
			       EVO_WINDOW_BASE +
				       PG106_NV907D_HEAD_SET_PIXEL_CLOCK_FREQUENCY(
					       0)));

		// Non-interference: every PDISP-range write in the whole trace
		// is one of THIS step's direct BAR0 writes; the ACR boot
		// (step 3) contributed zero and its PFB-window writes stay
		// inside PFB.
		size_t pdisp = 0;

		for (size_t i = 0; i < s.bus.tlen && i < s.bus.tcap; i++)
			if (s.bus.tr[i].space == SP_BAR0 &&
			    pg106_mmio_contains(PG106_MMIO_PDISP,
						(uint32_t)s.bus.tr[i].addr))
				pdisp++;
		CHECK(pdisp == s.pre_disp_pdisp_writes + s.disp_pdisp_writes &&
			      s.pre_disp_pdisp_writes == 0,
		      "bringup5: PDISP range touched only by the mode set "
		      "(%zu writes; ACR's PFB window never clobbered "
		      "PDISP)\n",
		      pdisp);
	}

	// ------------------------------------------------------------------
	// STEP 6 — SUBMIT: compute launch (grid 129x64x1, block 8x8x1) on the
	// step-4 mappings; VMM join, QMD/GPFIFO/GP_PUT-kick coherence.
	// ------------------------------------------------------------------
step6:
	if (verbose)
		printf("\nSTEP 6 SUBMIT (dispatch: pushbuffer + GPFIFO + "
		       "USERD GP_PUT kick on the mapped VAs)\n");
	{
		pg106_qmd qmd;
		const struct pg106_qmd_params qp = {
			.program_offset = 0x00A00000u,
			.raster_width = 129u,
			.raster_height = 64u,
			.raster_depth = 1u,
			.thread_dim = { 8u, 8u, 1u },
			.shared_memory_size = 1024u,
			.register_count = 168u,
			.barrier_count = 1u,
			.local_mem_low_size = 32u,
			.crs_size = 0x800u,
			// Semaphore + constant buffer live inside the step-4
			// 64K big-page mapping (no parallel constants):
			.release0_addr = VA_BIG + 0x40,
			.release0_payload = 42u,
			.cbuf = { { .addr = VA_BIG + 0xF000, .size = 256u } },
		};
		const pg106_sub_chan chan = {
			.chid = 7u,
			.runq = 0u,
			.inst_addr = VA_INST,
			.userd_addr = VA_USERD,
			.gpfifo_off = VA_GPFIFO,
			.gpfifo_len = 0x1000u, // the 4K ring page: 512 entries
			.devm = 0xFFFu,
			.priv = false,
			// The step-4 VMM is the channel's address space.
			.pdb_addr = pg106_mmu_root_addr(mmu),
			.pdb_target = PG106_TARGET_VRAM,
			.vm_limit = PG106_MMU_VA_LIMIT,
		};
		// NVK's one-time compute state; code region = the big page.
		const pg106_sub_compute_state cstate = {
			.program_region = VA_BIG,
			.local_memory_window = PG106_SUB_NVK_LOCAL_MEMORY_WINDOW,
			.shared_memory_window =
				PG106_SUB_NVK_SHARED_MEMORY_WINDOW,
		};
		uint32_t inst[PG106_SUB_INST_WORDS];
		struct pg106_sub_step rsteps[32];
		struct pg106_sub_rec rrec;
		uint32_t pb[128];
		static uint64_t ring[512];
		struct pg106_sub_step lsteps[128];
		struct pg106_sub_rec lrec;
		struct pg106_sub_result res;
		struct pg106_sub_launch_params lp = {
			.chan = &chan,
			.subch = 1u,
			.qmd = &qmd,
			.qmd_addr = VA_QMD, // the step-4 mapping's own VA
			.push_addr = VA_PUSH,
			.init = &cstate,
		};
		struct pg106_pte_query q;
		uint64_t qmd_pa, push_pa, ring_pa, inst_pa, userd_pa;

		if (mmu == NULL) {
			FAIL(": no VMM from step 4, submission cannot be "
			     "composed\n");
			goto finish;
		}
		CHECK(pg106_qmd_build(&qp, &qmd) == PG106_QMD_OK &&
			      pg106_qmd_validate(&qmd) == PG106_QMD_OK,
		      "bringup6: QMD v02_01 built (grid 129x64x1, block "
		      "8x8x1)\n");
		// Cross-module: the QMD's release/CB VAs resolve through the
		// step-4 big-page mapping (q.pa is the decoded page-FRAME
		// base, i.e. the mapping's base for every covered byte).
		CHECK(pg106_mmu_walk(mmu, qp.release0_addr, &q) && q.big &&
			      q.pa == PA_BIG,
		      "bringup6: QMD RELEASE0 addr 0x%llx -> 64K big-page "
		      "frame 0x%llx\n",
		      (unsigned long long)qp.release0_addr,
		      (unsigned long long)q.pa);
		CHECK(pg106_mmu_walk(mmu,
				     qp.cbuf[0].addr + qp.cbuf[0].size - 1,
				     &q) &&
			      q.big && q.pa == PA_BIG,
		      "bringup6: QMD CB0 spans inside the big page\n");

		// Channel instance (RAMFC) lands in VRAM at its walked PA.
		CHECK(pg106_sub_rec_init(&rrec, rsteps, 32) == PG106_SUB_OK &&
			      pg106_sub_ramfc_build(&chan, inst, &rrec) ==
				      PG106_SUB_OK,
		      "bringup6: RAMFC instance built (%zu words)\n",
		      rrec.len);
		pg106_mmu_walk(mmu, VA_INST, &q);
		inst_pa = q.pa;
		for (size_t i = 0; i < rrec.len; i++)
			bus_wr32(&s.bus, SP_VRAM, inst_pa + rsteps[i].addr,
				 (uint32_t)rsteps[i].value);
		CHECK(bus_rd32(&s.bus, inst_pa + 0xe8) == chan.chid,
		      "bringup6: instance RAMFC chid visible in VRAM at pa "
		      "0x%llx\n",
		      (unsigned long long)(inst_pa + 0xe8));
		// VMM join (gp100_vmm_join -> gf100_vmm_join_): the instance
		// points at the step-4 root directory, VRAM target 0, VER2
		// bit 10, 64K big pages bit 11; limit = 2^49 - 1.
		CHECK(bus_rd32(&s.bus, inst_pa + 0x200) ==
				      ((uint32_t)pg106_mmu_root_addr(mmu) |
				       (1u << 10) | (1u << 11)) &&
			      bus_rd32(&s.bus, inst_pa + 0x204) ==
				      (uint32_t)(pg106_mmu_root_addr(mmu) >>
						 32) &&
			      bus_rd32(&s.bus, inst_pa + 0x208) ==
				      0xffffffffu &&
			      bus_rd32(&s.bus, inst_pa + 0x20c) == 0x1ffffu,
		      "bringup6: instance PDB 0x%08x -> step-4 root "
		      "directory 0x%llx, VM limit 2^49-1\n",
		      bus_rd32(&s.bus, inst_pa + 0x200),
		      (unsigned long long)pg106_mmu_root_addr(mmu));

		CHECK(pg106_sub_rec_init(&lrec, lsteps, 128) == PG106_SUB_OK,
		      "bringup6: launch recorder ready\n");
		CHECK(pg106_sub_launch(&lp, pb, 128, ring, 512, 0, &res,
				       &lrec) == PG106_SUB_OK,
		      "bringup6: launch OK (%zu pb words, new_put %u)\n",
		      res.pb_words, res.new_put);
		// NVK shape: SET_OBJECT(2) + PROGRAM_REGION(3) + LOCAL/SHARED
		// windows (2+2) + SEND_PCAS_A(2) + SEND_SIGNALING_PCAS_B(1).
		CHECK(res.pb_words == 12,
		      "bringup6: 12 pushbuffer words (compute state + "
		      "SEND_PCAS_A + SEND_SIGNALING_PCAS_B)\n");

		// The pushbuffer embeds the SAME QMD the step-4 mapping
		// covers: pull the QMD's physical address through the MMU
		// walk, never a parallel constant.
		pg106_mmu_walk(mmu, VA_QMD, &q);
		qmd_pa = q.pa;
		pg106_mmu_walk(mmu, VA_PUSH, &q);
		push_pa = q.pa;
		pg106_mmu_walk(mmu, VA_GPFIFO, &q);
		ring_pa = q.pa;
		pg106_mmu_walk(mmu, VA_USERD, &q);
		userd_pa = q.pa;
		CHECK(pb[10] == (uint32_t)(VA_QMD >> 8),
		      "bringup6: SEND_PCAS_A carries qmd_addr>>8 = 0x%08x "
		      "(the step-4 VA, not a parallel constant)\n",
		      pb[10]);

		// Apply the launch steps to the shared bus in recorded order,
		// remembering trace positions for the kick-order check.
		size_t tr_qmd = (size_t)-1, tr_gpf = (size_t)-1,
		       tr_db = (size_t)-1;

		for (size_t i = 0; i < lrec.len; i++) {
			const struct pg106_sub_step *st = &lsteps[i];

			switch (st->type) {
			case PG106_SUB_STEP_QMD:
				tr_qmd = s.bus.tlen;
				for (size_t w = 0; w < PG106_QMD_WORDS; w++)
					bus_wr32(&s.bus, SP_VRAM,
						 qmd_pa + 4 * w, qmd.w[w]);
				break;
			case PG106_SUB_STEP_PB_WORD:
				bus_wr32(&s.bus, SP_VRAM,
					 push_pa + 4 * st->addr,
					 (uint32_t)st->value);
				break;
			case PG106_SUB_STEP_GPFIFO:
				tr_gpf = s.bus.tlen;
				bus_wr32(&s.bus, SP_VRAM,
					 ring_pa + 8 * st->addr, // ENTRY0
					 (uint32_t)st->value);
				bus_wr32(&s.bus, SP_VRAM,
					 ring_pa + 8 * st->addr + 4,
					 (uint32_t)(st->value >> 32));
				break;
			case PG106_SUB_STEP_GP_PUT:
				// USERD+0x8c: a VM address in the channel's
				// own USERD page, not BAR0.
				tr_db = s.bus.tlen;
				bus_wr32(&s.bus, SP_VRAM,
					 userd_pa + (st->addr - VA_USERD),
					 (uint32_t)st->value);
				break;
			default:
				break; // RELEASE0: armed in the QMD image
			}
		}
		CHECK(tr_db != (size_t)-1 && tr_gpf != (size_t)-1 &&
			      tr_qmd != (size_t)-1 && tr_db > tr_gpf &&
			      tr_gpf > tr_qmd,
		      "bringup6: shared-trace order QMD@%zu < GPFIFO@%zu < "
		      "GP_PUT@%zu (PUT lands after the GPFIFO append)\n",
		      tr_qmd, tr_gpf, tr_db);
		// The QMD words the pushbuffer embeds are the SAME words the
		// step-4 mapping covers: the upload sits at the mmu-walked PA.
		{
			bool same = true;

			for (size_t i = 0; i < PG106_QMD_WORDS; i++)
				same &= bus_rd32(&s.bus, qmd_pa + 4 * i) ==
					qmd.w[i];
			CHECK(same,
			      "bringup6: QMD upload lands at the mmu-walked "
			      "pa 0x%llx (same words the mapping covers)\n",
			      (unsigned long long)qmd_pa);
		}
		CHECK(bus_rd32(&s.bus, userd_pa + PG106_SUB_USERD_GP_PUT) ==
			      res.new_put,
		      "bringup6: USERD+0x8c (GPPut) at pa 0x%llx holds "
		      "PUT=%u\n",
		      (unsigned long long)(userd_pa + PG106_SUB_USERD_GP_PUT),
		      res.new_put);

		// GPFIFO entry points at the pushbuffer via its mmu-resolved
		// bus address.
		{
			uint64_t gaddr;
			uint32_t glen;
			unsigned glevel, gsync, gfetch, gpriv;
			uint32_t rl[2];

			pg106_gpfifo_unpack(res.gpfifo_entry, &gaddr, &glen,
					    &glevel, &gsync, &gfetch, &gpriv);
			CHECK(gaddr == VA_PUSH && glen == res.pb_words,
			      "bringup6: GPFIFO entry -> pushbuffer VA "
			      "0x%011llx, %u dwords\n",
			      (unsigned long long)gaddr, glen);
			CHECK(ring[0] == res.gpfifo_entry &&
				      bus_rd32(&s.bus, ring_pa) ==
					      (uint32_t)res.gpfifo_entry &&
				      bus_rd32(&s.bus, ring_pa + 4) ==
					      (uint32_t)(res.gpfifo_entry >>
							 32),
			      "bringup6: GPFIFO entry written at ring pa "
			      "0x%llx\n",
			      (unsigned long long)ring_pa);
			CHECK(bus_rd32(&s.bus, push_pa) == pb[0] &&
				      bus_rd32(&s.bus,
					       push_pa +
						       4 * (res.pb_words -
							    1)) ==
					      pb[res.pb_words - 1],
			      "bringup6: pushbuffer first/last words in VRAM "
			      "at pa 0x%llx (mmu-resolved)\n",
			      (unsigned long long)push_pa);
			{
				// Pascal has no BAR0 doorbell: nothing in the
				// launch touched BAR0 (0x90 is Volta+ usermode,
				// nvif/userc361.c:40).
				size_t bar0 = 0;

				for (size_t i = tr_qmd;
				     i < s.bus.tlen && i < s.bus.tcap; i++)
					if (s.bus.tr[i].space == SP_BAR0)
						bar0++;
				CHECK(bar0 == 0,
				      "bringup6: launch made %zu BAR0 writes "
				      "(kick is the USERD GPPut write)\n",
				      bar0);
			}
			CHECK(pg106_sub_runlist_chan(&chan, rl) ==
				      PG106_SUB_OK,
			      "bringup6: runlist entry 0x%08x/0x%08x (chid "
			      "%u, runq %u)\n",
			      rl[0], rl[1], chan.chid, chan.runq);
		}
	}

finish:
	if (mmu != NULL)
		pg106_mmu_destroy(mmu);

	if (s.bus.overflow || s.bus.tlen > s.bus.tcap)
		FAIL(": bus trace overflow (%zu > cap %u)\n", s.bus.tlen,
		     (unsigned)TRACE_CAP);

	if (verbose) {
		printf("\nbringup: %zu checks passed, %zu failed (SIMULATION "
		       "ONLY — no hardware was touched)\n",
		       s.pass, s.fail);
	}
	*tlen_out = s.bus.tlen <= s.bus.tcap ? s.bus.tlen : s.bus.tcap;
	return s.fail;
}

// ---------------------------------------------------------------------------
// Step 7 — DETERMINISM: run the whole scenario twice on identical fresh buses
// and diff the complete write traces byte-for-byte.
// ---------------------------------------------------------------------------

static struct brec g_trace_a[TRACE_CAP];
static struct brec g_trace_b[TRACE_CAP];

int
main(void)
{
	struct fw fw;
	size_t len_a = 0, len_b = 0, fail_a, fail_b;
	bool det_ok = true;

	if (fw_setup(&fw) != 0)
		return 1;

	fail_a = run_scenario(&fw, g_trace_a, &len_a, 1);
	fail_b = run_scenario(&fw, g_trace_b, &len_b, 0);

	if (fail_a != 0 || fail_b != 0 || len_a != len_b)
		det_ok = false;
	for (size_t i = 0; det_ok && i < len_a; i++)
		if (g_trace_a[i].addr != g_trace_b[i].addr ||
		    g_trace_a[i].value != g_trace_b[i].value ||
		    g_trace_a[i].space != g_trace_b[i].space)
			det_ok = false;

	printf("\nSTEP 7 DETERMINISM (two from-scratch runs over identical "
	       "mock buses)\n");
	if (det_ok)
		printf("PASS bringup7: runs A and B produce byte-identical "
		       "write traces; total trace length %zu writes\n",
		       len_a);
	else
		printf("FAIL bringup7: traces diverge or a run failed "
		       "(failures A=%zu B=%zu, len A=%zu B=%zu)\n",
		       fail_a, fail_b, len_a, len_b);

	printf("\nFINDINGS\n");
	printf("F1 (address-space gap, display vs mmio.h): disp_mode's "
	       "EVO_MTHD steps carry cl907d method offsets, and gf119.c "
	       "reads the latched state back at 0x640414 + head*0x300 — a "
	       "BAR0 window (0x640000+) that pg106_mmio_map does NOT "
	       "enumerate (its PDISP block ends at 0x61ffff). The test "
	       "adapts with the documented mapping BAR0 = 0x640000 + method "
	       "offset and asserts non-interference on explicit ranges; a "
	       "future phase should add the EVO method window to "
	       "pg106_mmio_map. No undocumented hack was needed.\n");
	printf("O1 (observation): the ACR falcon base 0x3c0000 is a "
	       "documented SIM stand-in (acr_boot.h); it lies in an "
	       "unenumerated BAR0 gap (pg106_mmio_block_at -> NONE), as on "
	       "real hardware where nvkm_top discovers it at runtime.\n");
	printf("\n%s: bringup integration (SIMULATION ONLY)\n",
	       det_ok ? "PASS" : "FAIL");
	return det_ok ? 0 : 1;
}
