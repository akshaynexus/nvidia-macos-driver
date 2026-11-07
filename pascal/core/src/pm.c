// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/pm.c — dGPU runtime power policy. See pm.h.

#include <pascal/pm.h>

#include <stddef.h>
#include <string.h>

void
pg106_pm_init(struct pg106_pm *pm, uint32_t idle_ms, bool d3cold)
{
	memset(pm, 0, sizeof(*pm));
	pm->state = PG106_PM_UNBOOTED;
	pm->idle_ms = idle_ms;
	pm->d3cold = d3cold;
}

static uint8_t
want(struct pg106_pm *pm, uint8_t state, uint8_t action)
{
	pm->state = state;
	pm->pending = action;
	return action;
}

uint8_t
pg106_pm_start(struct pg106_pm *pm)
{
	if (pm->state != PG106_PM_UNBOOTED || pm->pending)
		return PG106_PM_ACT_NONE;
	return want(pm, PG106_PM_BOOTING, PG106_PM_ACT_BOOT);
}

bool
pg106_pm_ready(const struct pg106_pm *pm)
{
	return pm->state == PG106_PM_IDLE || pm->state == PG106_PM_ACTIVE;
}

uint8_t
pg106_pm_open(struct pg106_pm *pm, uint64_t now_ms, uint8_t *action)
{
	uint8_t a = PG106_PM_ACT_NONE;

	(void)now_ms;
	switch (pm->state) {
	case PG106_PM_FAILED:
		if (action)
			*action = a;
		return PG106_PM_OPEN_FAILED;
	case PG106_PM_IDLE:
		pm->state = PG106_PM_ACTIVE;
		/* fall through */
	case PG106_PM_ACTIVE:
		pm->sessions++;
		if (action)
			*action = a;
		return PG106_PM_OPEN_READY;
	case PG106_PM_OFF:
		a = want(pm, PG106_PM_POWERING_ON, PG106_PM_ACT_POWER_ON);
		break;
	case PG106_PM_UNBOOTED:
		a = want(pm, PG106_PM_BOOTING, PG106_PM_ACT_BOOT);
		break;
	case PG106_PM_QUIESCING:
	case PG106_PM_POWERING_OFF:
		// Let the outstanding action finish; pg106_pm_done turns
		// around (POWER_ON after OFF, or straight back up).
		pm->wake_pending = true;
		break;
	default: // BOOTING, POWERING_ON: already on the way up
		break;
	}
	pm->sessions++;
	if (action)
		*action = a;
	return PG106_PM_OPEN_WAIT;
}

uint8_t
pg106_pm_close(struct pg106_pm *pm, uint64_t now_ms)
{
	if (pm->sessions == 0)
		return PG106_PM_ACT_NONE;
	pm->sessions--;
	if (pm->sessions == 0 && pm->state == PG106_PM_ACTIVE) {
		pm->state = PG106_PM_IDLE;
		pm->idle_since_ms = now_ms;
	}
	return PG106_PM_ACT_NONE;
}

static bool
may_power_off(const struct pg106_pm *pm)
{
	return pm->d3cold && !pm->d3cold_broken && pm->sessions == 0;
}

uint8_t
pg106_pm_tick(struct pg106_pm *pm, uint64_t now_ms)
{
	if (pm->state != PG106_PM_IDLE || pm->pending || !may_power_off(pm))
		return PG106_PM_ACT_NONE;
	if (now_ms < pm->idle_since_ms ||
	    now_ms - pm->idle_since_ms < pm->idle_ms)
		return PG106_PM_ACT_NONE;
	return want(pm, PG106_PM_QUIESCING, PG106_PM_ACT_QUIESCE);
}

// Back in service after a boot (or after a refused power-down).
static uint8_t
up(struct pg106_pm *pm, uint64_t now_ms)
{
	pm->wake_pending = false;
	if (pm->sessions) {
		pm->state = PG106_PM_ACTIVE;
	} else {
		pm->state = PG106_PM_IDLE;
		pm->idle_since_ms = now_ms;
	}
	pm->pending = PG106_PM_ACT_NONE;
	return PG106_PM_ACT_NONE;
}

uint8_t
pg106_pm_done(struct pg106_pm *pm, uint8_t action, int ret, uint64_t now_ms)
{
	if (!pm->pending || action != pm->pending)
		return PG106_PM_ACT_NONE;
	pm->pending = PG106_PM_ACT_NONE;
	pm->last_ret = ret;
	if (ret)
		pm->last_failed_action = action;

	switch (action) {
	case PG106_PM_ACT_BOOT:
		if (ret) {
			pm->state = PG106_PM_FAILED;
			pm->sessions = 0; // every waiter is told to fail
			pm->wake_pending = false;
			return PG106_PM_ACT_NONE;
		}
		pm->generation++;
		return up(pm, now_ms);
	case PG106_PM_ACT_QUIESCE:
		if (ret) {
			// Channels did not stop: keep power, stop trying.
			pm->d3cold_broken = true;
			return up(pm, now_ms);
		}
		if (pm->wake_pending || pm->sessions) {
			// An open came in while quiescing: the GPU is still
			// powered, but its channels are stopped. Re-init.
			return want(pm, PG106_PM_BOOTING, PG106_PM_ACT_BOOT);
		}
		return want(pm, PG106_PM_POWERING_OFF, PG106_PM_ACT_POWER_OFF);
	case PG106_PM_ACT_POWER_OFF:
		if (ret) {
			// Power was not cut; channels are stopped. Bring the
			// engines back and never try D3cold again.
			pm->d3cold_broken = true;
			return want(pm, PG106_PM_BOOTING, PG106_PM_ACT_BOOT);
		}
		pm->power_cycles++;
		if (pm->wake_pending || pm->sessions) {
			pm->wake_pending = false;
			return want(pm, PG106_PM_POWERING_ON, PG106_PM_ACT_POWER_ON);
		}
		pm->state = PG106_PM_OFF;
		return PG106_PM_ACT_NONE;
	case PG106_PM_ACT_POWER_ON:
		if (ret) {
			pm->state = PG106_PM_FAILED;
			pm->sessions = 0;
			return PG106_PM_ACT_NONE;
		}
		return want(pm, PG106_PM_BOOTING, PG106_PM_ACT_BOOT);
	default:
		return PG106_PM_ACT_NONE;
	}
}

uint32_t
pg106_pm_timer_ms(const struct pg106_pm *pm, uint64_t now_ms)
{
	uint64_t due;

	if (pm->state != PG106_PM_IDLE || pm->pending || !may_power_off(pm))
		return UINT32_MAX;
	due = pm->idle_since_ms + pm->idle_ms;
	if (now_ms >= due)
		return 0;
	return due - now_ms > UINT32_MAX - 1 ? UINT32_MAX - 1
					     : (uint32_t)(due - now_ms);
}

const char *
pg106_pm_state_name(uint8_t s)
{
	static const char *const n[PG106_PM_STATE_COUNT] = {
		"unbooted", "booting", "idle", "active", "quiescing",
		"powering-off", "off", "powering-on", "failed",
	};
	return s < PG106_PM_STATE_COUNT ? n[s] : "?";
}

const char *
pg106_pm_action_name(uint8_t a)
{
	switch (a) {
	case PG106_PM_ACT_NONE:      return "none";
	case PG106_PM_ACT_BOOT:      return "boot";
	case PG106_PM_ACT_QUIESCE:   return "quiesce";
	case PG106_PM_ACT_POWER_OFF: return "power-off";
	case PG106_PM_ACT_POWER_ON:  return "power-on";
	default:                     return "?";
	}
}
