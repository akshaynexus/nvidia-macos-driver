// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/stub/cxx_linkage.cpp — the kext's C++ glue calls the
// portable core's C API. Every core header must give its functions C
// linkage under C++, or the kext imports mangled names (e.g.
// __Z15pg106_pmc_probej for pg106_pmc_probe) that nothing exports and
// fails to load. This TU includes every header the glue can reach and
// references one function from each; the Makefile compiles it as C++ and
// fails if the object refers to any mangled pg106_ symbol.
//
// pascal/devinit.h is left out on purpose: its init-context struct has a
// field named `or`, a C++ alternative token, and no glue needs it. A C++
// TU that ever does must rename that field first.

#include <pascal/acr.h>
#include <pascal/acr_boot.h>
#include <pascal/ce.h>
#include <pascal/devboot.h>
#include <pascal/devinit_pmu.h>
#include <pascal/diag.h>
#include <pascal/disp_core.h>
#include <pascal/disp_mode.h>
#include <pascal/dispatch.h>
#include <pascal/edid.h>
#include <pascal/gr_boot.h>
#include <pascal/gr_ctx.h>
#include <pascal/hwexec.h>
#include <pascal/kdisp.h>
#include <pascal/mmio.h>
#include <pascal/mmu.h>
#include <pascal/pbdma.h>
#include <pascal/pm.h>
#include <pascal/pmc.h>
#include <pascal/pushbuf.h>
#include <pascal/qmd.h>
#include <pascal/top.h>
#include <pascal/uapi.h>
#include <pascal/vbios_bit.h>
#include <pascal/vbios_fetch.h>

#include "pg106_br_wire.h"
#include "pg106_device_gate.h"
#include "pg106_uc_abi.h"

typedef void (*any_fn)(void);
#define REF(f) reinterpret_cast<any_fn>(&f)

extern "C" any_fn pg106_cxx_linkage_refs[];
any_fn pg106_cxx_linkage_refs[] = {
	REF(pg106_pmc_probe),
	REF(pg106_device_gate),
	REF(pg106_start_decision),
	REF(pg106_devboot_run),
	REF(pg106_devboot_fini),
	REF(pg106_devboot_stage_name),
	REF(pg106_pm_init),
	REF(pg106_pm_done),
	REF(pg106_kd_create),
	REF(pg106_kd_handle),
	REF(pg106_vbios_fetch_first),
	REF(pg106_vbios_bit_parse),
	REF(pg106_hwexec_acr),
	REF(pg106_hw_pramin_write),
	REF(pg106_acrboot_plan_build),
	REF(pg106_grboot_plan_build),
	REF(pg106_mmu_init),
	REF(pg106_uapi_exec_validate),
	REF(pg106_top_scan),
	REF(pg106_gpfifo_pack),
	REF(pg106_fb_vram_size),
};
