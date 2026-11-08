// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/tools/pg106_probe.c — userspace check that PG106.kext started:
// finds the PG106 service, opens its user client and calls both selectors
// (pg106_uc_abi.h). macOS only; must run as root (the user client refuses
// non-administrators).
//
// Build (scripts/build-macos.sh does this):
//   clang -Wall -Wextra -I../src -o pg106_probe pg106_probe.c \
//         -framework IOKit -framework CoreFoundation
//
// Exit status: 0 = GP106 identified, 1 = service/open/call failed,
// 2 = ABI mismatch, 3 = called fine but the decode is not a GP106.
//
// IOKit.framework calls used (IOKit/IOKitLib.h): IOServiceMatching,
// IOServiceGetMatchingService, IOServiceOpen, IOConnectCallStructMethod,
// IOServiceClose, IOObjectRelease. MACH_PORT_NULL selects the default main
// port, which avoids the kIOMasterPortDefault → kIOMainPortDefault rename
// (macOS 12) in either direction.

#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "pg106_uc_abi.h"

static int
call_struct(io_connect_t conn, uint32_t sel, void *out, size_t size)
{
	size_t got = size;
	kern_return_t kr;

	memset(out, 0, size);
	kr = IOConnectCallStructMethod(conn, sel, NULL, 0, out, &got);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "pg106_probe: selector %u failed: 0x%08x\n",
			sel, kr);
		return -1;
	}
	if (got != size) {
		fprintf(stderr, "pg106_probe: selector %u returned %zu bytes, expected %zu\n",
			sel, got, size);
		return -1;
	}
	return 0;
}

int
main(void)
{
	io_service_t svc;
	io_connect_t conn = IO_OBJECT_NULL;
	struct pg106_uc_version ver;
	struct pg106_uc_pmc pmc;
	kern_return_t kr;
	int rc = 1;

	svc = IOServiceGetMatchingService(MACH_PORT_NULL,
					  IOServiceMatching("PG106"));
	if (svc == IO_OBJECT_NULL) {
		fprintf(stderr, "pg106_probe: no PG106 service registered "
				"(kext not loaded, probe() refused the GPU, or start() failed — "
				"see `log show --last 10m --predicate 'eventMessage CONTAINS \"PG106\"'`)\n");
		return 1;
	}

	kr = IOServiceOpen(svc, mach_task_self(), PG106_UC_TYPE_DEVICE, &conn);
	IOObjectRelease(svc);
	if (kr != KERN_SUCCESS) {
		// A refused initWithTask() surfaces as a generic error code,
		// so name the likely cause from our own credentials instead.
		fprintf(stderr, "pg106_probe: IOServiceOpen failed: 0x%08x%s\n", kr,
			geteuid() != 0 ? " (the user client is root-only: run with sudo)" : "");
		return 1;
	}

	if (call_struct(conn, PG106_UC_SEL_GET_VERSION, &ver, sizeof ver) != 0)
		goto out;
	printf("abi        %u.%u (tool built against %u.%u)\n", ver.major,
	       ver.minor, PG106_UC_ABI_MAJOR, PG106_UC_ABI_MINOR);
	if (ver.major != PG106_UC_ABI_MAJOR || ver.minor < PG106_UC_ABI_MINOR) {
		fprintf(stderr, "pg106_probe: kext ABI does not match this tool\n");
		rc = 2;
		goto out;
	}

	if (call_struct(conn, PG106_UC_SEL_GET_PMC, &pmc, sizeof pmc) != 0)
		goto out;
	printf("boot0      0x%08x\n", pmc.boot0);
	printf("arch       0x%03x  impl 0x%x  rev 0x%02x  status %u\n",
	       pmc.architecture, pmc.implementation, pmc.revision, pmc.status);
	printf("flags      %s%s%s\n",
	       (pmc.flags & PG106_UC_PMC_IS_GP10X) ? "gp10x " : "",
	       (pmc.flags & PG106_UC_PMC_IS_GP106) ? "gp106 " : "",
	       (pmc.flags & PG106_UC_PMC_LEGACY_NV4) ? "legacy-nv4 " : "");
	rc = (pmc.status == 0 && (pmc.flags & PG106_UC_PMC_IS_GP106)) ? 0 : 3;
	printf("result     %s\n", rc == 0 ? "GP106 identified" : "NOT a GP106 decode");

	// Bring-up / power state (ABI 0.3). Stage numbers: pascal/core
	// devboot.h (0 identify, 1 posted, 2 post, 3 top, 4 acr, 5 gr,
	// 6 sec2-bootstrap, 7 ready); pm states: pascal/core pm.h.
	{
		struct pg106_uc_status st;

		if (call_struct(conn, PG106_UC_SEL_GET_STATUS, &st, sizeof st) == 0) {
			printf("pm_state   %u  sessions %u  generation %u  power_cycles %u\n",
			       st.pm_state, st.sessions, st.generation, st.power_cycles);
			printf("boot       stage %u  ret %d  step %u%s\n", st.boot_stage,
			       (int)st.boot_ret, st.boot_step,
			       (st.flags & PG106_UC_ST_READY) ? "  READY" : "");
			printf("vbios      %u bytes from %s\n", st.vbios_size,
			       st.vbios_source == 1 ? "ACPI _ROM" :
			       st.vbios_source == 2 ? "PCI ROM" : "nowhere");
			printf("firmware   present 0x%04x  missing 0x%04x\n",
			       st.fw_present, st.fw_missing);
			printf("memory     VRAM %u MiB  BAR1 %u MiB\n", st.vram_mib,
			       st.bar1_mib);
			printf("flags      %s%s%s%s%s\n",
			       (st.flags & PG106_UC_ST_ACPI) ? "acpi " : "",
			       (st.flags & PG106_UC_ST_D3COLD) ? "d3cold " : "",
			       (st.flags & PG106_UC_ST_D3COLD_OFF) ? "d3cold-disabled " : "",
			       (st.flags & PG106_UC_ST_CHANNEL_HELD) ? "channel-held " : "",
			       (st.flags & PG106_UC_ST_RENDER_USERS) ? "render-users " : "");
		}
	}

out:
	IOServiceClose(conn);
	return rc;
}
