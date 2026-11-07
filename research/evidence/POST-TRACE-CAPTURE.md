# POST-TRACE-CAPTURE — closing the Phase 1 gate on the real Helios 300

Phase 1's gate needs the interpreter to reproduce a known-good register-write
trace for this machine's VBIOS. The model side is ready:
- `helios300-gp106.rom` (86.06.4F.00.08) runs `gm200_devinit_post` to
  completion in simulation;
- that run is 9,643 steps, of which 9,634 are host-issued falcon load/boot
  writes.

The known-good side has to come from the hardware.

## Result — 2026-11-07: Phase 1 gate MET on hardware

The owner granted permission, and the capture ran on the Helios 300 over
SSH with `capture_post_trace.sh`, followed by a reboot to restore.

- **nouveau:** `NVIDIA GP106 (136000a1)`, bios 86.06.4f.00.08, 6144 MiB
  GDDR5, PMU "firmware unavailable" (expected: nouveau has no Pascal PMU
  firmware), COPY used for buffer moves, no devinit, ACR or SEC2 errors.
- **Trace:** 548,905 mmiotrace lines, 493,118 writes, BAR0 at 0xa3000000.
  The raw trace stays local in `research/hardware/helios300/raw/`, gitignored
  because it carries NVIDIA firmware words. sha256 of `mmio.trace`:
  `46426f4ee7b711dd7d0ae4bf356e17392c57de278dec58d19714d22d4532876d`.
- **`compare_post_trace.py` result: `MATCH`.** All **9,634** modelled
  `gm200_devinit_post` writes equal the hardware writes, address and value,
  in order. That covers both PMU loads (DEVINIT + PRE_OS):
  - the IMEM/DMEM upload of the VBIOS app;
  - the `'I'` tables and boot scripts into DMEM;
  - MAILBOX0/BOOTVEC/CPUCTL start and the poll.

  The only hardware writes not modelled were three `0x10a040 = 0` writes
  inside the PRE_OS falcon reset. That is `nvkm_falcon_mask(0x040, 0, 0)` in
  `gm200_flcn_reset_wait_mem_scrubbing` (`falcon/gm200.c:143`), part of the
  reset the model abstracts as one step. The script attributes and prints
  them.
- **nouveau's own `vbios.rom`:** identical to the extracted
  `helios300-gp106.rom` for all 168,960 bytes (nouveau's copy is 4 KiB-padded).
- **SEC2 (B5, partly):** nouveau's SEC2 traffic goes to **0x087000**, with
  182 writes and 303 reads. That confirms the corrected base on this exact
  GPU. The last SEC2 MAILBOX0 read is 0.
- **GR (B10, partial):** FECS/GPCCS got 0 writes. nouveau initialises GR
  lazily, on first use by a client, and no 3D/compute client ran during the
  capture. Whether the gp102 FECS/GPCCS images boot on the GP106M is
  therefore still open. A repeat capture with a GR workload (e.g. a Vulkan
  or GL client on nouveau) answers it.

This is hardware evidence for the **POST** sequence only. It is not evidence
for the macOS kext, which has never run.

## Capture (on the Helios 300, Linux, ~1 minute)

```sh
# save your work first: this ends the graphical session until the reboot
sudo pascal/scripts/capture_post_trace.sh /var/tmp/posttrace
```

The script:
1. Stops GDM and unloads the NVIDIA modules.
2. Starts `mmiotrace`.
3. Loads nouveau with `config=NvForcePost=1 runpm=0`, which forces the full
   `gm200_devinit_post` even though the GPU is already posted.
4. Records for 30 s: POST, then nouveau's own ACR/SEC2/GR boot.
5. Saves the trace, dmesg, BAR0 base and nouveau's `vbios.rom`.

Rollback: `systemctl reboot` (nothing on disk is changed).

## Compare (on any Linux box with this repo)

```sh
cd pascal
scripts/compare_post_trace.py /path/to/posttrace     # copied from the laptop
```

The script builds `tools/post_trace_model.c`, runs the model on the
Helios ROM, and compares, in order, every host write to the PMU falcon load
and boot registers against the hardware trace from the first IMEMC write
onwards. The registers compared are IMEMC/T/D, DMEMC/D, MAILBOX0, BOOTVEC and
CPUCTL. Falcon reset is abstracted in the model and excluded.

The script reports `MATCH` (exit 0) or the first mismatching step with its
phase (exit 1). It was self-tested on a synthetic trace built from the
model's own writes:
- identical trace → MATCH, 9,634 writes;
- one value mutated → MISMATCH at step 49.

## What else the same capture answers

- **BLOCKERS B10:** whether the gp102-family ACR/SEC2/GR firmware boots on a
  GTX 1060 Mobile. Look at nouveau's dmesg lines for acr/sec2/gr in
  `dmesg-nouveau.txt`.
- **Phase 2:** a hardware ACR sequence to compare with the
  `PHASE2-ACR-TRACE.txt` model. No automated diff exists for that yet; the
  SEC2 base and queue offsets in the trace settle B5.
