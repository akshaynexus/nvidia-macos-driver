// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_pm.c — the D3cold idle policy state machine
// (core/src/pm.c). Pure logic: no hardware, no platform calls. Whether the
// Helios 300's PG00._OFF/_ON actually power-cycles the GPU, and whether the
// GPU then needs a POST, is BLOCKERS B5/B8.

#include <pascal/pm.h>

#include <stdint.h>
#include <stdio.h>

static int g_fail;

#define CHECK(c)                                                         \
	do {                                                             \
		if (!(c)) {                                              \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			g_fail++;                                        \
		}                                                        \
	} while (0)
#define CHECK_EQ(a, b) CHECK((long long)(a) == (long long)(b))

static void
test_cycle(void)
{
	struct pg106_pm pm;
	uint8_t act;

	pg106_pm_init(&pm, 5000, true);
	CHECK(!pg106_pm_ready(&pm));
	CHECK_EQ(pg106_pm_start(&pm), PG106_PM_ACT_BOOT);
	CHECK_EQ(pg106_pm_start(&pm), PG106_PM_ACT_NONE); // one at a time
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_BOOT, 0, 100), PG106_PM_ACT_NONE);
	CHECK_EQ(pm.state, PG106_PM_IDLE);
	CHECK_EQ(pm.generation, 1);
	CHECK(pg106_pm_ready(&pm));
	CHECK_EQ(pg106_pm_timer_ms(&pm, 100), 5000);

	// A session keeps it up however long it lasts.
	CHECK_EQ(pg106_pm_open(&pm, 200, &act), PG106_PM_OPEN_READY);
	CHECK_EQ(act, PG106_PM_ACT_NONE);
	CHECK_EQ(pm.state, PG106_PM_ACTIVE);
	CHECK_EQ(pg106_pm_timer_ms(&pm, 200), UINT32_MAX);
	CHECK_EQ(pg106_pm_tick(&pm, 1000000), PG106_PM_ACT_NONE);
	CHECK_EQ(pg106_pm_close(&pm, 10000), PG106_PM_ACT_NONE);
	CHECK_EQ(pm.state, PG106_PM_IDLE);
	CHECK_EQ(pg106_pm_timer_ms(&pm, 12000), 3000);
	CHECK_EQ(pg106_pm_tick(&pm, 14999), PG106_PM_ACT_NONE);
	CHECK_EQ(pg106_pm_tick(&pm, 15000), PG106_PM_ACT_QUIESCE);
	CHECK(!pg106_pm_ready(&pm));
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_QUIESCE, 0, 15010),
		 PG106_PM_ACT_POWER_OFF);
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_POWER_OFF, 0, 15020),
		 PG106_PM_ACT_NONE);
	CHECK_EQ(pm.state, PG106_PM_OFF);
	CHECK_EQ(pm.power_cycles, 1);

	// Next open: power on, then the full boot (POST + ACR + GR).
	CHECK_EQ(pg106_pm_open(&pm, 20000, &act), PG106_PM_OPEN_WAIT);
	CHECK_EQ(act, PG106_PM_ACT_POWER_ON);
	CHECK_EQ(pm.sessions, 1);
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_POWER_ON, 0, 20100),
		 PG106_PM_ACT_BOOT);
	// A second open while booting waits without a new action.
	CHECK_EQ(pg106_pm_open(&pm, 20150, &act), PG106_PM_OPEN_WAIT);
	CHECK_EQ(act, PG106_PM_ACT_NONE);
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_BOOT, 0, 20500), PG106_PM_ACT_NONE);
	CHECK_EQ(pm.state, PG106_PM_ACTIVE);
	CHECK_EQ(pm.sessions, 2);
	CHECK_EQ(pm.generation, 2);
	printf("PASS boot -> idle -> quiesce -> D3cold -> power-on -> re-boot\n");
}

static void
test_races(void)
{
	struct pg106_pm pm;
	uint8_t act;

	// Open while quiescing: power is still on, re-init without D3cold.
	pg106_pm_init(&pm, 0, true);
	pg106_pm_start(&pm);
	pg106_pm_done(&pm, PG106_PM_ACT_BOOT, 0, 0);
	CHECK_EQ(pg106_pm_tick(&pm, 0), PG106_PM_ACT_QUIESCE); // idle_ms 0
	CHECK_EQ(pg106_pm_open(&pm, 1, &act), PG106_PM_OPEN_WAIT);
	CHECK_EQ(act, PG106_PM_ACT_NONE);
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_QUIESCE, 0, 2), PG106_PM_ACT_BOOT);
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_BOOT, 0, 3), PG106_PM_ACT_NONE);
	CHECK_EQ(pm.state, PG106_PM_ACTIVE);
	CHECK_EQ(pm.power_cycles, 0);

	// Open while powering off: finish the off, then power straight on.
	pg106_pm_close(&pm, 4);
	CHECK_EQ(pg106_pm_tick(&pm, 4), PG106_PM_ACT_QUIESCE);
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_QUIESCE, 0, 5),
		 PG106_PM_ACT_POWER_OFF);
	CHECK_EQ(pg106_pm_open(&pm, 6, &act), PG106_PM_OPEN_WAIT);
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_POWER_OFF, 0, 7),
		 PG106_PM_ACT_POWER_ON);
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_POWER_ON, 0, 8), PG106_PM_ACT_BOOT);
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_BOOT, 0, 9), PG106_PM_ACT_NONE);
	CHECK_EQ(pm.state, PG106_PM_ACTIVE);
	CHECK_EQ(pm.power_cycles, 1);

	// A stale or wrong completion is ignored.
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_POWER_ON, 0, 10), PG106_PM_ACT_NONE);
	CHECK_EQ(pm.state, PG106_PM_ACTIVE);
	pg106_pm_close(&pm, 11);
	pg106_pm_close(&pm, 11); // extra close: harmless
	CHECK_EQ(pm.sessions, 0);
	printf("PASS open during quiesce / during power-off; stale completions ignored\n");
}

static void
test_failures(void)
{
	struct pg106_pm pm;
	uint8_t act;

	// Quiesce fails: keep power, never try again.
	pg106_pm_init(&pm, 0, true);
	pg106_pm_start(&pm);
	pg106_pm_done(&pm, PG106_PM_ACT_BOOT, 0, 0);
	pg106_pm_tick(&pm, 0);
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_QUIESCE, -16, 1), PG106_PM_ACT_NONE);
	CHECK_EQ(pm.state, PG106_PM_IDLE);
	CHECK(pm.d3cold_broken);
	CHECK_EQ(pg106_pm_tick(&pm, 100000), PG106_PM_ACT_NONE);
	CHECK_EQ(pg106_pm_timer_ms(&pm, 100000), UINT32_MAX);

	// Power-off call fails: engines re-init, D3cold off for this load.
	pg106_pm_init(&pm, 0, true);
	pg106_pm_start(&pm);
	pg106_pm_done(&pm, PG106_PM_ACT_BOOT, 0, 0);
	pg106_pm_tick(&pm, 0);
	pg106_pm_done(&pm, PG106_PM_ACT_QUIESCE, 0, 1);
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_POWER_OFF, -5, 2), PG106_PM_ACT_BOOT);
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_BOOT, 0, 3), PG106_PM_ACT_NONE);
	CHECK_EQ(pm.state, PG106_PM_IDLE);
	CHECK(pm.d3cold_broken);
	CHECK_EQ(pm.last_failed_action, PG106_PM_ACT_POWER_OFF);

	// Boot fails: FAILED, waiters refused, new opens refused.
	pg106_pm_init(&pm, 0, true);
	pg106_pm_start(&pm);
	CHECK_EQ(pg106_pm_open(&pm, 0, &act), PG106_PM_OPEN_WAIT);
	pg106_pm_done(&pm, PG106_PM_ACT_BOOT, -110, 1);
	CHECK_EQ(pm.state, PG106_PM_FAILED);
	CHECK_EQ(pm.sessions, 0);
	CHECK_EQ(pg106_pm_open(&pm, 2, &act), PG106_PM_OPEN_FAILED);
	CHECK_EQ(pm.sessions, 0);
	CHECK(!pg106_pm_ready(&pm));

	// Power-on fails: FAILED as well.
	pg106_pm_init(&pm, 0, true);
	pg106_pm_start(&pm);
	pg106_pm_done(&pm, PG106_PM_ACT_BOOT, 0, 0);
	pg106_pm_tick(&pm, 0);
	pg106_pm_done(&pm, PG106_PM_ACT_QUIESCE, 0, 1);
	pg106_pm_done(&pm, PG106_PM_ACT_POWER_OFF, 0, 2);
	pg106_pm_open(&pm, 3, &act);
	CHECK_EQ(act, PG106_PM_ACT_POWER_ON);
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_POWER_ON, -19, 4), PG106_PM_ACT_NONE);
	CHECK_EQ(pm.state, PG106_PM_FAILED);

	// D3cold disabled by policy: idle forever.
	pg106_pm_init(&pm, 0, false);
	pg106_pm_start(&pm);
	pg106_pm_done(&pm, PG106_PM_ACT_BOOT, 0, 0);
	CHECK_EQ(pg106_pm_tick(&pm, 1u << 30), PG106_PM_ACT_NONE);
	CHECK_EQ(pg106_pm_timer_ms(&pm, 5), UINT32_MAX);
	// Open on an unbooted machine (start() not called yet) boots it.
	pg106_pm_init(&pm, 0, false);
	CHECK_EQ(pg106_pm_open(&pm, 0, &act), PG106_PM_OPEN_WAIT);
	CHECK_EQ(act, PG106_PM_ACT_BOOT);
	printf("PASS failures: quiesce/power-off keep power, boot/power-on -> FAILED, policy off\n");
}

// Random walk with a model of the kext that completes actions with random
// results; checks the invariants that matter for safety after every event.
static uint32_t g_rng = 0x1060c0deu;
static uint32_t
rnd(void)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 17;
	g_rng ^= g_rng << 5;
	return g_rng;
}

static void
invariants(const struct pg106_pm *pm, uint8_t emitted)
{
	const bool busy = pm->state == PG106_PM_BOOTING ||
			  pm->state == PG106_PM_QUIESCING ||
			  pm->state == PG106_PM_POWERING_OFF ||
			  pm->state == PG106_PM_POWERING_ON;

	CHECK(busy == (pm->pending != PG106_PM_ACT_NONE));
	CHECK(pm->state != PG106_PM_ACTIVE || pm->sessions > 0);
	CHECK(pm->state != PG106_PM_IDLE || pm->sessions == 0);
	CHECK(pm->state != PG106_PM_OFF || pm->sessions == 0);
	CHECK(pm->state != PG106_PM_FAILED || pm->sessions == 0);
	// Power is only ever cut with nobody waiting and D3cold allowed.
	if (emitted == PG106_PM_ACT_POWER_OFF)
		CHECK(pm->sessions == 0 && pm->d3cold && !pm->d3cold_broken &&
		      !pm->wake_pending);
}

static void
test_random(void)
{
	struct pg106_pm pm;
	uint64_t now = 0;
	unsigned run, i, offs = 0, boots = 0;

	for (run = 0; run < 200; run++) {
		uint8_t pending_act;

		pg106_pm_init(&pm, rnd() % 50, (rnd() & 3) != 0);
		pending_act = pg106_pm_start(&pm);
		for (i = 0; i < 400; i++) {
			uint8_t a = PG106_PM_ACT_NONE, oa = PG106_PM_ACT_NONE;

			now += rnd() % 20;
			switch (rnd() % 4) {
			case 0:
				pg106_pm_open(&pm, now, &oa);
				a = oa;
				break;
			case 1:
				pg106_pm_close(&pm, now);
				break;
			case 2:
				a = pg106_pm_tick(&pm, now);
				break;
			default:
				if (pm.pending) {
					int ret = (rnd() % 16) == 0 ? -5 : 0;

					if (pm.pending == PG106_PM_ACT_POWER_OFF && !ret)
						offs++;
					if (pm.pending == PG106_PM_ACT_BOOT && !ret)
						boots++;
					a = pg106_pm_done(&pm, pm.pending, ret, now);
				}
				break;
			}
			if (a != PG106_PM_ACT_NONE)
				pending_act = a;
			CHECK(pending_act == PG106_PM_ACT_NONE ||
			      pm.pending == PG106_PM_ACT_NONE ||
			      pm.pending == pending_act);
			invariants(&pm, a);
		}
	}
	CHECK(offs > 0 && boots > offs);
	printf("PASS random walk: 200 runs x 400 events, %u power-offs, %u boots, "
	       "invariants held\n", offs, boots);
}

int
main(void)
{
	printf("== pm: D3cold idle policy (pure state machine; no platform calls) ==\n");
	test_cycle();
	test_races();
	test_failures();
	test_random();
	if (g_fail) {
		printf("%d failure(s)\n", g_fail);
		return 1;
	}
	printf("PASS test_pm\n");
	return 0;
}
