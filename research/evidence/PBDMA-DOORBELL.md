# PBDMA-DOORBELL.md — the GPFIFO doorbell / PUT question (PORT-AUDIT item 2)

Resolves the [RISK] flagged against `dispatch.c` in
`research/evidence/PORT-AUDIT.md` ("BAR0+0x90 doorbell sourced from
tinygrad/research doc, cross-check vs PBDMA path before hardware") and the
outstanding item 2 ("GPFIFO doorbell (BAR0+0x90 / PUT value) not yet
cross-checked against the PBDMA doorbell path").

**Status 2026-11-07:** this document's recommendation was not reflected in
code until `bb07dd5`: `dispatch.c` wrote BAR0+0x90 through the 2026-11-05
rounds. It now writes GP_PUT to USERD+0x8c (`PG106_SUB_USERD_GP_PUT`,
`PG106_SUB_STEP_GP_PUT`). The BAR0+0x90 A/B experiment in §Recommendation 3
remains a hardware-session item.

Question: when a driver publishes a GPFIFO entry, what exactly gets
written where, and with what value, to make the PBDMA (Host) fetch it?

All sources are read-only third-party references (submodule checkouts and
fetched upstream files); nothing is copied into the repo.

---

## 1. tinygrad (what the macOS port does today)

`third_party/tinygrad/tinygrad/runtime/ops_nv.py`:

- `nv_submit()` (lines 96-101) — the submission UOP, in order:
  1. store the 64-bit GPFIFO entry into `ring[put % n]`
     `(addr>>2<<2) | (words<<42) | (1<<41)` — LENGTH in the upper bits,
     bit 41 = LEVEL/SUBROUTINE (see `ENCODING-RESOLUTION.md` and
     `test_tinygrad_compat.c` section 5);
  2. store the new PUT index `(put+1) % n` into `gpput`;
  3. `memory_barrier()` (in the prior-art record,
     `research/prior-art/pascal-egpu/RESEARCH.md` line 229);
  4. store `token` into `doorbell` — **BAR0 MMIO + 0x90**
     (line 653: `Buffer("CPU", 4, options=BufferSpec(external_ptr=
     self.gpu_mmio.addr + 0x90))`).
- The `gpput` buffer is **not** in the ring: it is the channel control
  region, `gpput_off = offset + entries*8 + AmpereAControlGPFifo.GPPut.offset`
  (line 650). The control layout is the autogen `Nvc56fControl` struct
  (`tinygrad/runtime/autogen/nv_570.py`): `GPGet` at byte 0x88,
  `GPPut` at byte 0x8C.
- The doorbell **value** is `fifo.token`, obtained from RM control
  `NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN` (lines 647-649).

So tinygrad does **two** kicks: the PUT-index update in the channel
control page, then a token write to BAR0+0x90.

## 2. nouveau (where the kernel rings the bell)

Fetched from torvalds master to /tmp/pascal-ref/nouveau (paths relative
to `drivers/gpu/drm/nouveau/`):

- **Kepler..Pascal (our chip class): there is no doorbell register at
  all.** The USERD lives in **BAR1** (`nvkm/engine/fifo/gk104.c:114-118`,
  `gk104_chan_userd = { .bar = NVKM_BAR1_FB, .size = 0x200 }`, wired
  unchanged through gp100: `fifo->func->chan.func->userd->bar ==
  NVKM_BAR1_FB` at gk104.c:748). Submission = userspace writes the new
  GPFIFO PUT index straight into the BAR1-mapped USERD:
  `nvif/chan506f.c` `nvif_chan506f_gpfifo_kick()`:
  `nvif_wr32(&chan->userd, 0x8c, chan->gpfifo.cur)` — **USERD+0x8C =
  GPPut, value = new PUT entry index**; hardware reads it back at
  USERD+0x88 = GPGet (`nvif_chan506f_gpfifo_read_get`). The PBDMA polls
  GP_PUT; the write *is* the doorbell.
- **Volta+ is where the doorbell register appears**: USERD moves out of
  BAR1 into RAM (`gv100.c` `gv100_chan_userd` has no `.bar`), a
  `doorbell_handle` function appears (`gv100.c:35`,
  `gv100_chan_doorbell_handle` returns `chan->id`), and userspace rings
  the channel through the usermode page: `NVC361_NOTIFY_CHANNEL_PENDING`
  at offset **0x90** of `VOLTA_USERMODE_A` (0xC361).
- NVIDIA RM confirms the Volta+ value: `kfifoUpdateUsermodeDoorbell()`
  (`kernel/nvidia/src/kernel/gpu/fifo/kernel_fifo.c:4054-4087`) writes
  `workSubmitToken` to `NVC361_NOTIFY_CHANNEL_PENDING`; the token is
  built by the HAL, e.g. `kfifoGenerateWorkSubmitTokenHal_TU102`
  (`.../arch/turing/kernel_fifo_tu102.c:96-103`) as
  `NV_CTRL_VF_DOORBELL_VECTOR (chid) [11:0] |
   NV_CTRL_VF_DOORBELL_RUNLIST_ID [22:16]` (`dev_ctrl.h:36-37`).

## 3. NVIDIA SDK headers (submodule)

- `third_party/open-gpu-kernel-modules/src/common/sdk/nvidia/inc/class/
  clc06f.h:48-64` — `Nvc06fControl` ("PascalAControlGPFifo"): identical
  layout, `GPGet` 0x88 ("GP FIFO get offset, read only"), `GPPut` 0x8C
  ("GP FIFO put offset", read/write). The header's own comment points to
  `dev_pbdma.ref` chapter "User Control Registers", device NV_UDMA.
- `.../inc/class/clc361.h` — `NVC361_NOTIFY_CHANNEL_PENDING = 0x00000090`
  (VOLTA_USERMODE_A): the offset tinygrad pokes.
- `.../inc/swref/published/pascal/gp102/dev_pbdma.h` — **a 27-line stub**
  (only `NV_PBDMA_SET_CHANNEL_INFO_SCG_TYPE_COMPUTE1`). The published
  Pascal swref defines **no** doorbell register, and no
  `NV_PPBDMA_DOORBELL` exists anywhere in the SDK. Consistent with
  nouveau: on Pascal the doorbell *is* the USERD GPPut write.
- Reference docs (fetched from nvidia.github.io/open-gpu-doc to
  /tmp/pascal-ref; no Pascal dev_pbdma.ref is published — closest is
  Volta, whose FIFO_DMA format is the one clc06f.h implements):
  `manuals/volta/gv100/dev_pbdma.ref.txt` — GP entries are fetched when
  `NV_PPBDMA_GP_FETCH != NV_PPBDMA_GP_PUT`, i.e. hardware chases PUT;
  USERD is the writeback image of `NV_PPBDMA_GP_PUT(i)`
  (`dev_ram.ref.txt` RAMUSERD table: `NV_RAMUSERD_GP_PUT ->
  NV_PPBDMA_GP_PUT(i), Read/Write`).

## 4. Cross-check verdict

| source | bell write | value |
|---|---|---|
| tinygrad | control page +0x8C, then BAR0+0x90 | PUT index; then work-submit token |
| nouveau (GP106 class) | USERD+0x8C (BAR1) | new PUT entry index |
| nouveau (Volta+) | usermode page +0x90 (BAR0-mapped) | work-submit token (chid \| runlist<<16) |
| SDK clc06f.h | GPPut 0x8C / GPGet 0x88 | PUT index |

The three agree once split by generation: **on Pascal, the submission
kick is the GPPut write (USERD+0x8C, value = new PUT entry index)**.
BAR0+0x90 is the Volta-generation usermode doorbell
(`NVC361_NOTIFY_CHANNEL_PENDING`), written with the **work-submit token,
not the PUT** — tinygrad writes it unconditionally because it allocates
Volta-class channels (NVC36F) and reuses the same submit path on the
Mac port's GP106.

## 5. What our kext should do on GP106

1. Publish the GPFIFO entry in the ring, then write the new PUT **entry
   index** to GPPut (USERD+0x8C in the channel's USERD page), matching
   `nvif_chan506f_gpfifo_kick` and clc06f.h. `dispatch.c` already models
   the correct *value* (`new_put` index, `PG106_SUB_STEP_DOORBELL`;
   renamed `PG106_SUB_STEP_GP_PUT` and moved to USERD+0x8c in `bb07dd5`).
2. Treat BAR0+0x90 as **not part of the Pascal submission contract**. It
   is not in any published Pascal reference; on Volta+ it carries a
   token, not a PUT. Do not architect around it.
3. During hardware bring-up, run it as an experiment: submit with GPPut
   only; if the PBDMA does not advance (GPGet at USERD+0x88 stuck), then
   A/B-test a token write to BAR0+0x90 (token = chid, runlist 0 →
   value = chid, bits per `NV_CTRL_VF_DOORBELL_*`) and a plain 0 or 1
   write, and record which unpins it. This is the only piece of the
   question that cannot be settled from sources.

## 6. Remaining unknowns (hardware-only)

- Whether GP106's USERD write is snooped by the PBDMA immediately
  (doorbell-like) or polled at the `NV_PFIFO_USERD_WRITEBACK` cadence —
  sources do not state a latency; the polling model implies latency is
  irrelevant for correctness.
- What BAR0+0x90 decodes to on GP106 (register map slot unknown — it may
  be a harmless alias or an unrelated register; the write *could* in
  principle disturb something). Keep it behind a boot-arg until tested.
- Whether the Mac port's kext (which tinygrad drives via the RM
  user-client) even maps BAR1 USERD to userspace, or whether tinygrad's
  control-page mapping already lands on the right object — that is a
  kext UAPI decision, not a hardware one.

Evidence files: /tmp/pascal-ref/nouveau/** (fetched torvalds master),
/tmp/pascal-ref/volta-gv100-dev_pbdma.ref.txt,
/tmp/pascal-ref/volta-gv100-dev_ram.ref.txt, submodule
third_party/{open-gpu-kernel-modules,tinygrad}. Decoder that consumes
these layouts: `pascal/core/src/pbdma.c`, `pascal/tests/test_pbdma.c`.
