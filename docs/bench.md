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
  says `5 vars` - `fw.version=twinoak-repeater 0.0.0-dev on MeshCore
  repeater-v1.17.1 (d9296435)` (a release build says its version instead), and
  `walter.wake`, `link.errors`, `walter.seen` and `clock.state`, which tasks 03
  to 05 added;
  `sensor get fw.version` is the same string; `sensor set fw.version x` is
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
  `slot 0`, `max_payload 240`, `caps 0x61` (`CLI` and `ATTENTION` from task 05,
  `TIME_PERSIST` from task 04; the rest of the protocol is not implemented yet,
  on purpose), a non-zero `boot_id`, `fw_version 0.0.0-dev`,
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
  is `SerialTransport::worstPassMicros()`, which **still** has no console
  command of its own: task 05 put the console on the link but added no command
  upstream does not have, and the settings it added are the five
  `docs/ARCHITECTURE.md` §4 names. Read it with the debugger
  (`tasks/phase-1/reports/05.md` says what exposing it would have cost).
- [ ] **p1-03** **A frame mangled on purpose is dropped and counted, and
  nothing is answered**: with `-v`, send a `ping` and pull the TX line for a
  moment mid-frame, or feed a bad frame from a terminal; `link_errors` goes up
  by one and the node stays quiet.

## 3. The clock

`docs/link.md` §2a is the page: what each `time_state` means and when a
backwards set is accepted. Nothing below has ever run on a board — the rules are
native tests and the commands ran against the firmware's own session behind a
pipe (`tools/linkloop/`), but no clock has ever ticked on a RAK4630.

- [ ] **p1-04** `linkcli.py time` on a node that was just flashed says
  `time_state 0 (unset)` and an epoch in **May 2024** (upstream's built-in
  date), and `status` has **flag bit 3** set (`adverts held`).
- [ ] **p1-04** `time set`, then `reboot 0`: after the restart `time` says
  `time_state 1 (restored)` and an epoch **a few seconds** on from the set —
  not 2024, and not an hour on, because the firmware wrote its file in the
  second it restarted. `status` then shows bit 3 set again until the next
  `time set`.
- [ ] **p1-04** The same after **pressing the reset switch**, and after a
  **watchdog reset** if one can be provoked: `restored`, but **up to an hour
  ahead** of the real time rather than a few seconds — those two resets give
  the firmware no chance to write its file, and this board has no surviving-RAM
  copy (`tasks/phase-1/reports/04.md`). A `time set` to the real time is
  accepted, because nothing has left the node yet.
- [ ] **p1-04** **Power off for a minute and on again**: `restored`, the saved
  time plus an hour or less, and `reset_reason 1 (power-on)`. A `time set` to
  the real time is accepted.
- [ ] **p1-04** **With no laptop attached**, a nearby companion sees this
  repeater's advert about **a minute** after boot and not before (the hold); with
  `time set` sent within a second of `EV_BOOT`, at once. Upstream's own periodic
  advert is later still — two minutes at the default `adv_int`.
- [ ] **p1-04** **A nearby companion that saw this repeater's adverts before a
  power cycle still accepts its adverts after it** — the whole point of the
  keeper. The way to see it fail is `clkreboot` on the USB console, which sets
  the clock back to 2024 deliberately; this firmware restores the saved time on
  the boot that follows, so even that does not put the node behind itself.
- [ ] **p1-04** On the USB console: `sensor get clock.state` says `unset`,
  `restored`, `set` or `rtc`, matching `status`; upstream's own `time <epoch>`
  and `clock sync` still work and move the state to `set` within one pass of
  `loop()` (the keeper polls for a clock somebody else moved).
- [ ] **p1-04** **An RV-3028 hand-fitted** (a later batch, or a bench part on
  `0x52`): `time` says `time_state 3 (rtc)` after a power cycle, `status` never
  holds the adverts back, and a `time set` is written through to the part. No
  session could check a path that needs a part nobody has.

## 4. Management over the link

`docs/link.md` §2b and §2c are the pages: the console over the link, our
settings, and attention. Every rule below has a native test against fakes and
most of them ran against the firmware's own session behind a pipe
(`tools/linkloop/`); what no session could do is run **upstream's own
interpreter**, which is the thing these lines are really checking, and hear a
mesh admin.

- [ ] **p1-05** `linkcli.py --port <port> cli "get radio"` answers
  `> 869.618,62.5,8,8`, exactly as the USB console does; `cli "neighbors"` and
  `cli "stats-core"` answer as it does too (`stats-core` is one of the commands
  upstream refuses over the mesh and allows on a console — this link gets it
  because its `sender_timestamp` is 0). `cli "set name Bench"` is answered `ok`
  and the next advert carries that name; `cli "get name"` says `> Bench`.
- [ ] **p1-05** `cli "sensor list"` says `5 vars` and then `fw.version`,
  `walter.wake`, `link.errors`, `walter.seen` and `clock.state` — all five on a
  freshly booted node, and on a node that has been up for weeks the last of them
  behind `cli "sensor list 4"`, because upstream stops a listing at 134 bytes.
  `cli "sensor get walter.seen"` counts up while nothing is sent, and reads `0`
  right after any request.
- [ ] **p1-05** **A reply at the limit.** `cli "sensor list"` on a node with a
  long `fw.version` is the longest reply this firmware produces; it comes back
  whole and ends in `clock.state=...` with nothing cut mid-line. Nothing on the
  USB console is disturbed by any of it.
- [ ] **p1-05** **A line that restarts the node is not answered**:
  `cli "reboot"` prints *(no answer …)*, and `listen` then shows `EV_BOOT` with
  `reset_reason 4` and a new `boot_id`. The clock keeper's file was written on
  the way down, so `time` after it says `restored` and a few seconds on
  (section 3's second line, by another road).
- [ ] **p1-05** **A malformed line is refused and never run**: `cli ""` and a
  line with a tab in it are `ERR_MALFORMED`, and the node's behaviour is
  unchanged. With `-v`, the frame is there and the answer is one status byte.
- [ ] **p1-05** **`linkcli.py sleep 600`, then from a phone logged in as admin
  over the mesh: `sensor set walter.wake 1`** → it answers `ok` on the mesh, and
  `listen` on the link shows two `0x00` bytes, 300 ms, and then `EV_ATTENTION`
  reason 4 **ten times, a second apart, with `repeat` 0 to 9**. `status` then
  shows `attention 4` and flag bit 2 (asleep) — and the answer to that very
  `status` is the last place the 4 appears: the next one reads 0 and the flag is
  clear. This is the whole point of the task and needs two radios and a phone.
- [ ] **p1-05** **A second `walter.wake` inside the hour raises nothing.**
  `sensor set walter.wake 1` again (from the phone or the console) answers `ok`,
  `sensor get walter.wake` reads `1` while one is pending and `0` after it was
  told, and no second sequence appears on the link until an hour has passed.
- [ ] **p1-05** **While it believes this end is asleep, nothing else is sent.**
  After `sleep 600`, `listen` is silent but for an attention — and the moment any
  request is sent, flag bit 2 clears. (The only events this firmware has today
  are the two section 8 lets through, so what the bench adds here is that a real
  idle link is really quiet.)
- [ ] **p1-05** **`cli "sensor set walter.wake 1"` over the link** raises the
  same attention as the mesh admin's, which is the easy way to see the sequence
  with no phone at all — and **`cli "region load"` is the one line not to
  send**: it leaves upstream's interpreter waiting for a blank line, which this
  link cannot send. `cli " "` (one space) is the way out, and worth confirming
  once, because the alternative is a reboot.

## 5. Power

(phase 1, task 06)
