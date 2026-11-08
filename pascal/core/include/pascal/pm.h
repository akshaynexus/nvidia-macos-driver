// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/pm.h — dGPU runtime power policy: D3cold when
// no render session has been open for idle_ms, full power-up + re-POST +
// ACR/GR re-boot on the next open.
//
// Why this exists: the Helios 300 exposes ACPI power resource PG00
// (_ON/_OFF) through _PR0/_PR3 on PEG0 (SSDT11), so a full power-off of the
// dGPU is a platform operation that needs no NVIDIA PMU firmware
// (research/evidence/HELIOS300-HW-PROBE.md "Power"). After power returns the
// GPU is unposted (0x2240c bit 1 clear, BLOCKERS B5 asks whether that holds)
// and every falcon is cold, so wake = POST + ACR + GR again
// (pascal/devboot.h). This is the shape of Linux nouveau's runtime PM
// (drivers/gpu/drm/nouveau/nouveau_drm.c, torvalds/linux d48723711bae):
// nouveau_pmops_runtime_suspend (:1157-1190) runs nouveau_do_suspend
// (:953, channels/engines fini), pci_save_state (:1183) and
// pci_set_power_state(PCI_D3cold) (:1186), which lets the PCI/ACPI core
// turn the _PR3 power resource off; nouveau_pmops_runtime_resume
// (:1192-) goes back to D0 and nouveau_do_resume (:1016) re-runs device
// init, devinit POST included. The autosuspend delay is 5000 ms (:686);
// PG106_PM_IDLE_MS_DEFAULT uses the same. The platform
// call itself is the kext's job (pascal/kext/README.md "Power").
//
// The machine is pure: it never touches hardware. Each entry point returns
// the action the caller must perform next (power on, boot, quiesce, power
// off); the caller performs it and reports the result with pg106_pm_done().
// One action is outstanding at a time.
//
// States:
//   UNBOOTED   powered (D0), not brought up: the state at kext start
//   BOOTING    ACT_BOOT outstanding (devboot)
//   IDLE       up, no sessions; the idle timer runs (if D3cold enabled)
//   ACTIVE     up, >= 1 session
//   QUIESCING  ACT_QUIESCE outstanding (stop channels, wait idle)
//   POWERING_OFF ACT_POWER_OFF outstanding
//   OFF        D3cold
//   POWERING_ON ACT_POWER_ON outstanding
//   FAILED     a boot or power-on failed; sessions are refused until
//              pg106_pm_init again (kext reload) or the next system wake,
//              which tries the whole bring-up again. A failed quiesce or
//              power off is not FAILED: the GPU stays up and D3cold is
//              disabled for this load (never cut power under a GPU we could
//              not stop).
//   SUSPENDING ACT_SUSPEND outstanding: system sleep (quiesce channels and
//              engines, interrupts off, everything on the GPU dropped; the
//              platform cuts power: suspend.h)
//   SUSPENDED  the system sleeps; no register may be touched. Sessions stay
//              counted (their clients are told the GPU went away) and opens
//              wait.
//   RESUMING   ACT_RESUME outstanding: system wake (power back, cold-GPU
//              check: pg106_resume_probe), then ACT_BOOT.
//
// System sleep is not runtime D3cold: it is never refused (the machine
// sleeps whatever the GPU does), a failed quiesce is reported but still
// ends in SUSPENDED, and every wake runs the full bring-up (nouveau's
// nouveau_do_resume -> nvkm_device_init, devinit POST included). A GPU that
// was in D3cold (OFF) when the system slept stays OFF after the wake: the
// next session powers it on as before.
//
// C11, no libc beyond <stdint.h>/<stdbool.h>.

#ifndef PASCAL_PM_H
#define PASCAL_PM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// nouveau's runtime autosuspend delay (nouveau_drm.c:686).
#define PG106_PM_IDLE_MS_DEFAULT 5000u

enum pg106_pm_state {
	PG106_PM_UNBOOTED = 0,
	PG106_PM_BOOTING,
	PG106_PM_IDLE,
	PG106_PM_ACTIVE,
	PG106_PM_QUIESCING,
	PG106_PM_POWERING_OFF,
	PG106_PM_OFF,
	PG106_PM_POWERING_ON,
	PG106_PM_FAILED,
	// appended (pg106_uc_status.pm_state keeps the old numbers)
	PG106_PM_SUSPENDING,
	PG106_PM_SUSPENDED,
	PG106_PM_RESUMING,
	PG106_PM_STATE_COUNT
};

enum pg106_pm_action {
	PG106_PM_ACT_NONE = 0,
	PG106_PM_ACT_BOOT,      // run pg106_devboot_run (POST if needed, ACR, GR)
	PG106_PM_ACT_QUIESCE,   // stop/idle channels; nothing in flight after
	PG106_PM_ACT_POWER_OFF, // pci save state, ACPI PG00._OFF (D3cold)
	PG106_PM_ACT_POWER_ON,  // ACPI PG00._ON, link up, restore config space
	PG106_PM_ACT_SUSPEND,   // system sleep: pg106_gpu_suspend + fini, no
				// register access after it
	PG106_PM_ACT_RESUME,    // system wake: power back (PG00._ON or a power
				// cycle if the GPU kept its power), then BOOT
};

// Result of pg106_pm_open(): may the session proceed?
enum pg106_pm_open_result {
	PG106_PM_OPEN_READY = 0, // GPU up, session counted
	PG106_PM_OPEN_WAIT,      // counted; GPU coming up: perform the action,
	                         // then wait for pg106_pm_ready()
	PG106_PM_OPEN_FAILED,    // refused (FAILED); not counted
};

struct pg106_pm {
	uint8_t state;
	uint8_t pending; // outstanding action (PG106_PM_ACT_*)
	uint32_t sessions;
	uint32_t idle_ms;
	uint64_t idle_since_ms;
	bool d3cold;        // policy: may power off when idle
	bool d3cold_broken; // a quiesce/power-off failed: never again
	bool wake_pending;  // an open arrived while powering down
	uint32_t generation; // bumps on every successful boot (wakes too)
	uint32_t power_cycles;
	int last_ret;
	uint8_t last_failed_action;
	// system sleep
	uint8_t resume_to;      // PG106_PM_OFF: stay off after the wake;
				// else boot (PG106_PM_BOOTING)
	bool suspend_pending;   // a suspend arrived with an action outstanding
	bool in_wake;           // the outstanding BOOT is a wake's
	uint32_t suspends, resumes, resume_failures;
	int last_suspend_ret, last_resume_ret;
};

// Start in UNBOOTED with no sessions. idle_ms 0 = power off as soon as the
// last session closes (the next tick); d3cold false = never power off.
void pg106_pm_init(struct pg106_pm *pm, uint32_t idle_ms, bool d3cold);

// Kext start(): returns ACT_BOOT from UNBOOTED, else ACT_NONE.
uint8_t pg106_pm_start(struct pg106_pm *pm);

// A render session opens / closes. open sets *action (ACT_NONE when no
// work is needed, ACT_POWER_ON from OFF; while powering down the action
// comes later, from pg106_pm_done()).
uint8_t pg106_pm_open(struct pg106_pm *pm, uint64_t now_ms, uint8_t *action);
uint8_t pg106_pm_close(struct pg106_pm *pm, uint64_t now_ms);

// Idle timer. Returns ACT_QUIESCE once IDLE has lasted idle_ms with
// D3cold enabled, else ACT_NONE.
uint8_t pg106_pm_tick(struct pg106_pm *pm, uint64_t now_ms);

// Report the outcome of `action` (must equal pm->pending; anything else is
// ignored and returns ACT_NONE). Returns the next action, if any:
//   BOOT ok -> IDLE/ACTIVE; fail -> FAILED
//   QUIESCE ok -> ACT_POWER_OFF; fail -> back up, D3cold disabled
//   POWER_OFF ok -> OFF (or ACT_POWER_ON if an open is waiting);
//             fail -> ACT_BOOT: power was not cut but the channels are
//             stopped, so the engines are re-initialised; D3cold disabled
//   POWER_ON ok -> ACT_BOOT; fail -> FAILED
uint8_t pg106_pm_done(struct pg106_pm *pm, uint8_t action, int ret,
		      uint64_t now_ms);

// System sleep. Returns ACT_SUSPEND when the GPU has state to drop (it is
// up, failed, or never booted), ACT_NONE when it is already OFF (D3cold)
// or already suspended. With another action outstanding the suspend is
// remembered and pg106_pm_done returns ACT_SUSPEND after that action.
// ACT_SUSPEND's outcome never stops the sleep: done(SUSPEND) always ends
// in SUSPENDED (a failure is kept in last_suspend_ret).
uint8_t pg106_pm_suspend(struct pg106_pm *pm, uint64_t now_ms);
// System wake: from SUSPENDED, ACT_RESUME (then ACT_BOOT from done), or
// ACT_NONE back to OFF when the GPU was in D3cold at suspend (ACT_POWER_ON
// if sessions are waiting). ACT_NONE in any other state.
//   RESUME ok -> ACT_BOOT; fail -> FAILED (sessions refused, PG106Error)
uint8_t pg106_pm_resume(struct pg106_pm *pm, uint64_t now_ms);
// SUSPENDED: the system sleeps, no register access (the SUSPEND and
// RESUME actions themselves run with the GPU powered).
bool pg106_pm_asleep(const struct pg106_pm *pm);

// GPU usable (IDLE or ACTIVE).
bool pg106_pm_ready(const struct pg106_pm *pm);

// Milliseconds until pg106_pm_tick() would act (UINT32_MAX = no timer
// needed). The kext arms its IOTimerEventSource with this.
uint32_t pg106_pm_timer_ms(const struct pg106_pm *pm, uint64_t now_ms);

const char *pg106_pm_state_name(uint8_t state);
const char *pg106_pm_action_name(uint8_t action);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_PM_H
