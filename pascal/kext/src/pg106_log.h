// SPDX-License-Identifier: GPL-2.0-only
// pg106_log.h — one log line, two sinks.
//
// On macOS 26.7.2 the first real boots showed none of our IOLog lines in
// `log show` or `dmesg`: since 10.12 IOLog goes to the unified log (and the
// serial console), not to the kernel message buffer, and `log show` can
// only decode a kext's messages when it finds that kext's binary by UUID,
// which an OpenCore-injected kext has no on-disk path for (AirportAth10k's
// lines vanished the same way). printf() (libkern KPI) writes the kernel
// message buffer, so `sudo dmesg` shows the line on any build; IOLog is
// kept for the unified log, which decodes once the kext lives in
// /Library/Extensions. Results that must be readable regardless go into
// the registry as well (PG106BootLog, PG106SelfTest, PG106FirmwareEmbedded).
#ifndef PG106_LOG_H
#define PG106_LOG_H

#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>

// Each kext defines this flag (boot-arg pg106_printf=0 clears it in probe):
// the printf sink can be bisected away without rebuilding.
extern "C" bool pg106_log_printf;

#define PG106_LOGLINE(prefix, fmt, ...)                                        \
	do {                                                                   \
		if (pg106_log_printf)                                          \
			printf(prefix fmt "\n", ##__VA_ARGS__);                \
		IOLog(prefix fmt "\n", ##__VA_ARGS__);                         \
	} while (0)

// Safe mode (boot-arg -x, OpenCore Shift+Enter) and the per-kext kill
// switch: a bad build can always be bypassed from the boot menu.
static inline bool
pg106_stand_down(const char *kill_arg)
{
	uint32_t v = 0;

	if (PE_parse_boot_argn("-x", &v, sizeof(v)))
		return true;
	v = 0;
	if (kill_arg && PE_parse_boot_argn(kill_arg, &v, sizeof(v)))
		return true;
	v = 1;
	if (PE_parse_boot_argn("pg106_printf", &v, sizeof(v)) && v == 0)
		pg106_log_printf = false;
	return false;
}

#endif // PG106_LOG_H
