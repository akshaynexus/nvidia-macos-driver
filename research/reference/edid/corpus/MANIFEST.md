# EDID differential-test corpus

Real-world EDID dumps used by `pascal/tests/test_edid_corpus.c` and
`pascal/scripts/edid_corpus_diff.sh` to differential-test our parser
(`pascal/core/src/edid.c`) against `edid-decode`. 44 files (36 below, plus
the eight HDMI 2.0 dumps added 2026-11-08 at the end): 35 dumps
downloaded 2026-11-05 from the linuxhw/EDID repository (the dump collection
behind linux-hardware.org, licensed CC-BY-4.0) plus the pre-existing
`acer_ed320qr_s.bin` repo reference. EDID bytes are raw monitor data;
redistributed with that attribution.

## Coverage

- Desktop monitors 1080p (many vendors), 1440p (Acer/ASUS), 1680x1050,
  1280x1024, 1600x900
- 4K laptop panels (BOE 3840x2160), laptop panels (AU Optronics, BOE,
  LG Display, Apple), 3:2 (Aosiman 1920x1280), 16:10 (Apple/LGD/AUO)
- TV panels (Samsung SAM0B30, @TV), including interlaced first DTDs
  (AOC2255, @TV: the "+0.5 Odd/Even Field" form)
- High refresh: AUO 1920x1080 @ 144 Hz, AUO 2560x1600 @ 90 Hz, plus the
  165 Hz Acer ED320QR S reference
- Extension coverage: no extension (128 B), CTA-861 (256 B), multi-extension
  (512/768 B), base blocks with declared-vs-found extension mismatches
- Sync polarity mix (Hpol/Vpol P and N), analog and digital input bytes

## Normalization applied to the downloaded text dumps

Each source file is an `edid-decode (hex):` text dump (16 hex bytes per
line, blank line between blocks). Conversion: concatenate the hex bytes of
the hex section into one blob. The known dump-side duplication artifact
(whole 128-byte blocks repeated, as documented for the Acer reference in
../README.md) was handled as follows: **consecutive identical 128-byte
blocks are deduplicated only when the resulting blob length is not a
multiple of 128**; for every file in this corpus the raw byte count was
already a whole number of 128-byte blocks (128/256/512/768), so no file
actually needed the dedupe pass and all blobs were kept byte-for-byte as
dumped. Files whose length could not be normalized to a multiple of 128
would be flagged in the harness and skipped; none occurred.

## Files

| File | Upstream path | Source |
|---|---|---|
| `@TV_@TV0001_E0D5C59870A4.bin` | `Digital/@TV/@TV0001/E0D5C59870A4` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/@TV/@TV0001/E0D5C59870A4 (sha256 `f1d31c0805131904ac53fc3b0631fcdbdeda3f253acc27a20dd401b6eda83f7e`) |
| `AOC_AOC0001_0420EB9741E8.bin` | `Digital/AOC/AOC0001/0420EB9741E8` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/AOC/AOC0001/0420EB9741E8 (sha256 `7af16ebbaab7b724a6347aca5bb07a90889ce792c2e524d8a22f3f721ed3db63`) |
| `AOC_AOC2255_AD5F3185742C.bin` | `Digital/AOC/AOC2255/AD5F3185742C` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/AOC/AOC2255/AD5F3185742C (sha256 `97dea1093e7effb56fb0fd333db4f09846aa4908e75c8625617f72ab47bc08ef`) |
| `AOC_AOC2270_000FDC582001.bin` | `Digital/AOC/AOC2270/000FDC582001` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/AOC/AOC2270/000FDC582001 (sha256 `7083918a0b555593c83810f746efe52518918716b943e28f0fe32cc9a8a21ed3`) |
| `AOpen_AOP0948_3B3EB3E42461.bin` | `Digital/AOpen/AOP0948/3B3EB3E42461` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/AOpen/AOP0948/3B3EB3E42461 (sha256 `8cb69e4be64ca64d983f44397b79d331f9e5ac2b937c7df88086797736a9e95c`) |
| `APT_APT1245_57D0654D2449.bin` | `Digital/APT/APT1245/57D0654D2449` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/APT/APT1245/57D0654D2449 (sha256 `17ac4d8536ebced0dbdcc70a1fba29242f100cae13c6cfc8ee736011a9558ed7`) |
| `ASRock_ASRACA7_7E618F8FD49D.bin` | `Digital/ASRock/ASRACA7/7E618F8FD49D` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/ASRock/ASRACA7/7E618F8FD49D (sha256 `38908efbe41e8c2f4a5f0b06ee8eb46ef733e42815398e1d0dc2835d3ab25045`) |
| `ASUS_AUS22DA_05F20943A191.bin` | `Digital/ASUS/AUS22DA/05F20943A191` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/ASUS/AUS22DA/05F20943A191 (sha256 `94348d97afd84cee7160bfbde9d28a07cd0772dbfcb1cf2da45cabe33a959a96`) |
| `ASUS_AUS27B6_44939D31CCE6.bin` | `Digital/ASUS/AUS27B6/44939D31CCE6` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/ASUS/AUS27B6/44939D31CCE6 (sha256 `1a81ccdd170df0e832d9f1a3e75c001f72550e7b5de63b42e43492872598d7da`) |
| `ASUS_AUS27D2_009D3409E5C0.bin` | `Digital/ASUS/AUS27D2/009D3409E5C0` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/ASUS/AUS27D2/009D3409E5C0 (sha256 `1928d00deeae1373bf8d6c06cd26d2e1186750598250da4a1f6e8e26082ef494`) |
| `AU_Optronics_AUO2077_03A4AE3068B7.bin` | `Digital/AU Optronics/AUO2077/03A4AE3068B7` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/AU Optronics/AUO2077/03A4AE3068B7 (sha256 `2098aac6fcf3e780e9fbf5f63b8502aba144ddfaf9f3f82c31f838846d65ce26`) |
| `AU_Optronics_AUO4444_2D2138E809D5.bin` | `Digital/AU Optronics/AUO4444/2D2138E809D5` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/AU Optronics/AUO4444/2D2138E809D5 (sha256 `aae08ae56d490fe803867bd097eec30219bae1b8f2f06c4dd51ed60e9dc85450`) |
| `AU_Optronics_AUOCBAC_55D8ADBDFD53.bin` | `Digital/AU Optronics/AUOCBAC/55D8ADBDFD53` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/AU Optronics/AUOCBAC/55D8ADBDFD53 (sha256 `b29a15bb57bab7d23e384d3fa983a8b828d8abe4e978ea4c182f68948ced01fa`) |
| `AU_Optronics_AUOF7AD_893808B85271.bin` | `Digital/AU Optronics/AUOF7AD/893808B85271` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/AU Optronics/AUOF7AD/893808B85271 (sha256 `2c4791ffd5261b40dfb78920b3ea2903e576ee32b965e67a85d4acc6d16143d2`) |
| `Acer_ACR0497_01CF1CDFD3A8.bin` | `Digital/Acer/ACR0497/01CF1CDFD3A8` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/Acer/ACR0497/01CF1CDFD3A8 (sha256 `8164dc67870af9b72aaa8bcc6acb6f36698042e93a9b73eb96f0352277308976`) |
| `Acer_ACR081D_0E9F5D263C37.bin` | `Digital/Acer/ACR081D/0E9F5D263C37` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/Acer/ACR081D/0E9F5D263C37 (sha256 `67c4ee7424a343313c1b98b5a30efbd7f94f970250026c679c05538d695dad28`) |
| `Acer_ACR0BDC_2335533BB391.bin` | `Digital/Acer/ACR0BDC/2335533BB391` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/Acer/ACR0BDC/2335533BB391 (sha256 `036f75378d0656d32819cb19fc0343a6862fc38f5d749255bc7c7812d604a9f9`) |
| `Amazon_AMZABC5_856F92326120.bin` | `Digital/Amazon/AMZABC5/856F92326120` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/Amazon/AMZABC5/856F92326120 (sha256 `cc25b8e86bbad779e2e3295e37c4c408763cc2698a8a77004450f4aade3e6c37`) |
| `Ancor_Communications_ACI20D1_89B8870601A2.bin` | `Digital/Ancor Communications/ACI20D1/89B8870601A2` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/Ancor Communications/ACI20D1/89B8870601A2 (sha256 `76142267121037c9297e602619dc94ea884f3dfa974343cb14ff4610953fb4fb`) |
| `Aosiman_ASM1240_B717F5D5FCDF.bin` | `Digital/Aosiman/ASM1240/B717F5D5FCDF` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/Aosiman/ASM1240/B717F5D5FCDF (sha256 `4b5bce179fccdd85be89beebf6038f1855eb1c748657cf712e01631692dd426d`) |
| `Apple_APPA02E_6AF0F2960A8A.bin` | `Digital/Apple/APPA02E/6AF0F2960A8A` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/Apple/APPA02E/6AF0F2960A8A (sha256 `2a33b413d05358be1f1de9abf1e45b9985d43f3f3dd73a142b6eecb96858f46e`) |
| `Apple_APPA033_0C6813FA033F.bin` | `Digital/Apple/APPA033/0C6813FA033F` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/Apple/APPA033/0C6813FA033F (sha256 `8f42686cfe42f8c0eb687bf5a6fe02722e92838071456429340b61b2bbb88086`) |
| `Arnos_Instruments_AIC0023_47E29C122115.bin` | `Digital/Arnos Instruments/AIC0023/47E29C122115` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/Arnos Instruments/AIC0023/47E29C122115 (sha256 `601b881ebe9b0c4914d9e17b96d9c486f21302d96e475cdd3dd2254197236f31`) |
| `BOE_BOE0708_A31E51E035CC.bin` | `Digital/BOE/BOE0708/A31E51E035CC` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/BOE/BOE0708/A31E51E035CC (sha256 `e2734c02421469dea2115dfdeb687dc06864ceaad99629e827a9bd1496843242`) |
| `BOE_BOE0802_D158831929B8.bin` | `Digital/BOE/BOE0802/D158831929B8` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/BOE/BOE0802/D158831929B8 (sha256 `c8bab9fc002599b604238f3a1a7b0bb53a0ea7e35bac4d58e1a8881f9dad93ed`) |
| `BOE_BOE08F3_C912FA1DEAAC.bin` | `Digital/BOE/BOE08F3/C912FA1DEAAC` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/BOE/BOE08F3/C912FA1DEAAC (sha256 `3fd7c07f8f6a939b084ec4cb08019e1964cb1d77b354adc15e0186127ac2613a`) |
| `BOE_BOE097E_06054E7CC9E1.bin` | `Digital/BOE/BOE097E/06054E7CC9E1` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/BOE/BOE097E/06054E7CC9E1 (sha256 `88f21ae09174f544df0cdd087fd81dd65a42d1bd5315439e1817721cca74213e`) |
| `Belinea_MAX089A_44F90476DF22.bin` | `Digital/Belinea/MAX089A/44F90476DF22` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/Belinea/MAX089A/44F90476DF22 (sha256 `0c7632c17f35a4460a686c6a798905154e7621851f7ef1563c65cd29490513f7`) |
| `BenQ_BNQ7803_1DA8B99A8B52.bin` | `Digital/BenQ/BNQ7803/1DA8B99A8B52` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/BenQ/BNQ7803/1DA8B99A8B52 (sha256 `343e8849ef773c7b08ecbbc244d4e8b223ef340b75647b83b807fdd305d1e49c`) |
| `BenQ_BNQ7840_070F451FB774.bin` | `Digital/BenQ/BNQ7840/070F451FB774` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/BenQ/BNQ7840/070F451FB774 (sha256 `6bea8d48e359bed72d66b5c3a4055019a4e2f025fc178aad46ecd3fd516dec38`) |
| `LG_Display_LGD0000_09163E9A6BF1.bin` | `Digital/LG Display/LGD0000/09163E9A6BF1` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/LG Display/LGD0000/09163E9A6BF1 (sha256 `17379a94e2aeda9e944b3c22ce6ad39a79cfc5e4184153a80a3f3c67c5d4abaf`) |
| `LG_Display_LGD0697_99F7167ABDA0.bin` | `Digital/LG Display/LGD0697/99F7167ABDA0` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/LG Display/LGD0697/99F7167ABDA0 (sha256 `4c8cf2543c5505849e64e14a085c5db32dc78e147a78411b704803798d904586`) |
| `Samsung_SAM0B30_3A85AFF582A5.bin` | `Digital/Samsung/SAM0B30/3A85AFF582A5` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/Samsung/SAM0B30/3A85AFF582A5 (sha256 `87f9b82bcc443b5be6d13e0f67ec771950ab767154cd5220a8a9f9f664578cfe`) |
| `Samsung_SAM0B30_3FA61F0FF134.bin` | `Digital/Samsung/SAM0B30/3FA61F0FF134` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/Samsung/SAM0B30/3FA61F0FF134 (sha256 `eb476bb3ba06b71474def47f4b27c0b911a1c081e61745f519d49dff47876285`) |
| `Sony_MS_003C_5FD483E737AD.bin` | `Digital/Sony/MS_003C/5FD483E737AD` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/Sony/MS_003C/5FD483E737AD (sha256 `8c44d909c66184d30db881df4f090d429acf6a67682feda1208914e741d1eb80`) |
| `acer_ed320qr_s.bin` | `research/reference/edid/acer_ed320qr_s.bin (pre-existing repo reference; original source Digital/Acer/ACR0000/53D1F4B469C0)` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/Acer/ACR0000/53D1F4B469C0 |

### HDMI 2.0 additions (2026-11-08, disp3)

Eight dumps fetched 2026-11-08 from the same repository (CC-BY-4.0) for the
HDMI 2.0 mode work (pascal/core/src/disp_hw.c mode limits; DISP-PLAN.md §12).
Picked by searching linuxhw/EDID for HDMI-connected dumps (an HDMI LLC VSDB
plus an HDMI Forum VSDB with SCDC Present), i.e. EDIDs read over an HDMI
cable, not the DisplayPort variants of the same monitors. The sha256 is of
the upstream text dump.

**Normalization (differs from the 2026-11-05 set):** these dumps repeat the
whole EDID once (blocks 0..n then 0..n again; a linux-hardware.org capture
artifact). The blob kept is the first copy: (1 + extension count) blocks,
or, for the Dell S3225QC, the 3 blocks its HF-EEODB announces. The dropped
tail was checked to be byte-identical to the kept part.

| Monitor | What it exercises | Bytes |
|---|---|---|
| HP Z32x (`HP_HWP3277_D7586F224936.bin`) | 4K60 DTD 594 MHz, VIC 97/96, HDMI Max TMDS 300, HF 600 MHz + SCDC, audio | 512 -> 256 |
| HP V28 4K (`HP_HPN367F_BF3C94EBFA5F.bin`) | 4K60 DTD 594 MHz, VIC 97/96, HDMI 300, HF 600 + SCDC, audio | 256 |
| BenQ LCD (4K) (`BenQ_BNQ801E_A78A0EA63EEE.bin`) | 4K60 DTD 533.25 MHz + VIC 97, HDMI VSDB Max TMDS 600, HF 600 + SCDC, audio | 256 |
| ASUS XG248Q (`ASUS_AUS24B3_442CD2878048.bin`) | 1080p240 DTD 571 MHz, 200/144/120 Hz, HDMI 340, HF 590 + SCDC, audio | 512 -> 256 |
| Lenovo Y25-25 (`Lenovo_LEN66AA_776B1F6C591D.bin`) | 1080p240 DTD 571 MHz, HDMI 600, HF 600 + SCDC, YCbCr 4:2:0 VDB, audio | 256 |
| AOC Q24G2 (`AOC_AOCB205_455BB35C9238.bin`) | 1440p144 DTD 583.6 MHz, 1440p120 497.75 MHz, VIC 63, HDMI 330, HF 600 + SCDC | 512 -> 256 |
| LG HDR 4K (`Goldstar_GSM7706_BB5C4B11FE67.bin`) | 4K60 DTD 527.85 MHz, 4K30, HDMI 300, HF 600 + SCDC | 512 -> 256 |
| DELL S3225QC (`Dell_DELD1B9_9F88D4C9E48A.bin`) | base declares 1 extension; the CTA block's HF-EEODB overrides it to 2 (CTA + DisplayID); DisplayID Type I timings 4K120 (1188 MHz) and 1440p120 (497.75 MHz); VIC 97, HDMI 340, HF 600 + SCDC | 768 -> 384 |

| File | Upstream path | Source |
|---|---|---|
| `HP_HWP3277_D7586F224936.bin` | `Digital/HP/HWP3277/D7586F224936` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/HP/HWP3277/D7586F224936 (sha256 `31a5bc7d86f5fc978f3c908840b60b01b147e35bfb74296d1ad759aeb1182ef8`) |
| `HP_HPN367F_BF3C94EBFA5F.bin` | `Digital/HP/HPN367F/BF3C94EBFA5F` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/HP/HPN367F/BF3C94EBFA5F (sha256 `b1965c826344e18fba6e860b784179ccd2962fd93f734cfacc598070e5471177`) |
| `BenQ_BNQ801E_A78A0EA63EEE.bin` | `Digital/BenQ/BNQ801E/A78A0EA63EEE` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/BenQ/BNQ801E/A78A0EA63EEE (sha256 `86128814cad8a683ae8e099b6a631e055c406d362a76a77eebaa5b226d2a79d3`) |
| `ASUS_AUS24B3_442CD2878048.bin` | `Digital/ASUS/AUS24B3/442CD2878048` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/ASUS/AUS24B3/442CD2878048 (sha256 `d04897c16e8c3d5885b3ddec6cd88962521ca85a3152d87b08206ce0eea83621`) |
| `Lenovo_LEN66AA_776B1F6C591D.bin` | `Digital/Lenovo/LEN66AA/776B1F6C591D` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/Lenovo/LEN66AA/776B1F6C591D (sha256 `f6cfcd1648f1e8f63952296abd418f40805738a804b5d14ccb6988cbfd68da41`) |
| `AOC_AOCB205_455BB35C9238.bin` | `Digital/AOC/AOCB205/455BB35C9238` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/AOC/AOCB205/455BB35C9238 (sha256 `7c83d707fc18c154e5ec602dcbc01ec59ebf1d81bae940dce45d7659f453bcae`) |
| `Goldstar_GSM7706_BB5C4B11FE67.bin` | `Digital/Goldstar/GSM7706/BB5C4B11FE67` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/Goldstar/GSM7706/BB5C4B11FE67 (sha256 `8702068f2c96debdfdbe37a55dbc742447641540263e86b9d890e06ed11d3bc0`) |
| `Dell_DELD1B9_9F88D4C9E48A.bin` | `Digital/Dell/DELD1B9/9F88D4C9E48A` | https://raw.githubusercontent.com/linuxhw/EDID/master/Digital/Dell/DELD1B9/9F88D4C9E48A (sha256 `c950b1bff1b427f69b704c5d9bf5c53a71058b2879fd7d5348c265b8860ecd62`) |

Verify a file: `sha256sum -c` is not wired up; compare against the hashes above (the harness prints nothing about them — they are provenance only).
