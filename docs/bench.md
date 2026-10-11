# The bench — what no session could verify

What every task left under *Not verified here*, collected in the order the rig
is brought up (`hardware/pcb/BENCH-BREADBOARD.md` §7 in the hardware
repository): UART, reset, SWD, flash, then the kill line and I2C. Each line
says which task wrote it, so the report with the detail is one hop away
(`tasks/phase-P/reports/NN.md`). Tick what the bench confirmed; a line that
fails goes back to the task's follow-ups.

## 0. Before the rig

- [ ] **p0-01** Nothing flashed: the image of phase 0 is upstream's stock
  `simple_repeater` for the RAK4631 built with our channel, and the first
  thing worth flashing is phase 1's. If it is flashed anyway (UF2 or
  `adafruit-nrfutil dfu serial -pkg firmware.zip`), it should boot, print
  `Repeater ID:` on the USB console at 115200 and advertise on 869.618 MHz,
  62.5 kHz, SF8, CR8 — and it carries upstream's default admin password, which
  phase 3's provisioning replaces.
- [ ] **p0-02** **The release key, which only Thomas can make.**
  `python spec/tools/manifestref.py keygen`, then the private hex into the
  repository secret `RELEASE_SIGNING_KEY` and the public hex into
  `keys/release.pub` in a commit (`keys/README.md`). Until it exists a tag is
  refused, which is the safe state, and the tag path of
  `.github/workflows/release.yml` has never run — only its dry run, which is
  the same jobs with `gh release create` left out.
- [ ] **p0-02** **The raw `.bin` written over SWD boots.** The release's
  `twinoak-repeater-rak4630-single-868-vX.Y.Z.bin` is the application bytes
  alone, to be written at the `load_address` the manifest gives (`0x00026000`
  today) with nothing else erased. The Walter's flasher does this in service;
  on the bench it is a J-Link or a CMSIS-DAP with an explicit base address, for
  example `pyocd flash -t nrf52840 --base-address 0x26000 <file>` or
  `openocd … -c "program <file> 0x26000 verify reset exit"`. What is being
  checked: the board comes up exactly as it does from the `.uf2` of the same
  release — same console line, same advert — and the file system, the identity
  and the bootloader are untouched afterwards. No session can do it: nothing
  was flashed and no radio was heard.

- [ ] **p0-03** **The first Monday, and the first real bump.** The upstream
  watch was rehearsed, not fired: what no session can check is that
  `cron: "29 6 * * 1"` is a minute GitHub actually runs
  `.github/workflows/upstream.yml` at, and that a *real* newer `repeater-v*`
  tag takes the same path a rehearsal did. On the Monday after a MeshCore
  repeater release: the run is in the Actions tab, a pull request **MeshCore
  \<tag\>** is open with upstream's commit list in it, and its CI run needs
  **Approve and run** (`docs/upstream.md` says why, and gives the `gh api`
  line). Green means merge; red means
  `tools/new-sync-task.ps1 -Tag <tag> -Pr <n>` and a session. The first bump is
  also the first test of `tasks/sync/TEMPLATE.md` — and it will be red for one
  reason before any other, which `docs/upstream.md`'s *What a bump costs today*
  names.

## 1. The repeater alone on USB

- [ ] **p1-01** Flash the UF2 (double-tap reset on the adapter's USB-C; the
  bootloader's drive appears) and open the console at 115200. It prints
  `Repeater ID: <64 hex digits>`, and on the very first boot of a module - or
  after `erase` - one more line, `First boot: a random admin password was set
  (not shown; …)`. `ver` says `v1.17.1 (Build: 14 Aug 2026)`, upstream's.
  `board` says `RAK 4631`. `get radio` says `869.618,62.5,8,8`. `sensor list`
  says `1 vars` and `fw.version=twinoak-repeater 0.0.0-dev on MeshCore
  repeater-v1.17.1 (d9296435)` (a release build says its version instead);
  `sensor get fw.version` the same string; `sensor set fw.version x` is
  refused (`can't find custom var`, upstream's wording for a key that took no
  value).
- [ ] **p1-01** It hears adverts (`neighbors` after a few minutes on the roof
  antenna) and repeats (a message through it from a phone).
- [ ] **p1-01** A phone login with the password `password` is refused, and so
  is `not-a-password` (the build's placeholder, which the first boot
  overwrote). After `password <something>` on the console that one is
  accepted; a power cycle keeps it (`/prefs.json` holds it) and prints no
  `First boot` line.
- [ ] **p1-01** `INFO.VARIANT` of the module, for the Walter's flasher
  (hardware repository, `BENCH-BREADBOARD.md` caveat 6): record it here.
- [ ] **p1-01** I2C is at 100 kHz on the scope (SCL, P0.14) - `Wire.setClock`
  after upstream's `begin()`; the BME280, if fitted, is found by itself
  (`sensor list` does not show it - upstream lists no setting for it - but a
  telemetry request from a phone carries temperature, humidity and pressure).

## 2. The link from a laptop

`docs/link.md` is the page: the wiring, the commands, and what each answer
means. Everything below is `tools/linkcli.py` from a laptop, and every one of
these lines is a native test against fakes already — what no session could do
is put the bytes on a real UART through real filters.

- [ ] **p1-03** **A 3.3 V USB-UART dongle on the adapter's link pins** (J4/J5
  on 1–2; dongle TX to P0.15, dongle RX to P0.16, grounds together).
  `linkcli.py --port <port> hello` prints `protocol 1.0`, `role 1 (repeater)`,
  `slot 0`, `max_payload 240`, `caps 0x00` (nothing else of the protocol is
  implemented yet, on purpose), a non-zero `boot_id`, `fw_version 0.0.0-dev`,
  `board rak4630-single` and `upstream repeater-v1.17.1+d9296435`.
- [ ] **p1-03** `ping` answers and echoes the nonce; `status` shows the uptime
  and `reset_reason` — **1 (power-on)** after a power cycle, **2 (reset pin)**
  after the reset line, **4 (software)** after `reboot 0` — with
  `flags 0x0010` (the radio up) and `link_errors 0`. A power cycle changes
  `boot_id`; a `reboot 0` changes it too.
- [ ] **p1-03** `identity` prints the same 64 hex digits the USB console's
  `Repeater ID:` line printed, and the node's advert name.
- [ ] **p1-03** `reboot 0` is answered *before* the node goes, and `listen`
  then shows `EV_BOOT` with `reset_reason 4` and a new `boot_id`. **`reboot 1`
  enters the UF2 bootloader and the way back is a power cycle** — worth doing
  once, deliberately, because it is the mode a person at the field USB port
  uses; 2 (serial DFU) and 3 (over-the-air DFU) are the same kind of one-way
  door.
- [ ] **p1-03** **Through both bulkhead filters** (470 Ω and 2.2 nF on each
  line, D-sub pins 8/9): the same five commands, and `link.errors` stays 0
  over an hour of `ping` once a second. `sensor get link.errors` on the USB
  console is the same counter, and `sensor get walter.seen` says how many
  seconds ago the last valid request arrived (`never` before the first one).
- [ ] **p1-03** **Repeating is unaffected while `ping` runs ten times a
  second**: a message through the repeater from a phone still arrives, and
  `link.errors` stays 0. The worst pass measured on the host is tens of
  microseconds (`tasks/phase-1/reports/03.md`); what the board actually spends
  is `SerialTransport::worstPassMicros()`, which has no console command of its
  own yet — read it with the debugger, or wait for task 05's `CLI` over the
  link.
- [ ] **p1-03** **A frame mangled on purpose is dropped and counted, and
  nothing is answered**: with `-v`, send a `ping` and pull the TX line for a
  moment mid-frame, or feed a bad frame from a terminal; `link_errors` goes up
  by one and the node stays quiet.

## 3. The clock

(phase 1, task 04)

## 4. Management over the link

(phase 1, task 05)

## 5. Power

(phase 1, task 06)
