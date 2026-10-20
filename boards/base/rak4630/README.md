# `boards/base/rak4630/` — the base under the application

A RAK4630 arrives from the factory with RAK's RUI3, not the MBR, the SoftDevice
and the bootloader this firmware is linked against. The Walter writes that
base **once**, on a person's command, from the release's `provision` file
(`spec/SYSTEM.md` S-5a; `spec/FIRMWARE-MANIFEST.md` §4a). Nobody here built
the file. This directory is where it came from, what it is, and the pin that
`tools/base/check.py` holds it to on every run of `tools/verify.ps1` and in
every release. Nothing here is edited: a different file is a different base,
and the pin, this page and `THIRD-PARTY-NOTICES` change together or not at all.

## The file

| | |
|---|---|
| File | `wiscore_rak4631_board_bootloader-0.4.4_s140_6.1.1.hex`, byte for byte as downloaded (`.gitattributes`: binary, no line-ending conversion; LF, 11 592 lines) |
| SHA-256 | `e19bad05e2be9fbdd4f4ec564eb1ff605d495ffe42eb90410e5162792e1ca5bd` |
| Size | 509 804 bytes |
| Source | [`RAKWireless/WisBlock`](https://github.com/RAKWireless/WisBlock), commit [`d3da249c0e4df56fb972021cf6e99239f27857bf`](https://github.com/RAKWireless/WisBlock/commit/d3da249c0e4df56fb972021cf6e99239f27857bf) (2026-07-23, *Update RAK4631 bootloader to V0.4.4 with BLE OTA recovery*), path `bootloader/RAK4630/Latest/wiscore_rak4631_board_bootloader-0.4.4_s140_6.1.1.hex`, git blob `0d8085df5db7dcbddd6d96bd10be062fd351e256`. That is the only commit that has ever touched the file; `master` was `ae4137bb` on 2026-10-20 and carries the same blob. RAK publishes no release tag for it: `Latest/` is the release. |
| Bootloader | **0.4.4**, RAKwireless' build of the [Adafruit nRF52 Bootloader](https://github.com/adafruit/Adafruit_nRF52_Bootloader) for the WisCore RAK4631, built *Jun 17 2026*. The file says so itself: the `INFO_UF2.TXT` it serves reads `UF2 Bootloader 0.4.4` / `Model: WisBlock RAK4631 Board` / `Board-ID: WisBlock-RAK4631-Board` / `Date: Jun 17 2026` / `Ver: 0.4.4` / `SoftDevice: S140 6.1.1`, and RAK's `bootloader/RAK4630/README.md` at the same commit says the same |
| SoftDevice | **S140 6.1.1** — read from the SoftDevice information structure at `0x00003000` (`SD_ID` 140, `SD_VERSION` 6001001), which is what `boards/rak4631.json` names and what `nrf52840_s140_v6.ld` links the application above |
| In a release | `twinoak-base-rak4630-0.4.4-s140-6.1.1.hex`, `use: provision`, under every `rak4630-*` image (`docs/release.md`, *The base*) |

`base.json` beside it is the same facts for a program; `tools/base/check.py`
refuses a file whose hash, §4a summary or README differ from it.

## What `manifestref.py base` says of it

`python spec/tools/manifestref.py base boards/base/rak4630/wiscore_rak4631_board_bootloader-0.4.4_s140_6.1.1.hex 0x26000 0xED000`
— against the application region the release reads from the linker script,
`0x00026000`..`0x000ED000` (`docs/release.md`, *The application region*):

```
{
 "bootloader": "0x000F4000",
 "softdevice": "S140 6.1.1",
 "flash_bytes": 185328,
 "uicr_words": ["0x10001014", "0x10001018"],
 "mbr_params": "0x000FE000"
}
```

So: the MBR at `0x00000000`; the SoftDevice, ending exactly at `0x00026000`
where the application starts; nothing between there and the bootloader (the
application and the file system that holds the node's identity are never the
base's); the bootloader at `0x000F4000`, which is where `UICR.NRFFW[0]` points;
the MBR's parameter page at `0x000FE000` in `UICR.NRFFW[1]`; no other UICR
word — in particular **nothing in `UICR.APPROTECT`**, which the Walter writes
from the chip's `INFO.VARIANT` and never from a file (§4a rule 3). 185 328
bytes of flash in all. The bootloader's settings page at `0x000FF000`
(`boards/rak4631.json`, `settings_addr`) is not in the file: a fresh base has
no settings, and the bootloader writes that page itself.

## APPROTECT on a revision 3 part

A revision 3 nRF52840 (build code `F`, `INFO.VARIANT` `AAF0`) closes its debug
port at every pin, power or brownout reset unless `UICR.APPROTECT` is `0x5A`
**and** the firmware that boots copies that word into `APPROTECT.DISABLE`.
Nordic's MDK does that in `SystemInit` from version 8.40.2 on; a bootloader
built from an older MDK leaves the port closed after the Walter's one reset,
and with it S-5's whole SWD update path (`spec/SYSTEM.md` §10.2).

**Whether this bootloader's source tree uses MDK 8.40.2 or later: unknown from
the source — the first board says.** What was read, and what was not:

- RAK does not publish 0.4.4's source. The WisBlock README at the pinned commit
  says *"Full bootloader source is maintained separately"*, no repository of
  RAKwireless' on GitHub is that source, and the file came with no tag.
- The tree RAK *does* publish beside it —
  `bootloader/RAK4630/Latest/WisCore_RAK4631_Bootloader/`, the **0.4.3**
  source — is Adafruit's bootloader *"0.6.2-11 modification"* (its `Makefile`)
  with nrfx vendored in, and that nrfx's `mdk/nrf.h` says **MDK 8.29.0**: no
  `nrf52_handle_approtect`, no APPROTECT handling of any kind
  (`lib/nrfx/mdk/system_nrf52840.c` does not contain the word). Its
  `src/boards/wiscore_rak4631_board/board.h` is the RAK4631: `REGOUT0` 3.3 V,
  LEDs P1.03 and P1.04, USB `239A:0029`/`002A`, `UF2_BOARD_ID
  WisBlock-RAK4631-Board` — what the 0.4.4 file's strings say too.
- 0.4.4 is 0.4.3 plus *BLE OTA resiliency* (RAK's changelog line). Whether
  RAK also moved its nrfx is exactly what cannot be told without the source.
- The bootloader region of the file (`0x000F4000`–`0x000FD858`; **not** the
  SoftDevice, whose licence forbids it) was disassembled on this machine
  (`out/p3.4-01/`, `arm-none-eabi-objdump` from the pinned toolchain) and
  compared with two bootloaders of known MDK 8.29.0: RAK's 0.4.3 and Adafruit's
  0.9.1 for the PCA10056 (nrfx v2.0.0, the one the pinned Arduino core ships).
  All three reach UICR through `mov.w rX, #0x10001000` and touch only
  `REGOUT0` (`+0x304`), `PSELRESET` (`+0x200`, `+0x204`) and `NFCPINS`
  (`+0x20C`); none reads `APPROTECT` (`+0x208`) and none stores to
  `APPROTECT.DISABLE` (`0x40000558`) or `FORCEPROTECT` (`0x40000550`). That is
  evidence the 0.4.4 build has no APPROTECT handling either — but it is a
  reading of a binary without a positive control, not the source, so it is
  recorded here and decides nothing.

What decides it: the Walter's `rak provision` reports the port after its
reset (S-5a), and the first board's walk writes `INFO.VARIANT` and that line
down (`docs/bench.md` §0, *p3.4-01*). If the port reads closed on a revision 3
part, the hardware repository's `software/FIRST-UNIT.md` step 8 says to stop
and decide — a bootloader built from MDK 8.40.2 or later — before anything
else. The application does not rescue it: the pinned Arduino core
(`framework-arduinoadafruitnrf52`, MeshCore's fork, `nordic/nrfx/mdk/nrf.h`)
is **MDK 8.30.2** and does not touch `APPROTECT` either.

## The two candidates, and the three questions

The task weighed two files by the same three questions, in this order of
preference: passes §4a; built for the RAK4631; MDK 8.40.2 or later.

| | RAK's `wiscore_rak4631_board_bootloader-0.4.4_s140_6.1.1.hex` | Adafruit's release for the RAK4631 |
|---|---|---|
| Passes §4a? | **Yes**, every rule, against the release's region (above) | There is no such file: `adafruit/Adafruit_nRF52_Bootloader` has no `wiscore_rak4631_board` under `src/boards/` (51 boards at tag `0.11.0`, none RAK's) and none of its 33 releases, `0.1.0` to `0.11.0` (2026-05-21), carries an asset with *rak* in its name. Its PCA10056 file of the same shape passes §4a, for what that is worth — but that is another board |
| Built for the RAK4631? | **Yes**: the board strings in the file, RAK's 0.4.3 board directory, RAK's own README | No — see above. Vendoring a file built for another board is what the task said to stop rather than do |
| MDK 8.40.2 or later? | **Unknown from the source** (above); the published 0.4.3 tree is 8.29.0 | **No**: at tag `0.11.0` the `lib/nrfx` submodule is `7a4c9d94`, whose `mdk/nrf.h` says 8.29.0, and the `Makefile` compiles that MDK's `system_nrf52840.c` |

Only one candidate exists for this board, and it passes the first two
questions; the third is the first board's. Nothing from a forum, a mirror or a
branch tip was considered.

## Licences

Three parties' work is in the file, and the release carries their notices in
`THIRD-PARTY-NOTICES` (the repository's copy is at the root; `build_release.py`
puts the same file in every release):

- **The bootloader** — [Adafruit nRF52 Bootloader](https://github.com/adafruit/Adafruit_nRF52_Bootloader),
  MIT, Copyright (c) 2016 Adafruit Industries (`LICENSE` of the tree, which
  RAK's 0.4.3 tree carries unchanged).
- **RAK's modifications** — [RAKWireless/WisBlock](https://github.com/RAKWireless/WisBlock),
  MIT, Copyright (c) 2021 RAKwireless (the repository's `LICENSE`).
- **The MBR and the SoftDevice S140 6.1.1** — Nordic Semiconductor ASA's
  five-clause licence, Copyright (c) 2007 - 2018 Nordic Semiconductor ASA
  (`lib/softdevice/s140_nrf52_6.1.1/s140_nrf52_6.1.1_license-agreement.txt` in
  RAK's tree; the same text ships in Nordic's SDK). Redistribution in binary
  form is permitted with its notice reproduced (clause 2, which
  `THIRD-PARTY-NOTICES` is for), only for use with a Nordic integrated circuit
  (clause 4: the RAK4630 is an nRF52840), and the binary may not be reverse
  engineered or disassembled (clause 5: nothing here does, and the reading
  above stops at `0x000F4000`).
