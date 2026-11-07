// SPDX-License-Identifier: MIT
/*
 * pg106_kif.h - the in-kernel interface between the GP106's GPL-2.0 kexts
 * (PG106, the GPU owner, and PG106FB, the HDMI IOFramebuffer) and the
 * PolyForm-NC accelerator PG106Accel. It is MIT so either side may include
 * it. It carries no code: only names, numbers and request layouts.
 *
 * Transport: IOKit only. No side links against or includes the other's
 * code; every call is a virtual IOService method on the other side's
 * registered service:
 *
 *   svc->callPlatformFunction(OSSymbol::withCStringNoCopy(PG106_KIF_FN_x),
 *                             false, &req, (void *)(uintptr_t)PG106_KIF_ABI,
 *                             nullptr, nullptr);
 *
 * Calls *to* the accelerator go the same way, with the client service that
 * the accelerator passed in the request (`client`), and the PG106ACCEL_KIF_FN_*
 * names.
 *
 * Versioning. PG106_KIF_ABI is major << 16 | minor. A callee answers
 * kIOReturnUnsupported (0xe00002c7) without touching req when the caller's
 * major differs from its own, or when the caller's minor is newer than its
 * own. Minor bumps only append fields at the end of a request and new
 * functions. Every request starts with { abi, size }: the caller sets abi =
 * PG106_KIF_ABI and size = sizeof(request); a callee reads at most `size`
 * bytes and writes back its own PG106_KIF_ABI into abi. Each provider also
 * publishes the ABI it implements as a registry number, so a client can see
 * the version (and decide whether to attach) before making any call:
 *   PG106    -> "PG106KifABI"
 *   PG106FB  -> "PG106FBKifABI"
 *
 * Returns are IOReturn. kIOReturnNotReady: the GPU is not booted
 * (PG106Ready false, or powered off). kIOReturnNoResources: out of VRAM,
 * BAR1, address space or channels. kIOReturnBusy: see FB_FLIP.
 *
 * Calling context: every function may block (it is called from a thread,
 * never from a primary interrupt) unless it says otherwise, and none may
 * be called with the callee's own gate held (the callee takes its gate).
 *
 * Status: ABI 1.0, agreed 2026-11-08 by PG106Accel (accel2), PG106 (kdev3)
 * and PG106FB (disp2). The canonical copy is pascal/bridge/include/pg106_kif.h.
 * Nothing behind it has run on a GP106 yet.
 */
#ifndef PG106_KIF_H
#define PG106_KIF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PG106_KIF_ABI_MAJOR 1u
#define PG106_KIF_ABI_MINOR 0u
#define PG106_KIF_ABI ((PG106_KIF_ABI_MAJOR << 16) | PG106_KIF_ABI_MINOR)
/* The rule above, for a callee implementing `mine` asked by a caller at `theirs`. */
#define PG106_KIF_ABI_ACCEPTS(mine, theirs) \
	(((mine) >> 16) == ((theirs) >> 16) && ((theirs) & 0xffffu) <= ((mine) & 0xffffu))

/* ======================================================================
 * PG106 (the GPU owner): VRAM, GPU address space, a copy channel, fences.
 * Answered on the "PG106" service.
 * ====================================================================== */

#define PG106_KIF_FN_INFO        "org.helios300.pg106.kif.info"
#define PG106_KIF_FN_VRAM_ALLOC  "org.helios300.pg106.kif.vram_alloc"
#define PG106_KIF_FN_VRAM_FREE   "org.helios300.pg106.kif.vram_free"
#define PG106_KIF_FN_VA_ALLOC    "org.helios300.pg106.kif.va_alloc"
#define PG106_KIF_FN_VA_FREE     "org.helios300.pg106.kif.va_free"
#define PG106_KIF_FN_MAP         "org.helios300.pg106.kif.map"
#define PG106_KIF_FN_UNMAP       "org.helios300.pg106.kif.unmap"
#define PG106_KIF_FN_CHAN_OPEN   "org.helios300.pg106.kif.chan_open"
#define PG106_KIF_FN_CHAN_CLOSE  "org.helios300.pg106.kif.chan_close"
#define PG106_KIF_FN_COPY        "org.helios300.pg106.kif.copy"
#define PG106_KIF_FN_RELEASE     "org.helios300.pg106.kif.release"
#define PG106_KIF_FN_FENCE       "org.helios300.pg106.kif.fence"

/* INFO caps bits */
#define PG106_KIF_CAP_VRAM    (1u << 0) /* VRAM_ALLOC/FREE work */
#define PG106_KIF_CAP_BAR1    (1u << 1) /* VRAM_ALLOC can give a CPU mapping */
#define PG106_KIF_CAP_VM      (1u << 2) /* VA_ALLOC/FREE, MAP/UNMAP work */
#define PG106_KIF_CAP_COPY    (1u << 3) /* a copy-engine channel can be opened */
#define PG106_KIF_CAP_RELEASE (1u << 4) /* semaphore releases on that channel */

struct pg106_kif_info {
	uint32_t abi, size;
	/* out */
	uint32_t caps;               /* PG106_KIF_CAP_* that work right now */
	uint32_t ready;              /* 1 = booted (same meaning as PG106Ready) */
	uint64_t vram_bytes;         /* all of VRAM */
	uint64_t vram_avail_bytes;   /* what VRAM_ALLOC may still hand out */
	uint64_t bar1_phys;          /* CPU physical address of BAR1 */
	uint64_t bar1_bytes;
	uint64_t bar1_avail_bytes;
	uint64_t va_base, va_bytes;  /* the window VA_ALLOC hands out from */
	uint32_t page_bytes;         /* small page size of the accelerator VM (4096) */
	uint32_t pad0;
};

/* One GPU address space ("the accelerator VM") is created by PG106 for the
 * accelerator. Every VA below, and every channel opened through this
 * interface, belongs to it. It is not NVK's address space. */

/* VRAM_ALLOC flags */
#define PG106_KIF_VRAM_CONTIG  (1u << 0) /* one physically contiguous range */
#define PG106_KIF_VRAM_SCANOUT (1u << 1) /* display-engine readable (implies CONTIG,
					  * pitch-linear, 4 KiB aligned at least) */
#define PG106_KIF_VRAM_CPU     (1u << 2) /* map through BAR1: bar1_phys + kva */
#define PG106_KIF_VRAM_GPU_VA  (1u << 3) /* also map into the accelerator VM: gpu_va */

struct pg106_kif_vram {
	uint32_t abi, size;
	/* in */
	uint64_t bytes;
	uint64_t align;              /* power of two, 0 = 4096 */
	uint32_t flags;              /* PG106_KIF_VRAM_* */
	uint32_t pad0;
	/* out (VRAM_ALLOC); in: handle (VRAM_FREE) */
	uint64_t handle;             /* nonzero, opaque */
	uint64_t vram_offset;        /* VRAM physical address (what display ctxdmas use) */
	uint64_t actual_bytes;
	uint64_t bar1_phys;          /* CPU physical address, 0 without PG106_KIF_VRAM_CPU */
	uint64_t kva;                /* kernel virtual address of that mapping, 0 without */
	uint64_t gpu_va;             /* 0 without PG106_KIF_VRAM_GPU_VA */
	uint64_t cookie;             /* unique per allocation for the boot (never reused) */
};

struct pg106_kif_va {
	uint32_t abi, size;
	uint64_t bytes;              /* in */
	uint64_t align;              /* in, power of two, 0 = page_bytes */
	uint64_t gpu_va;             /* out (VA_ALLOC); in (VA_FREE) */
};

/* MAP kinds */
#define PG106_KIF_MAP_VRAM   1u /* target = a VRAM_ALLOC handle + offset */
#define PG106_KIF_MAP_SYSMEM 2u /* target = an IOMemoryDescriptor the caller has
				 * prepare()d and keeps prepared until UNMAP */
/* MAP flags */
#define PG106_KIF_MAP_READONLY (1u << 0)

struct pg106_kif_map {
	uint32_t abi, size;
	/* in */
	uint64_t gpu_va;             /* inside a VA_ALLOC range, page aligned */
	uint64_t bytes;              /* page multiple */
	uint32_t kind;               /* PG106_KIF_MAP_* */
	uint32_t flags;
	uint64_t vram_handle;        /* MAP_VRAM */
	uint64_t offset;             /* into the VRAM allocation or the descriptor */
	void    *md;                 /* MAP_SYSMEM: (IOMemoryDescriptor *) */
	/* UNMAP takes gpu_va + bytes from the same request */
};

/* CHAN_OPEN kinds */
#define PG106_KIF_CHAN_COPY 1u /* a copy engine (GP106: PASCAL_DMA_COPY_B, 0xc1b5) on its own channel */

struct pg106_kif_chan {
	uint32_t abi, size;
	/* in */
	uint32_t kind;
	uint32_t pad0;
	/* in: the accelerator service. PG106 calls PG106ACCEL_KIF_FN_EVENT on it
	 * (from PG106's work loop) when the completed sequence number advances
	 * and when the channel is lost. PG106 retains it until CHAN_CLOSE. */
	void    *client;             /* (IOService *) */
	uint64_t client_tag;         /* echoed in every event */
	/* out (CHAN_OPEN); in (CHAN_CLOSE) */
	uint64_t chan;               /* nonzero, opaque */
};
/* While any channel is open PG106 keeps the GPU booted (as a render session
 * does: PG106::openSession). If it must go away anyway (GPU lost, PG106
 * stopping), it sends PG106_KIF_EVENT_LOST once and every handle of that
 * client is dead; the client then closes them (CHAN_CLOSE/VRAM_FREE are
 * accepted and do nothing). */

/* COPY: a pitch-linear 2D copy on the channel's copy engine. Linear is
 * height = 1. Both addresses are accelerator-VM addresses. */
struct pg106_kif_copy {
	uint32_t abi, size;
	/* in */
	uint64_t chan;
	uint64_t src_va, dst_va;
	uint32_t src_pitch, dst_pitch;
	uint32_t width_bytes, height;
	uint32_t flags;              /* 0 */
	uint32_t pad0;
	/* out */
	uint64_t seqno;              /* completes when the copy has landed */
};

/* RELEASE: after everything submitted before it on the channel, the GPU
 * writes the 32-bit value to gpu_va (a 4-byte semaphore release, with the
 * WFI + membar the channel needs for the write to be visible to the CPU). */
struct pg106_kif_release {
	uint32_t abi, size;
	/* in */
	uint64_t chan;
	uint64_t gpu_va;             /* 4-byte aligned */
	uint32_t value;
	uint32_t flags;              /* PG106_KIF_RELEASE_IRQ: also raise an event when it lands */
	/* out */
	uint64_t seqno;
};
#define PG106_KIF_RELEASE_IRQ (1u << 0)

/* FENCE ops */
#define PG106_KIF_FENCE_QUERY 0u /* completed = the channel's completed seqno */
#define PG106_KIF_FENCE_WAIT  1u /* block up to timeout_us for seqno */

struct pg106_kif_fence {
	uint32_t abi, size;
	/* in */
	uint64_t chan;
	uint32_t op;
	uint32_t timeout_us;
	uint64_t seqno;
	/* out */
	uint64_t completed;
};
/* Sequence numbers are per channel, start at 1, increase by one per COPY or
 * RELEASE, and complete in order. WAIT answers kIOReturnTimeout. */

/* ======================================================================
 * PG106FB (the HDMI head's IOFramebuffer): scanout and flips.
 * Answered on the PG106FB service. PG106Accel attaches to a PG106FB whose
 * provider is a PG106 on the same IOPCIDevice as itself, and to nothing else.
 * ====================================================================== */

#define PG106FB_KIF_FN_INFO "org.helios300.pg106fb.kif.info"
#define PG106FB_KIF_FN_FLIP "org.helios300.pg106fb.kif.flip"

/* Pixel formats: IOSurface fourccs. Only 'BGRA' (32-bit, A8R8G8B8 in the
 * display's terms) is required. */
#define PG106_KIF_FMT_BGRA 0x42475241u

struct pg106fb_kif_info {
	uint32_t abi, size;
	/* out */
	uint32_t head;               /* display head index */
	uint32_t online;             /* 1 = a mode is set and the head scans out */
	uint32_t width, height;      /* the current mode's active area */
	uint32_t pitch;              /* bytes per row of the framebuffer's own scanout */
	uint32_t format;             /* PG106_KIF_FMT_* */
	uint64_t vram_offset;        /* the framebuffer's own scanout surface */
	uint64_t vram_bytes;
	uint64_t bar1_phys;          /* its CPU physical address, 0 if none */
	uint64_t kva;                /* its kernel mapping, 0 if none */
	uint32_t refresh_us;         /* one frame period of the current mode */
	uint32_t generation;         /* bumps on every mode change */
};

/* FB_FLIP flags */
#define PG106FB_KIF_FLIP_HOME  (1u << 0) /* back to the framebuffer's own scanout;
					  * vram_offset/width/height/pitch ignored */
#define PG106FB_KIF_FLIP_ASYNC (1u << 1) /* do not wait for vblank (tearing); optional */

struct pg106fb_kif_flip {
	uint32_t abi, size;
	/* in */
	uint64_t vram_offset;        /* a PG106_KIF_VRAM_SCANOUT allocation (or inside one) */
	uint32_t width, height, pitch, format;
	uint32_t flags;
	uint32_t generation;         /* from INFO; a stale one is refused (kIOReturnNotReady) */
	/* the accelerator service; PG106FB calls PG106ACCEL_KIF_FN_FLIP_DONE on it */
	void    *client;             /* (IOService *) */
	uint64_t cookie;             /* echoed in FLIP_DONE */
};
/* FB_FLIP answers:
 *   kIOReturnSuccess  queued; the base channel latches it at the next vblank
 *                     and FLIP_DONE follows exactly once;
 *   kIOReturnBusy     the previous flip has not reached the screen yet;
 *                     nothing was queued and no FLIP_DONE follows;
 *   kIOReturnNotReady no mode set, head off, GPU powered off, or a stale
 *                     generation;
 *   anything else     refused (geometry, format, offset outside VRAM).
 * A surface that was flipped to stays on screen, and must stay allocated,
 * until the FLIP_DONE of the flip that replaces it.
 *
 * Constraints (PG106FB, agreed 2026-11-08): vram_offset 4 KiB aligned (the
 * hardware minimum is 256 B, SURFACE_SET_OFFSET takes offset >> 8); pitch a
 * multiple of 256 and >= width * 4; width/height equal to the current mode's
 * active size (no scaling); 'BGRA' only. The base channel's ctxdma covers
 * all of VRAM, so any PG106_KIF_VRAM_SCANOUT allocation is reachable.
 * Busy means the previous flip's base notifier still reads NOT_BEGUN.
 * FLIP_DONE is sent from PG106FB's vblank interrupt path when the notifier
 * leaves NOT_BEGUN, on PG106's work loop with its gate held: the receiver
 * must not call PG106FB or PG106 from inside it. A flip still pending when
 * the head turns off or the GPU powers off gets FLIP_DONE with status
 * kIOReturnAborted (0xe00002eb), so no cookie is ever left without one.
 * INFO and FLIP may be called from any thread that does not hold PG106's
 * gate (PG106FB takes it inside).
 *
 * Boot-args on the PG106FB side: pg106fb=0 (no PG106FB at all),
 * pg106fb_accel=0 (PG106FB runs but does not publish PG106FBKifABI, so it
 * stays an unaccelerated IOFramebuffer). */

/* ======================================================================
 * PG106Accel (the accelerator): events back from PG106 and PG106FB.
 * Answered on the PG106Accel service (the `client` above). Called from the
 * caller's work loop; the accelerator does not block in them.
 * ====================================================================== */

#define PG106ACCEL_KIF_FN_EVENT     "org.helios300.pg106accel.kif.event"
#define PG106ACCEL_KIF_FN_FLIP_DONE "org.helios300.pg106accel.kif.flip_done"

#define PG106_KIF_EVENT_PROGRESS 1u /* completed advanced */
#define PG106_KIF_EVENT_LOST     2u /* the channel and every handle are dead */

struct pg106accel_kif_event {
	uint32_t abi, size;
	uint32_t event;              /* PG106_KIF_EVENT_* */
	uint32_t pad0;
	uint64_t client_tag;
	uint64_t chan;
	uint64_t completed;
};

struct pg106accel_kif_flip_done {
	uint32_t abi, size;
	uint64_t cookie;
	uint32_t status;             /* IOReturn: success = on screen now */
	uint32_t pad0;
	uint64_t vblank_ns;          /* PTIMER time of the vblank it latched at, 0 if unknown */
};

#ifdef __cplusplus
}
#endif

#endif /* PG106_KIF_H */
