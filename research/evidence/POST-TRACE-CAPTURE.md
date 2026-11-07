# POST-TRACE-CAPTURE — closing the Phase 1 gate on the real Helios 300

Phase 1's gate needs the interpreter to reproduce a known-good register-write
trace for this machine's VBIOS. The model side is ready:
- `helios300-gp106.rom` (86.06.4F.00.08) runs `gm200_devinit_post` to
  completion in simulation;
- that run is 9,643 steps, of which 9,634 are host-issued falcon load/boot
  writes.

The known-good side has to come from the hardware.

The capture was approved by the owner on 2026-11-07, but it could not be run
remotely: stopping the display manager over SSH was refused by the session's
permission policy. It is therefore an owner-run step.

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
