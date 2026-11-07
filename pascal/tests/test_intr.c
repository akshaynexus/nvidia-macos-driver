// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_intr.c — PMC interrupt routing (gp100_mc) and the
// FIFO non-stall path a fence wait sleeps on. SIMULATION on mockgpu.h.

#include "mockgpu.h"

#include <pascal/fifo.h>
#include <pascal/intr.h>

#include <errno.h>

static int g_fail, g_checks;

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

static struct mockgpu g_m;

struct seen {
	unsigned calls;
	uint32_t last;
};

static void
on_disp(void *ctx, uint32_t p)
{
	struct seen *s = ctx;

	s->calls++;
	s->last = p;
	g_m.intr_extra &= ~0x04000000u; // the display handler acks its engine
}

int
main(void)
{
	struct pg106_hw_bus bus;
	struct pg106_intr in;
	struct seen d = { 0 };

	printf("== SIMULATION: PMC interrupt routing on tests/mockgpu.h ==\n");
	mg_init(&g_m, 0x180000000ull);
	bus = (struct pg106_hw_bus){ &g_m, mg_rd32, mg_wr32, mg_delay, NULL, 0 };
	g_m.intr_allow = 0xffffffffu;
	pg106_intr_init(&in, &bus);
	CHECK_EQ(g_m.intr_allow, 0); // 0x180 = ~0 blocked everything
	CHECK_EQ(pg106_intr_set(&in, 0x04000000u, on_disp, &d), 0);
	CHECK_EQ(g_m.intr_allow, 0x04000000u);
	CHECK_EQ(pg106_intr_set(&in, 0x04000100u, on_disp, &d), -EBUSY);
	// A display interrupt: primary sees it, unarms; secondary routes it.
	g_m.intr_extra = 0x04000000u;
	CHECK_EQ(mg_irq(&g_m), 1);
	CHECK_EQ(pg106_intr_pending(&in), 1);
	pg106_intr_unarm(&in);
	CHECK_EQ(mg_irq(&g_m), 0);
	CHECK_EQ(pg106_intr_service(&in), 0x04000000u);
	CHECK_EQ(d.calls, 1);
	pg106_intr_rearm(&in);
	CHECK_EQ(g_m.intr_allow, 0x04000000u);
	CHECK_EQ(mg_irq(&g_m), 0);
	// A bit nobody owns that is allowed (a handler removed while the
	// bit stays raised by the hardware) is blocked, not stormed on.
	CHECK_EQ(pg106_intr_set(&in, 0x00100000u, on_disp, &d), 0);
	g_m.intr_extra = 0x00100000u;
	in.h[1].fn = NULL; // simulate the race: handler gone, bit allowed
	in.h[1].mask = 0;
	CHECK_EQ(pg106_intr_service(&in), 0x00100000u);
	CHECK_EQ(in.unhandled, 1);
	CHECK_EQ(g_m.intr_allow & 0x00100000u, 0);
	pg106_intr_rearm(&in);
	CHECK_EQ(mg_irq(&g_m), 0);
	// Device gone: 0xffffffff is never "pending".
	g_m.rd_hook = NULL;
	CHECK_EQ(pg106_intr_set(&in, 0x04000000u, NULL, NULL), 0);
	CHECK_EQ(g_m.intr_allow, 0);
	// Restore after power cycle re-arms only what has handlers.
	CHECK_EQ(pg106_intr_set(&in, 0x04000000u, on_disp, &d), 0);
	g_m.intr_allow = 0xffffffffu;
	pg106_intr_restore(&in);
	CHECK_EQ(g_m.intr_allow, 0x04000000u);
	mg_free(&g_m);
	printf("%s: %d checks, %d failures\n", g_fail ? "FAILED" : "PASSED",
	       g_checks, g_fail);
	return g_fail ? 1 : 0;
}
