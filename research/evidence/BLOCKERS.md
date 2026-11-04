# BLOCKERS — items only the owner can resolve

Format: what is needed, why, how to provide, what unblocks.

## B1. Helios 300 VBIOS dump (Phase 1)

Phase 1's gate is a DEVINIT register-write trace for **this machine's** VBIOS.
Without it the interpreter runs against a substitute.

How to provide (any one):
- From Linux on the Helios 300: `echo 1 > /sys/bus/pci/devices/0000:01:00.0/rom`
  then `cat /sys/bus/pci/devices/0000:01:00.0/rom > helios300-gp106.rom`
  (dGPU PCI address may differ; `lspci -d 10de:` to find it).
- From GPU-Z on Windows: save the BIOS file.
Drop it at `research/firmware/vbios/helios300-gp106.rom`.

Until provided: a legitimately-obtainable GP106 VBIOS substitute will be used
and marked, as the handoff allows.

## B2. macOS-only verification (Phases 3-6, standing)

All kext compilation (MacKernelSDK), IOKit probing, NVK-on-bridge behavior and
any display work can only be verified on the owner's Mac / Helios 300.
Owner-run build scripts and runbooks are produced instead; per handoff §3 the
owner's hardware is never touched by automation.

## B3. Real `nvidia/gp106/acr/` blobs (informational)

linux-firmware master (2026-11-05) ships no `nvidia/gp106/acr/` directory;
upstream nouveau's `gp102_acr_new` wiring means the `nvidia/gp102/acr/` blobs
committed under `research/firmware/gp106/acr/` are the ones the driver would
actually request. If NVIDIA ever publishes GP106-specific ACR binaries, replace
the files — the parser tests need no changes.
