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
