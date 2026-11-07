// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_vmm.c — VRAM allocator, PRAMIN access, VMMs with
// tables in VRAM, TLB invalidate, instance-block join, BAR1/BAR2.
//
// SIMULATION against tests/mockgpu.h, whose MMU walker reads the tables
// out of the mock's VRAM independently of core/src/mmu.c. Hardware
// reference values (owner's nouveau mmiotrace of this GPU,
// research/hardware/helios300/raw/posttrace2-2026-11-07, register writes
// only) are checked where this code computes the same register word.

#include "mockgpu.h"

#include <pascal/vmm.h>
#include <pascal/vram.h>

#include <errno.h>

static int g_fail, g_checks;

#define CHECK(c)                                                         \
	do {                                                             \
		g_checks++;                                              \
		if (!(c)) {                                              \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			g_fail++;                                        \
		}                                                        \
	} while (0)
#define CHECK_EQ(a, b)                                                   \
	do {                                                             \
		unsigned long long a_ = (unsigned long long)(a);         \
		unsigned long long b_ = (unsigned long long)(b);         \
		g_checks++;                                              \
		if (a_ != b_) {                                          \
			printf("FAIL %s:%d: %s == %s (0x%llx vs 0x%llx)\n", \
			       __FILE__, __LINE__, #a, #b, a_, b_);      \
			g_fail++;                                        \
		}                                                        \
	} while (0)

#define VRAM_6G 0x180000000ull // GP106M Helios: 6 GiB

static struct mockgpu g_m;
static struct pg106_hw_bus g_bus;

static void
setup(void)
{
	mg_init(&g_m, VRAM_6G);
	g_bus = (struct pg106_hw_bus){ &g_m, mg_rd32, mg_wr32, mg_delay, NULL, 0 };
}

static void
test_range(void)
{
	struct pg106_range r;
	uint64_t a, b, c, d;

	CHECK_EQ(pg106_range_init(&r, 0, 0x100000, 0x1000), 0);
	CHECK_EQ(pg106_range_alloc(&r, 0x1000, 0, false, &a), 0);
	CHECK_EQ(a, 0);
	CHECK_EQ(pg106_range_alloc(&r, 0x1000, 0, true, &b), 0);
	CHECK_EQ(b, 0xff000);
	CHECK_EQ(pg106_range_alloc(&r, 0x3000, 0x40000, true, &c), 0);
	CHECK_EQ(c, 0xc0000); // aligned down below b
	CHECK_EQ(pg106_range_alloc(&r, 0x800, 0x10000, false, &d), 0);
	CHECK_EQ(d, 0x10000);
	CHECK(pg106_range_is_allocated(&r, c, 0x3000));
	CHECK(!pg106_range_is_allocated(&r, 0x1000, 0x1000));
	CHECK_EQ(pg106_range_free(&r, c, 0x3000), 0);
	CHECK_EQ(pg106_range_free(&r, c, 0x3000), -EINVAL); // double free
	CHECK_EQ(pg106_range_reserve(&r, 0x2000, 0x2000), 0);
	CHECK_EQ(pg106_range_reserve(&r, 0x2000, 0x1000), -EBUSY);
	CHECK_EQ(pg106_range_free(&r, 0x2000, 0x2000), 0);
	CHECK_EQ(pg106_range_free(&r, a, 0x1000), 0);
	CHECK_EQ(pg106_range_free(&r, b, 0x1000), 0);
	CHECK_EQ(pg106_range_free(&r, d, 0x1000), 0);
	CHECK_EQ(r.free_bytes, 0x100000);
	CHECK_EQ(r.nfree_, 1); // fully coalesced
	CHECK_EQ(pg106_range_alloc(&r, 0x200000, 0, false, &a), -ENOSPC);
	pg106_range_fini(&r);
	printf("PASS range allocator: head/tail, alignment, reserve, coalescing\n");
}

static void
test_vram(void)
{
	struct pg106_vram v;
	uint64_t pa, pb;
	uint8_t buf[13], back[13];
	unsigned i;

	setup();
	CHECK_EQ(pg106_vram_init(&v, &g_bus, VRAM_6G), 0);
	// nouveau keeps the last MiB out (rsvd_tail): the first tail
	// allocation ends at 6 GiB - 1 MiB, where the Helios trace has its
	// instance blocks (0x17febf000 etc.).
	CHECK_EQ(pg106_vram_alloc(&v, 0x1000, 0x1000, PG106_VRAM_TAIL, &pa), 0);
	CHECK_EQ(pa, 0x17feff000ull);
	CHECK_EQ(pg106_vram_alloc(&v, 0x80000, 0x40000, PG106_VRAM_TAIL, &pb), 0);
	CHECK_EQ(pb, 0x17fe40000ull);
	for (i = 0; i < sizeof(buf); i++)
		buf[i] = (uint8_t)(0x30 + i);
	// unaligned write across a 1 MiB PRAMIN window boundary
	CHECK_EQ(pg106_vram_write(&v, 0x1000ffffeull, buf, sizeof(buf)), 0);
	CHECK_EQ(pg106_vram_read(&v, 0x1000ffffeull, back, sizeof(back)), 0);
	CHECK(!memcmp(buf, back, sizeof(buf)));
	CHECK_EQ(mg_get(&g_m, 0x1700), 0x10010); // last window programmed
	CHECK_EQ(pg106_vram_wr32(&v, pa, 0x12345678), 0);
	CHECK_EQ(mg_vram_rd32(&g_m, pa), 0x12345678);
	CHECK_EQ(pg106_vram_rd32(&v, pa + 4), MG_POISON);
	CHECK_EQ(pg106_vram_alloc(&v, 0x2000, 0, PG106_VRAM_ZERO, &pb), 0);
	CHECK_EQ(pb, 0);
	CHECK_EQ(mg_vram_rd32(&g_m, 0x1ffc), 0);
	pg106_vram_fini(&v);
	mg_free(&g_m);
	printf("PASS VRAM: rsvd tail, PRAMIN window + unaligned RMW, zeroing\n");
}

static void
test_fb_init(void)
{
	struct pg106_vram v;
	struct pg106_fb fb = { 0 };

	setup();
	mg_set(&g_m, 0x100c14, 0x00040001);
	mg_set(&g_m, 0x100ce0, 0x0000006a); // lmag 6, lsca 10 -> 6 GiB
	CHECK_EQ(pg106_fb_vidmem_size(&g_bus), VRAM_6G);
	CHECK_EQ(pg106_vram_init(&v, &g_bus, VRAM_6G), 0);
	CHECK_EQ(pg106_fb_init(&fb, &v, 0), 0);
	CHECK_EQ(mg_get(&g_m, 0x100cc8), (uint32_t)(fb.mmu_wr >> 8));
	CHECK_EQ(mg_get(&g_m, 0x100ccc), (uint32_t)(fb.mmu_rd >> 8));
	CHECK_EQ(mg_get(&g_m, 0x100cc4) & 0x60000, 2u << 17);
	CHECK_EQ(mg_get(&g_m, 0x100c14), 0x1);
	CHECK_EQ(mg_get(&g_m, 0x100c80) & 0x1800, 0x1800);
	CHECK_EQ(mg_get(&g_m, 0x100c10), 0); // no flush page given
	CHECK(!fb.vpr_locked);
	CHECK_EQ(fb.mmu_rd & 0xfff, 0);
	CHECK_EQ(mg_vram_rd32(&g_m, fb.mmu_wr + 0x1fffc), 0); // zeroed
	pg106_vram_fini(&v);
	mg_free(&g_m);
	printf("PASS FB init: mmu_wr/rd, remapper off, per-instance big page\n");
}

// Map random-ish ranges and compare the mock walk (VRAM tables) with core
// mmu's own walk, page by page.
static void
test_vmm_walk(void)
{
	struct pg106_vram v;
	struct pg106_vmm vmm;
	uint64_t inst, va;
	struct pg106_map m;
	int ap;
	uint64_t pa;
	unsigned i, bad = 0;

	setup();
	CHECK_EQ(pg106_vram_init(&v, &g_bus, VRAM_6G), 0);
	CHECK_EQ(pg106_vram_alloc(&v, 0x1000, 0x1000, PG106_VRAM_TAIL | PG106_VRAM_ZERO,
				  &inst), 0);
	CHECK_EQ(pg106_vmm_init(&vmm, &v, PG106_MMU_VA_LIMIT, 0, false), 0);
	CHECK_EQ(pg106_vmm_join(&vmm, inst), 0);
	// gp100_vmm_join: VER2 | 64K | VRAM target | pd; limit - 1.
	CHECK_EQ(mg_vram_rd64(&g_m, inst + 0x200),
		 pg106_mmu_root_addr(vmm.mmu) | 0xc00);
	CHECK_EQ(mg_vram_rd64(&g_m, inst + 0x208), PG106_MMU_VA_LIMIT - 1);

	memset(&m, 0, sizeof(m));
	m.page_shift = 12;
	m.target = PG106_TARGET_VRAM;
	m.kind = PG106_KIND_PITCH;
	for (i = 0; i < 6; i++) {
		m.va = 0x100000000ull * (i + 1) + 0x1ff000ull * i;
		m.pa = 0x10000000ull + 0x3000000ull * i;
		m.size = 0x3000 + 0x1000ull * i;
		CHECK_EQ(pg106_vmm_map(&vmm, &m), 0);
	}
	m.page_shift = 16;
	m.va = 0x7ff00000000ull;
	m.pa = 0x40000000;
	m.size = 0x30000;
	CHECK_EQ(pg106_vmm_map(&vmm, &m), 0);
	m.page_shift = 12;
	m.va = 0x1000;
	m.pa = 0x0dead000;
	m.size = 0x1000;
	m.target = PG106_TARGET_SYS_COHERENT;
	m.vol = true;
	CHECK_EQ(pg106_vmm_map(&vmm, &m), 0);
	// Before upload the GPU sees nothing (root is zero, PDEs not written).
	CHECK(mg_translate(&g_m, inst, 0x100000000ull, &ap, &pa) != 0);
	CHECK_EQ(pg106_vmm_commit(&vmm), 0);
	CHECK(vmm.last_sync.writes > 0);
	CHECK_EQ(g_m.inv_count, 1);
	CHECK_EQ(g_m.inv_lo, pg106_vmm_pdb_invalidate(&vmm));
	CHECK_EQ(g_m.inv_last_type, 0x1); // PAGE_ALL, not HUB_ONLY
	for (i = 0; i < 6; i++) {
		uint64_t base = 0x100000000ull * (i + 1) + 0x1ff000ull * i, off;

		for (off = 0; off < 0x3000 + 0x1000ull * i; off += 0x1000) {
			struct pg106_pte_query q;

			va = base + off + 0x24;
			if (mg_translate(&g_m, inst, va, &ap, &pa) ||
			    !pg106_mmu_walk(vmm.mmu, va & ~0xfffull, &q) ||
			    pa != q.pa + 0x24 || ap != 0)
				bad++;
		}
	}
	CHECK_EQ(bad, 0);
	CHECK_EQ(mg_translate(&g_m, inst, 0x7ff00012340ull, &ap, &pa), 0);
	CHECK_EQ(pa, 0x40012340ull);
	CHECK_EQ(mg_translate(&g_m, inst, 0x1008, &ap, &pa), 0);
	CHECK_EQ(ap, 2);
	CHECK_EQ(pa, 0x0dead008ull);
	CHECK(mg_translate(&g_m, inst, 0x2000, &ap, &pa) != 0);
	CHECK_EQ(g_m.nerr, 0);

	// Unmap without a flush: the TLB still answers (stale), which is why
	// the flush is required; after commit the translation is gone.
	CHECK_EQ(mg_translate(&g_m, inst, 0x100000000ull, &ap, &pa), 0);
	CHECK_EQ(pg106_vmm_unmap(&vmm, 0x100000000ull, 0x3000, 12), 0);
	CHECK_EQ(pg106_vmm_sync(&vmm), 0);
	CHECK_EQ(mg_translate(&g_m, inst, 0x100000000ull, &ap, &pa), 0); // stale
	CHECK_EQ(pg106_vmm_flush(&vmm), 0);
	CHECK(mg_translate(&g_m, inst, 0x100000000ull, &ap, &pa) != 0);
	// Unlinked subtree after the last unmap below a PD1 slot.
	{
		uint64_t w = 1;

		CHECK(pg106_mmu_pde_word(vmm.mmu, 0x100000000ull,
					 PG106_PDE_LEVEL1, &w));
		CHECK_EQ(w, 0);
	}
	// Full re-upload (D3cold) leaves the GPU view unchanged.
	pg106_mmu_mark_all_dirty(vmm.mmu);
	CHECK_EQ(pg106_vmm_commit(&vmm), 0);
	CHECK_EQ(mg_translate(&g_m, inst, 0x7ff00012340ull, &ap, &pa), 0);
	CHECK_EQ(pa, 0x40012340ull);
	CHECK(!pg106_mmu_dirty(vmm.mmu));
	pg106_vmm_fini(&vmm);
	pg106_vram_fini(&v);
	mg_free(&g_m);
	printf("PASS VMM: VRAM tables walked by the mock == core walk; TLB stale "
	       "until 0x100cbc; join 0x200/0x208\n");
}

static void
test_vmm_limits(void)
{
	struct pg106_vram v;
	struct pg106_vmm vmm;
	struct pg106_map m;

	setup();
	CHECK_EQ(pg106_vram_init(&v, &g_bus, VRAM_6G), 0);
	// 3 table pages: root + one more; mapping needs PD2/PD1/PD0/PT.
	CHECK_EQ(pg106_vmm_init(&vmm, &v, 0x10000000, 0x3000, true), 0);
	memset(&m, 0, sizeof(m));
	m.va = 0;
	m.pa = 0;
	m.size = 0x1000;
	m.page_shift = 12;
	m.kind = PG106_KIND_PITCH;
	CHECK_EQ(pg106_vmm_map(&vmm, &m), -ENOSPC);
	m.va = 0x10000000;
	CHECK_EQ(pg106_vmm_map(&vmm, &m), -EINVAL); // beyond limit
	pg106_vmm_fini(&vmm);
	pg106_vram_fini(&v);
	mg_free(&g_m);
	printf("PASS VMM limits: table region full -> -ENOSPC, VA limit\n");
}

static bool
swallow_inv(struct mockgpu *m, uint32_t a, uint32_t v)
{
	(void)m;
	(void)v;
	return a == 0x100cbc;
}

static void
test_flush_timeout(void)
{
	struct pg106_vram v;
	struct pg106_vmm vmm;

	setup();
	CHECK_EQ(pg106_vram_init(&v, &g_bus, VRAM_6G), 0);
	CHECK_EQ(pg106_vmm_init(&vmm, &v, 0x10000000, 0, true), 0);
	g_m.wr_hook = swallow_inv;
	CHECK_EQ(pg106_vmm_flush(&vmm), -ETIMEDOUT);
	CHECK(g_m.t_us >= 2000000);
	g_m.wr_hook = NULL;
	CHECK_EQ(pg106_vmm_flush(&vmm), 0);
	CHECK_EQ(g_m.inv_last_type, 0x5); // HUB_ONLY | PAGE_ALL for BAR VMMs
	pg106_vmm_fini(&vmm);
	pg106_vram_fini(&v);
	mg_free(&g_m);
	printf("PASS TLB flush: 2 s budget on 0x100c80 bit 15; BAR VMMs HUB_ONLY\n");
}

static void
test_bar(void)
{
	struct pg106_vram v;
	struct pg106_bar bar1, bar2;
	uint64_t buf, off, off2;

	setup();
	CHECK_EQ(pg106_vram_init(&v, &g_bus, VRAM_6G), 0);
	CHECK_EQ(pg106_bar_init(&bar2, &v, PG106_BAR2, 0x2000000), 0);
	CHECK_EQ(pg106_bar_init(&bar1, &v, PG106_BAR1, 0x10000000), 0);
	CHECK_EQ(mg_get(&g_m, 0x1714), 0x80000000u | (uint32_t)(bar2.inst >> 12));
	CHECK_EQ(mg_get(&g_m, 0x1704), 0x80000000u | (uint32_t)(bar1.inst >> 12));
	// The first object nouveau allocates is BAR2's instance block, at the
	// very top below the tail: same here (0x17feff000).
	CHECK_EQ(bar2.inst, 0x17feff000ull);
	CHECK_EQ(pg106_vram_alloc(&v, 0x10000, 0x1000, 0, &buf), 0);
	CHECK_EQ(pg106_bar_map(&bar1, buf, 0x10000, PG106_TARGET_VRAM, &off), 0);
	CHECK_EQ(off, 0);
	mg_bar1_wr32(&g_m, off + 0x1234, 0xfeedf00d);
	CHECK_EQ(mg_vram_rd32(&g_m, buf + 0x1234), 0xfeedf00d);
	CHECK_EQ(pg106_vram_rd32(&v, buf + 0x1234), 0xfeedf00d);
	CHECK_EQ(pg106_bar_map(&bar1, buf + 0x8000, 0x1000, PG106_TARGET_VRAM, &off2), 0);
	CHECK_EQ(off2, 0x10000);
	CHECK_EQ(mg_bar1_rd32(&g_m, off2 + 0x0), mg_vram_rd32(&g_m, buf + 0x8000));
	CHECK_EQ(pg106_bar_unmap(&bar1, off, 0x10000), 0);
	CHECK_EQ(pg106_bar_unmap(&bar1, off, 0x10000), -EINVAL);
	g_m.nerr = 0;
	mg_bar1_wr32(&g_m, off + 0x1234, 1);
	CHECK_EQ(g_m.nerr, 1); // unmapped now
	CHECK_EQ(pg106_bar_flush(&g_bus), 0);
	CHECK_EQ(g_m.bar_flushes, 1);
	// Power cycle: VRAM lost; restore rewrites inst + tables, re-binds.
	{
		unsigned i;

		for (i = 0; i < MG_HASH; i++) {
			free(g_m.pages[i].data);
			g_m.pages[i].data = NULL;
			g_m.pages[i].pfn = 0;
		}
		mg_set(&g_m, 0x1704, 0);
		pg106_vram_forget_window(&v);
		CHECK_EQ(pg106_bar_restore(&bar1), 0);
		mg_vram_wr32(&g_m, buf + 0x8010, 0x77);
		CHECK_EQ(mg_bar1_rd32(&g_m, off2 + 0x10), 0x77);
	}
	pg106_bar_fini(&bar1);
	CHECK_EQ(mg_get(&g_m, 0x1704) & 0x80000000u, 0);
	pg106_bar_fini(&bar2);
	pg106_vram_fini(&v);
	mg_free(&g_m);
	printf("PASS BAR1/BAR2: bind 0x1704/0x1714, BAR1 accesses through the "
	       "VRAM tables, unmap, restore after power loss\n");
}

// Register words the owner's mmiotrace of this GPU shows nouveau writing,
// recomputed by this code from the instance addresses the same trace
// implies. Register-level only; no memory contents are reconstructed.
static void
test_trace_words(void)
{
	struct pg106_vram v;
	struct pg106_bar b;

	setup();
	CHECK_EQ(pg106_vram_init(&v, &g_bus, VRAM_6G), 0);
	memset(&b, 0, sizeof(b));
	b.vmm.vram = &v;
	b.id = PG106_BAR2;
	b.inst = 0x17febf000ull;
	CHECK_EQ(pg106_bar_bind(&b), 0);
	CHECK_EQ(mg_get(&g_m, 0x1714), 0x8017febfu); // trace: BAR2 bind
	b.id = PG106_BAR1;
	b.inst = 0x17feaa000ull;
	CHECK_EQ(pg106_bar_bind(&b), 0);
	CHECK_EQ(mg_get(&g_m, 0x1704), 0x8017feaau); // trace: BAR1 bind
	pg106_vram_fini(&v);
	mg_free(&g_m);
	printf("PASS hardware reference: BAR2 0x8017febf, BAR1 0x8017feaa from "
	       "inst 0x17febf000 / 0x17feaa000\n");
}

int
main(void)
{
	printf("== SIMULATION: vram/vmm/bar on tests/mockgpu.h ==\n");
	test_range();
	test_vram();
	test_fb_init();
	test_vmm_walk();
	test_vmm_limits();
	test_flush_timeout();
	test_bar();
	test_trace_words();
	printf("%s: %d checks, %d failures\n", g_fail ? "FAILED" : "PASSED",
	       g_checks, g_fail);
	return g_fail ? 1 : 0;
}
