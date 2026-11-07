# Helios 300 GP106 — pg106_hwtest hardware runs (2026-11-08)

HARDWARE EVIDENCE. These are `pascal/tools/pg106_hwtest` runs on the owner's
Acer Predator Helios 300: GP106M 10de:1c20, BOOT_0 0x136000a1, CachyOS on
Linux 7.3.1, with no driver bound. The coordinator ran each one with the
owner's approval. Runs 2-6 used `pascal/scripts/hw-run.sh`, and every stage
started from a fresh GPU: woken from D3cold, so unposted and with no WPR
lock.

Edits made before committing: firewall (UFW) log lines are removed and the
laptop home directory is shown as `~`. Nothing else was changed.

| Run | Binary (commit) | A: POST | B: FIFO + CE selftest | C: ACR/SEC2/GR/READY | What stopped it, and the fix |
| --- | --- | --- | --- | --- | --- |
| 1 | 8d18971 | FAIL -ETIMEDOUT | – | – | PMU falcon never reset → 39df02f (nouveau's register-level `nvkm_falcon_reset`) |
| 2 | 2d0315a | PASS | PASS | FAIL step 233 | Plan asserted SEC2 CPUCTL ALIAS_EN = 0 → 0a56594 (FALCON_START, decided at run time) |
| 3 | 0a56594 | PASS | PASS | FAIL GR plan build | 0x502608 = 0x40005 was clamped to 0xff → 386e794 (u8 like nouveau); eeb41e6 also fixes the tile map, the zcull words and 0x260 |
| 4 | eeb41e6 | PASS | PASS | FAIL GR discovery | GPC registers read 0xbadf1200 → c86d5e2 (nouveau's mc init, 0x200 = 0xffffffff) |
| 5 | c86d5e2 | PASS | PASS | FAIL SEC2 bootstrap | Reply wait too short in real time → bbdbca6 (nouveau's 1000 ms reply budget) |
| 6 | bbdbca6 | PASS | PASS | **PASS** | — |

GR self-test runs (`C_ARGS=--gr`, after READY; logs in `grctx-runN/`):

| Run | Binary (commit) | Golden context | Dispatch | What stopped it, and the fix |
| --- | --- | --- | --- | --- |
| grctx-4 | bbdbca6 (+ --gr) | PASS: FECS image 0x87500, 61 mmio pairs | FAIL: GR trap 0x400100 bit 21, GP_GET 1 | hand-made SM61 control words suspected → dfed358 (NAK's encodings, three submissions, trap registers decoded) |
| grctx-5 | dfed358 | PASS | FAIL at step 2: SKED 0x407020 = 0x1000 TOTAL_TEMP_SIZE at SEND_SIGNALING_PCAS_B | QMD CRS 0x800 with no local-memory allocation on the channel → 09fa5f3 (SLM 0 + CRS 0 like NAK's runner on this GPU, INVALIDATE_SKED_CACHES) |
| grctx-6 | 7853fd0 | PASS | **PASS**: EXIT kernel, QMD RELEASE0, host semaphore, store kernel wrote 0x6a5ec0de | — |

## grctx-run6: the GR self-test passes on the real GPU (binary sha256 32d4c16d…a62209)

`C.txt` line 41:

    gr selftest: ret 0; runlist 0 chid 0; golden image 0x87500 bytes (FECS
    0x409800 0x00000001); ctx VA 0x310250000, mmio list 61 pairs; QMD release
    0x6a5e0001 (10 us); host sem 0x6a5e0002; store 0x6a5ec0de (want
    0x6a5ec0de, 10 us); GP_GET 3; GR_INTR 0x00000000 FECS_INTR 0x00000000
    PFIFO_INTR 0x80000000 GR_STATUS 0x00000000; MMU faults 0, PBDMA errors 0;
    non-stall events 2

What this proves on the GP106, through `pascal/core` alone:
- the golden context our port generates is one the HUB/GPC ucode loads and
  runs a channel on (FECS image 0x87500, 61 mmio pairs, context at
  inst+0x210);
- PASCAL_COMPUTE_B binds and takes NVK's compute state on that channel;
- a v02_01 QMD built by `pg106_qmd_build` launches: the SMs run NAK's SM61
  words, RELEASE0 writes its payload at grid completion (10 µs), the host
  semaphore with WFI follows;
- the store kernel reads the constant buffer (c[0][0x140], the store VA)
  and STG.E lands in VRAM: 0x6a5ec0de;
- no GR interrupt, no MMU fault, no PBDMA error; GP_GET 3 = all three
  submissions consumed.

`PFIFO_INTR 0x80000000` in both self-test lines is bit 31, the engine
non-stall line (`gk104_fifo_intr`), raised by the NON_STALL_INTERRUPT method
at the end of our own submissions. The register is sampled before the
interrupt service acknowledges it; the service did run (non-stall events 1
after the CE test, 2 after GR). Since 480dba8 the self-tests read the register
again after the service, require 0 there and exactly one new non-stall event,
and the harness prints `PFIFO_INTR 0x80000000 -> 0x00000000 after the
service`.

Afterwards nvidia loaded and GDM came back (`run.log`, `dmesg.txt`).

## grctx-run5: the GR self-test's first two steps (binary from dfed358)

- **Golden context:** generated on the GPU again: FECS replied 0x87500,
  the channel context sits at VA 0x310250000 with a 61-pair mmio list.
- **Step 1 (class bind):** SET_OBJECT(PASCAL_COMPUTE_B) + NVK's compute
  state + a host semaphore (0x6a5e0003) completed: the context loaded and
  the class bound (ctxsw 0x409b00 = 0x8017fe8f, the channel's instance).
- **Step 2 (EXIT dispatch):** GR_INTR 0x00200000 (exception), TRAP
  0x00000100 (SKED), FE method 0x02bc SEND_SIGNALING_PCAS_B data 3,
  SKED 0x407020 = 0x00001000 = TOTAL_TEMP_SIZE in nouveau's
  `gk104_sked_error`; all GPC/TPC/MP error registers 0; GP_GET 2. The
  launch was rejected before any SM work: the QMD's
  SHADER_LOCAL_MEMORY_CRS_SIZE 0x800 (gallium's figure, which gallium backs
  with its TLS area) had no SET_SHADER_LOCAL_MEMORY_* allocation behind it.
  NAK's hardware tests, which passed on this GPU under nouveau (NVK run 3,
  `nak-hw.log`), dispatch with CRS 0 and no local memory at all.
- The `host sem 0x00000000` in the result line is step 2's host
  semaphore, behind the stalled dispatch; step 1's own word was polled and
  landed.

## Run 6: the full bring-up on the real GPU (binary sha256 fb15ea1d…a8607dfb7)

- **POST:** DEVINIT done after 22 polls. Two PMU falcon resets.
- **mc init:** PMC_ENABLE 0x40002121 → 0x5c6cf1e1, the same value nouveau
  reaches.
- **B:** VRAM 6144 MiB, FIFO up. The self-test channel is runlist 5, chid 0.
  The host semaphore was released after 60 µs, the CE semaphore after 50 µs,
  and the 64 KiB VRAM→VRAM copy had 0 mismatched words.
- **C:**
  - The HS ACR ran and the SEC2 RTOS started through 0x130 (ALIAS_EN set).
  - Init message: two queues, at 0x01000000 and 0x01000080 (EMEM).
  - GR discovery: 2 GPCs × 5 TPCs, PPC masks 0x9/0x12/0x4.
  - BOOTSTRAP_FALCON(FECS, GPCCS): both acknowledged. The GPCCS reply took
    4780 µs; nouveau's own trace shows 4.9 ms.
  - FECS and GPCCS started through 0x130. FECS replied with ctx size
    0x87500, the same value nouveau got. Compute class 0xc1c0.
  - Result: `ready ok bring-up complete`. The self-test passed again.
- **Restore:** afterwards nvidia loaded and drove the GPU normally
  (`dmesg.txt`).

`pg106_hwtest` reproduces exactly: `pascal/scripts/stage-hw-run.sh NAME
bbdbca6` rebuilds the same binary sha256.
