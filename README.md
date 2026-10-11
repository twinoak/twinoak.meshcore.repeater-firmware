# TwinOak repeater firmware

The firmware family for the LoRa box of a
[TwinOak standard repeater v2](https://github.com/twinoak/twinoak.meshcore.standard-repeater-design-v2):
a RAK4630 behind a cavity filter, in a shielded box, with a second box beside it
that holds an LTE modem and is allowed to reflash this one.

It is a **MeshCore repeater** — upstream's, compiled unmodified from a pinned
commit — with four things added:

1. a **link** to the management MCU in the other box (the Walter): local
   management with console privileges, and the unit's power and environment
   telemetry with the right shunt for each channel;
2. a **clock** that copes with a board that has no RTC;
3. the **radio half of a MeshMaster exit node**: on the Walter's behalf it can
   log in to a neighbouring repeater, ask it something, and hand back the
   answer — as itself, with its own key, without ever ceasing to repeat;
4. an **image that says what it is**, in a release that is signed.

The plan, the decisions and the two protocols are in [`spec/`](spec/README.md)
(start with `spec/SYSTEM.md`). How this firmware is put together is
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). How it gets built is
[`tasks/`](tasks/README.md); how it gets released is
[`docs/release.md`](docs/release.md); how it keeps up with MeshCore — the weekly
pull request that moves the pin, the one command for a bump that needs a
session, and the seven places a bump re-reads — is
[`docs/upstream.md`](docs/upstream.md).

## Building it

```powershell
pio run -e repeater_rak4630_single   # the repeater for the single-RAK adapter
pio test -e native                   # the tests that need no board (in a container on Windows)
.\tools\verify.ps1                   # everything CI runs, in order, with a log per step in out/verify/
```

A clean first build downloads, once, into `~/.platformio`: the `nordicnrf52`
platform, the ARM GCC toolchain, MeshCore's fork of the Adafruit nRF52 Arduino
core at the commit MeshCore pins, `adafruit-nrfutil` for the DFU package; and
into `.pio/libdeps/`: MeshCore itself at the pinned commit (`[meshcore]` in
`platformio.ini`) and every library at the exact version `[libs]` names. It
takes a few minutes; every build after it takes seconds. The images it leaves
in `.pio/build/<env>/` — and `tools/verify.ps1` copies to `out/build/<env>/` —
are `firmware.hex` (the Walter, over SWD), `firmware.zip` (a laptop,
`adafruit-nrfutil dfu serial`), `firmware.uf2` (the bootloader's drive) and
`firmware.elf`.

`pio test -e native` is a host build. Linux and CI use the machine's `g++`;
this Windows machine has none, so `tools/verify.ps1` runs that step in a
`python:3.11-slim` container (`tasks/RUNBOOK.md` §2 has the one-line
`docker run`).

`tools/verify.ps1` also rehearses the release on every run: the release
tooling's own tests, then a dry run over the build it has just made, leaving
`out/release/` with the three images, a validated `manifest.json`, its
signature by the specification's public *test* key and `SHA256SUMS`
([`docs/release.md`](docs/release.md)). That step needs the `cryptography` of
`tools/release/requirements.txt`, and on a machine without it the same
container provides it. The task piles that build the rest are run with

```powershell
.\tasks\run-pile.ps1          # phases 0 to 3, one headless session per task
.\tasks\run-all.ps1 -Phase 0  # one phase
```

## What it will be, in one table

| | |
|---|---|
| MCU / radio | nRF52840 + SX1262 (RAK4630 stamp, u.FL) |
| Built with | PlatformIO; MeshCore as a pinned dependency ([`docs/upstream.md`](docs/upstream.md)); the Adafruit nRF52 core MeshCore pins |
| Builds | `pio run -e <role>_<board>`; `pio test -e native` for everything that needs no board |
| Verified by | `tools/verify.ps1` — locally first; CI runs the same script |
| Flashed | in service: by the Walter over SWD, application region only. At a bench: UF2 or serial DFU over the adapter's USB-C |
| Released as | a GitHub Release: the images, `manifest.json`, its signature, `SHA256SUMS` |
| Channel | 869.618 MHz, 62.5 kHz, SF8, CR8, +22 dBm (MeshCore Denmark, EU Narrow) |

## Rails that hold in every task

- **It repeats, whatever else happens.** Nothing added here may stall the mesh
  loop: not the link, not a sensor that stopped answering, not the Walter.
- **Upstream is not edited.** Our code subclasses and composes
  (`docs/ARCHITECTURE.md` §2).
- **Nothing the mesh says is logged as text.** `MESH_PACKET_LOGGING` and
  `MESH_DEBUG` stay off; a test greps for them.
- **A password is in RAM for the length of a send** and nowhere else.
- **One packet per send** for everything the mesh client originates.
- **Path hashes this firmware chooses are two bytes or more.**
- **No default password is ever on the air.**
- **Never a chip erase.** The file system holds the node's identity; an image
  lives inside its application region and nowhere else.

## Licence

MIT — [`LICENSE`](LICENSE). MeshCore is MIT too and is a dependency, not a
fork: nothing of its tree is vendored here except what a consumer of the
package cannot reach, and each such file says so.
