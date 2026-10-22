# The bench — what no session could verify

What every task left under *Not verified here*, collected in the order the rig
is brought up (`hardware/pcb/BENCH-BREADBOARD.md` §7 in the hardware
repository): UART, reset, SWD, flash, then the kill line and I2C. Each line
says which task wrote it, so the report with the detail is one hop away
(`tasks/phase-P/reports/NN.md`). Tick what the bench confirmed; a line that
fails goes back to the task's follow-ups.

How to read a line:

- **`[ ]`** is the box to tick, and **`→ ______`** is a number or a word to
  write in. A line with no blank is a yes or no.
- **`[boards]`** means the line needs the **production adapter and connector
  boards**; everything else can be walked on the breadboard rig
  (`BENCH-BREADBOARD.md` §1: a RAK4631 WisBlock Core on a RAK19001 base, with
  the two UART lines through 470 Ω / 2.2 nF and optional INA3221, BME280 and
  RV-3028 breakouts). On that rig the link lands on **P0.15 / P0.16 of the
  base** rather than on the adapter's J4/J5, and the current-sense breakout has
  one shunt value where the board has three — which is exactly what the
  `[boards]` lines are about.
- A line that needs a **second radio**, a **phone** or **ten minutes on the
  roof antenna** says so where it stands.
- A line ticked **`[x] … (bench, <date>, <commit>)`** was walked by a session
  through `tools/bench/` (§0a) and not by a person: the date and the commit are
  the run's, and the result file under `out/bench/` has what it saw. A line a
  person walked is ticked without that tag.

## 0a. The bench a session may use

`tools/bench/` is the one way a session touches a board (`tasks/RUNBOOK.md`
§2). It needs **`bench.local.json`** at the repository root — git-ignored,
because it carries this machine's USB serial numbers; `bench.example.json` is
the shape — saying:

| Key | What |
|---|---|
| `board` | `rak4630-single`: the board as `spec/FIRMWARE-MANIFEST.md` spells it; an image for another board is refused |
| `serial` | the RAK's **USB serial number** — the nRF52840's FICR device id, 16 hex digits, which the application's port reports (the Adafruit core's `TinyUSB_Port_GetSerialNumber`) and the bootloader's port is expected to report too. The tool finds the board by it and by nothing else: it never picks "the only COM port" |
| `rf` | what is on the antenna port: `dummy-load`, `antenna` or `nothing`. **A repeater image is flashed only on `dummy-load`**; the scanner, which never transmits, on any |
| `identity` | `keep`, or `disposable` if this module's identity may be thrown away (§10 erases it). Only `disposable` lets the console send `erase` or a `password` line |
| `dongle_serial` | optional: the 3.3 V USB-UART dongle on the link pins, so `record` can use `hello` and `identity` instead of the console |
| `bootloader_serial` | optional: only if the bootloader's port turns out to report another serial number than the application's |

With no file, or no port with that serial number, every command exits **3,
*no bench***, and that is not a failure. What the commands are:
`bench status`, `bench check <zip>` (the guards alone), `bench record`,
`bench flash <zip>`, `bench console "<line>"` (and `--scanner`),
`bench restore`, and `bench walk <section>` (§0b). Before the first flash of a run the tool records what is on
the board — role, version, `Repeater ID`, name — and `restore` puts that role
back and checks the identity; a run that cannot be put back says so in its
last line, in capitals.

**A new unit, which shows only its bootloader.** A radio that arrives with its
base and no application (`spec/SYSTEM.md` S-5a, what the Walter's
`rak provision` leaves) has no application port and no console, so there is
nothing of what it ran to record. `bench flash` records the image it is about
to write instead — its role, its version and its descriptor, read out of the
file — and the run file still says `how: bootloader`, because none of it is
something a board said. `bench restore` then puts that same image back and its
last line says which case this was: *the board arrived with only its
bootloader; restored: the image the run began with*. Nothing is touched at
1200 baud on the way: a board with no application has no port to touch, and
the tool goes straight to the bootloader's port it can already see. A board
recorded in its bootloader with **no** image named (`bench record` with no
`--image`) is still a run nothing can put back, and that is still said in
capitals. Every flash is logged with the image's descriptor and
SHA-256 in `out/bench/flash.log` and under the user's local application data,
where the lock per board also lives (one user at a time). SWD is never used by
this tool: the `[boards]` SWD lines of this page stay a person's.

**The first board this page was walked on** (p3.4-02, 22 October 2026; a RAK4630 on the
production adapter, its base written with a probe). Its USB serial number is the nRF52840's FICR
device id and **every mode reports the same one**, so `bootloader_serial` was not needed:

| what was running | USB id | serial number | COM, on that machine |
|---|---|---|---|
| the application (repeater or scanner) | `239A:8029` | `E926F6BEB407A412` | COM30 |
| the bootloader, after a double-tap or a watchdog reset | `239A:0029` | `E926F6BEB407A412` | COM29 |
| the bootloader, after the 1200-baud touch (serial-only DFU) | `239A:002A` | `E926F6BEB407A412` | COM31 |

The third row is why `tools/bench` takes **both** bootloader product ids (`boards/rak4631.json`
lists all four): the touch's own DFU mode is `0x002A`, and a tool that waited for `0x0029` alone
waits for a port that is already there under the other id.

**Two things a real board does that a fake cannot.** *The boot lines are a race.* `Repeater ID:`
and the `First boot:` lines are printed about a second after a reset and only while a host holds
the port (`Adafruit_USBD_CDC::write` drops bytes while `tud_cdc_connected()` is false), and the
port is not back before 1.4 s: a run of the tool catches them sometimes and not others. So the
tool reads the node's key with upstream's own **`get public.key`** (the same 32 bytes,
`src/helpers/CommonCLI.cpp:902` at the pin) and resets nothing for it; §10 holds the first boot to
what it left behind (a new key, `firstboot.state`) and the two lines themselves are a person's,
with a terminal already open across the reset button. *Flashing away from the scanner takes two
DFUs.* The scanner starts the nRF52840's watchdog (8 s, `src/roles/scanner/Wdt.h`) and a soft
reset does not stop it, so it bites in the middle of the DFU that would replace the scanner; the
tool waits for the port to go, finds the bootloader the board was left in and runs
`adafruit-nrfutil` once more, which works (`tasks/phase-3/FOLLOWUPS.md` has the decision that is
still open). `adafruit-nrfutil` **exits 0 even when its write failed**, so what the tool holds it
to is its own `Device programmed.` line.

## 0b. What a session walks, and what stays yours

`python tools/bench/bench.py walk <section>` (§0a, task `p3.3-03`) runs the
lines of four sections of this page as steps, in the page's order: it prints the
line, what it sent, what it saw and *ok*, *differs* or *needs a person*, and
leaves a result file under `out/bench/` with every `→ ______` of the lines it
walked filled in. `--tick` then ticks what came back *ok* here and writes what
came back *differs* into the follow-ups of the task that wrote the line; a
line an earlier walk ticked is reported *already ticked* and left alone, and
only a line the page really has lost is *not found*. The
rest of this page is a person's, and so are the lines in the right-hand column:

| | `bench walk` runs | Yours |
|---|---|---|
| §1 | the console on a repeater: the banner's `Repeater ID`, `ver`, `board`, `get radio`, both `sensor list` pages with the cut digit, `sensor get fw.version`, `sensor set fw.version x` refused, and `get dutycycle` | the adverts and the repeating, a phone's login, `INFO.VARIANT`, the scope on SCL, the two USB serial numbers (a double-tap), and the refusals with `rf: antenna` in the config |
| §8 | the descriptor in the file, the flash, `hello` over the link if the dongle is there, and the console's `fw.version` — the three agreeing | the Walter's half, which is another repository's |
| §9 | the whole round trip: the `boot` line and `radio_ok`, `info`, `stat`, a 281-point `sweep`, `hist`, `dwell`, `cad`, `stop` mid-sweep, a minute of output with no `nan`, the `rom`, `scope 863 870`, the dummy-load baseline — and the repeater back with its identity | `pwr` against the boards, the watchdog, the image check with a generator, the site through the filter, and `scope` from the Walter UART |
| §10 | **only with `"identity": "disposable"`**: the `erase`, both `First boot:` lines, `done` then `never`, `get dutycycle`, `path.hash.mode`, the provisioning at the USB port or over the link, and the identity afterwards | the power cut in the middle, and the phone's login |
| §0, §2–§7 | nothing: a radio, a phone, a scope probe, a multimeter or the roof | all of it |

A walk needs the board (§0a): with no `bench.local.json`, or no port with its
serial number, it exits 3 and the task that would have walked it says *Not
verified here*, as before the tool existed.

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

- [ ] **p3.4-01** **The base boots, and the port after the reset.** The
  release's `twinoak-base-rak4630-0.4.4-s140-6.1.1.hex` (`use: provision`;
  RAK's bootloader 0.4.4 with SoftDevice S140 6.1.1, vendored at
  `boards/base/rak4630/`) has never met a radio: no session can write it (the
  Walter's `rak provision <radio> confirm`, S-5a, or the hardware repository's
  `walter-swd-bench`, or a probe). What a person checks, in this order, on the
  first radio that gets it: **(1)** after the one reset at the end the radio
  enumerates on USB as the RAK4631 bootloader (VID `239A`, the `RAK4631` drive
  on a double-tap) and its `INFO_UF2.TXT` reads `UF2 Bootloader 0.4.4`,
  `SoftDevice: S140 6.1.1`, `Date: Jun 17 2026` — the file that was vendored is
  the one that boots → ______; **(2)** `INFO.VARIANT` of the part (`rak swd id`
  / `info` on the Walter, or the bootloader's drive, `docs/bench.md` §1) →
  ______; **(3)** the port after the reset, as `rak provision` reports it:
  *open* or *closed*. On a build code `F` part (`AAF0`) *closed* means this
  bootloader was built from a Nordic MDK before 8.40.2 and S-5's SWD update
  path is gone after every reset — `boards/base/rak4630/README.md`,
  *APPROTECT*, says what is known and the hardware repository's
  `software/FIRST-UNIT.md` step 8 says to stop and decide → ______; **(4)**
  the first application after it is an ordinary first install (`rak update
  --first-install`) and this pile's 02 walks §1 from there.
- [ ] **p3.5-01** **The first new unit flashed and restored by the tool.** A
  radio that has just had its base written shows only the bootloader's port,
  and that is the one state this bench cannot make for itself: the tool never
  erases and the base is the Walter's to write, so the path is a host test here
  (`tools/bench/test_bench.py`, *a new unit*). On the next radio that comes out
  of `rak provision` (the Walter's phase 6, task 03), with the dummy load on
  the antenna port and the serial number in `bench.local.json`:
  `bench status` says `bootloader on COM…` → ______; then
  `bench flash out/build/repeater_rak4630_single/firmware.zip`, whose log says
  *the board is in its bootloader* and *records the image it is about to be
  given* and which must **not** say *the 1200-baud touch* → ______; then
  `bench restore`, whose last line must be *the board arrived with only its
  bootloader; restored: the image the run began with* → ______. What is being
  checked is that no hand is needed between the two: the first board on this
  bench needed a `bench record --image …` typed in between (p3.4-02).
- [ ] **p0-03** **The first Monday, and the first real bump.** The upstream
  watch was rehearsed, not fired: what no session can check is that
  `cron: "29 6 * * 1"` is a minute GitHub actually runs
  `.github/workflows/upstream.yml` at, and that a *real* newer `repeater-v*`
  tag takes the same path a rehearsal did. On the Monday after a MeshCore
  repeater release: the run is in the Actions tab, a pull request **MeshCore
  \<tag\>** is open with upstream's commit list in it, and its CI run needs
  **Approve and run** (`docs/upstream.md` says why, and gives the `gh api`
  line). Green means merge; red means
  `tools/new-sync-task.ps1 -Tag <tag> -Pr <n>` and a session. **p3.2-01** is
  what makes green the expected outcome: no test and no tool of ours names the
  release any more, so a bump that changes nothing we rely on should pass on its
  first run (`docs/upstream.md`, *What a bump costs today*;
  `tasks/phase-3.2/reports/01.md`, where the same bump was rehearsed on
  `repeater-v1.17.0` — a *real* tag is the thing no session can rehearse). What
  to read before merging a green one is the eight places of
  `tasks/sync/TEMPLATE.md`; a red one is still the first test of that file.

## 1. The repeater alone on USB

- [x] **p1-01** Flash the UF2 (double-tap reset on the adapter's USB-C; the
  bootloader's drive appears) and open the console at 115200. It prints
  `Repeater ID: <64 hex digits>`, and on the very first boot of a module - or
  after `erase` - one more line, `First boot: a random admin password was set
  (not shown; …)`. `ver` says `v1.17.1 (Build: 14 Aug 2026)`, upstream's.
  `board` says `RAK 4631`. `get radio` says `869.618,62.5,8,8`. `sensor list`
  says **`10 vars`** and prints the first five of them -
  `fw.version=twinoak-repeater 0.0.0-dev on MeshCore repeater-v1.17.1
  (d9296435)` (a release build says its version instead), then `walter.wake`,
  `link.errors`, `walter.seen` and `clock.state` - and then `... next:` with
  **the digit cut**, because upstream stops a listing at 134 bytes and the
  tenth key made its header one byte longer (§5's paging line walks it, and
  `tasks/phase-3/reports/03.md` has the arithmetic); `sensor list 5` is the
  other five. `sensor get fw.version` is the same string; `sensor set
  fw.version x` is refused (`can't find custom var`, upstream's wording for a
  key that took no value). (bench, 2026-10-22, 9fb721b)
- [ ] **p1-01** It hears adverts (`neighbors` after a few minutes on the roof
  antenna) and repeats (a message through it from a phone).
- [ ] **p1-01** A phone login with the password `password` is refused, and so
  is `not-a-password` (the build's placeholder, which the first boot
  overwrote). After `password <something>` on the console that one is
  accepted; a power cycle keeps it (`/prefs.json` holds it) and prints no
  `First boot` line.
- [ ] **p1-01** `INFO.VARIANT` of the module, for the Walter's flasher
  (hardware repository, `BENCH-BREADBOARD.md` caveat 6): → ______________
- [ ] **p1-01** I2C is at 100 kHz on the scope (SCL, P0.14) - `Wire.setClock`
  after upstream's `begin()`; the BME280, if fitted, is found by itself
  (`sensor list` does not show it - upstream lists no setting for it - but a
  telemetry request from a phone carries temperature, humidity and pressure).
- [ ] **p3.3-02** **The board's two USB serial numbers are one.** With the
  repeater running, `python -m serial.tools.list_ports -v` shows the RAK as
  `239A:8029` with a 16-hex-digit serial number (→ ______________); after a
  double-tap reset the bootloader's port shows `239A:0029` with — this is the
  line — **the same** serial number. If it differs, that number goes in
  `bench.local.json` as `bootloader_serial`, and the report of p3.3-02 says why
  it could not be read off a file (the bootloader's source is not on disk).
  Then a power cycle, and the repeater is back. p3.4-02 read the **same**
  number in all three modes of one board, including the touch's `239A:002A`
  (§0a's table); what is left of this line is the double-tap and the power
  cycle, which are a hand on the board.
- [ ] **p3.3-02** **The round trip through `tools/bench/`, with the dummy load
  on the antenna port** and `bench.local.json` filled in (§0a):
  `python tools/bench/bench.py status` finds the board; `bench flash
  out/build/repeater_rak4630_single/firmware.zip` records what was there
  (`Repeater ID` → ______, caught from the banner or after one `reboot`),
  touches at 1200 baud, waits for the bootloader's port, runs
  adafruit-nrfutil (`Device programmed.`), and reads `sensor get fw.version`
  back as the descriptor's version; `bench console "ver"` answers upstream's
  version line; `bench restore` ends in a line beginning `restored:` that
  names the image it used and says the `Repeater ID` matches. Time from touch
  to application back → ______ s (the tool allows 15 s for the bootloader and
  40 s for the application; a board slower than that is a finding).
- [ ] **p3.3-02** **What it refuses on a real board.** With the RF port set to
  `antenna` in `bench.local.json`: `bench flash` of the repeater image is
  refused before any port is opened (exit 2) and so is a scanner image while
  the board carries a repeater (it could not be put back); `bench console
  "password x"` is refused with `identity: keep`. Nothing on the board changes
  — `ver` and `get name` read as before.

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
- [ ] **p1-03** **[boards]** **Through both bulkhead filters** (470 Ω and
  2.2 nF on each line, D-sub pins 8/9): the same five commands, and
  `link.errors` stays 0 over an hour of `ping` once a second. `sensor get
  link.errors` on the USB console is the same counter, and `sensor get
  walter.seen` says how many seconds ago the last valid request arrived
  (`never` before the first one).
- [ ] **p1-03** **Repeating is unaffected while `ping` runs ten times a
  second**: a message through the repeater from a phone still arrives, and
  `link.errors` stays 0. The worst pass measured on the host is tens of
  microseconds (`tasks/phase-1/reports/03.md`); what the board actually spends
  is `SerialTransport::worstPassMicros()`, which **still** has no console
  command of its own: task 05 put the console on the link but added no command
  upstream does not have, and the settings it added are the five
  `docs/ARCHITECTURE.md` §4 names. Read it with the debugger
  (`tasks/phase-1/reports/05.md` says what exposing it would have cost):
  worst pass → ______ µs.
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
- [ ] **p1-05** `cli "sensor list"` says `10 vars` and then `fw.version`,
  `walter.wake`, `link.errors`, `walter.seen` and `clock.state` — those five on
  a freshly booted node, and on a node that has been up for weeks the last of
  them behind `cli "sensor list 4"`, because upstream stops a listing at 134
  bytes. `cli "sensor get walter.seen"` counts up while nothing is sent, and
  reads `0` right after any request.
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
- [ ] **p1-05** **A `CLI` line and its reply are never printed on the USB
  console.** With the USB console open, run `cli "password <something>"` and
  then `cli "get name"` from the laptop: nothing of either line or of either
  reply appears on `Serial`. The rails test is a grep over our sources
  (`test/test_rails`, rule 2); what a board shows while **upstream's** own
  interpreter runs is upstream's business — its `get acl` and `log` branches
  print to the console on purpose — and only a bench can watch it. Set the
  password back afterwards.
- [ ] **p1-05** **`cli "sensor set walter.wake 1"` over the link** raises the
  same attention as the mesh admin's, which is the easy way to see the sequence
  with no phone at all — and **`cli "region load"` is the one line not to
  send**: it leaves upstream's interpreter waiting for a blank line, which this
  link cannot send. `cli " "` (one space) is the way out, and worth confirming
  once, because the alternative is a reboot.

## 5. Power

The hardware repository's `hardware/pcb/CURRENT-SENSE.md` §6 is the fuller
procedure for this block — the zero-current offsets, the known-current check at
0.1/0.5/1.0/2.0 A, the charge profile, the "dead-battery dawn" case and the
thermal check. These are the **firmware's** lines on top of it, and the first
of them is the exit criterion of the whole phase.

- [ ] **p1-06** **[boards]** **`linkcli.py power` against a multimeter — sign
  first, then size.** With the bench supply at 18–21 V standing in for the panel:
  **channel 1** positive and within a few percent of a DMM in series with the
  panel lead; **channel 2 positive while charging** and **negative with the
  panel switched off**; **channel 3** equal to the two radios' draw (expect
  20–60 mA idle, and a visible step when the Walter's modem attaches). Channel
  1's LSB is 0.4 mA, channel 2's is 2 mA and channel 3's is 0.8 mA, so do not
  chase the last milliamp — what matters is that none of the three reads 2×,
  2.5× or 5× out, which is what a single shunt constant would give
  (`CURRENT-SENSE.md` §4). Panel → ______ mA / ______ mV;
  battery → ______ mA / ______ mV; load → ______ mA / ______ mV.
- [ ] **p1-06** **[boards]** **The same three numbers in a phone's telemetry
  view** of the repeater, on LPP channels 2, 3 and 4 (`docs/link.md` §2d has
  the table), and
  **the battery voltage in its `status`** — which is channel 2's bus voltage and
  not the unconnected ADC pin. `linkcli.py lpp` and the phone must agree byte
  for byte; that equality is a native test over the same field list, but only a
  radio proves the mesh half of it. **No phone has ever read this node's
  telemetry.**
- [ ] **p1-06** **[boards]** **`linkcli.py env` against a thermometer** in the
  enclosure, and
  the humidity and pressure against anything trustworthy (a weather station's
  sea-level pressure minus about 12 Pa per 100 m of altitude). The temperature
  must also appear on **LPP channel 5**; the humidity and pressure deliberately
  do not, because CayenneLPP 1.6.1 cannot encode them (`reports/06.md`).
  Enclosure → ______ °C / ______ %RH / ______ hPa.
- [ ] **p1-06** **The battery thresholds, with the simulator.** Step the pack
  below **3500 mV** and hold it: nothing for the first minute, then
  `EV_ATTENTION` **reason 2** with `detail` the millivolts. Below **3300 mV**,
  held a minute: **reason 3**. A dip of a few seconds — or a transmit burst —
  raises nothing. Coming back needs 100 mV of hysteresis *and* the same minute.
  A second reason 2 inside the hour raises nothing (the machine's limit, §2c).
- [ ] **p1-06** **The thresholds are settable and survive a restart.**
  `cli "sensor set power.low_mv 3600"` answers `ok`, `cli "sensor list 5"` shows
  it, `reboot 0` — and it is still 3600 after the boot. `sensor set
  power.low_mv 3350` is refused (`can't find custom var`) because it would sit
  less than 100 mV above `crit`, and so are `2400` and `4400`.
- [ ] **p1-06** **[boards]** **With the INA3221's bus unplugged, the repeater
  keeps repeating** and `power` answers `valid 0` with every number 0. Nothing
  on the console hangs, `ping` still answers inside its 250 ms, and a packet
  forwarded during the unplug is forwarded normally. **Then plug it back in**:
  the three channels come back within about five seconds (the reader's
  back-off) with no restart. Do the same with the A-board's BME280: `env`
  answers `valid 0` and `power` is unaffected. **The reviewer's option**
  (`tasks/phase-1/reports/06.md`, *Deviations*): hold **SDA low** instead of
  unplugging it, which is what a bus that is stuck rather than absent does to
  the core's unbounded TWIM loops — the one failure mode this firmware cannot
  bound by itself.
- [ ] **p1-06** **`sensor list` really does page.** `cli "sensor list"` says
  `10 vars` and prints the first five; `cli "sensor list 5"` prints
  `power.low_mv`, `power.crit_mv`, `obs.wake`, `client.refused` and
  `firstboot.state` (`obs.wake` arrived with p2-03, `client.refused` with p2-04
  and `firstboot.state` with p3-03). The first page is **160 characters** on a
  node whose clock is not `set`, so it comes back cut at 159 and the digit of
  `... next:5` is gone; on a release image (a longer `fw.version`) it is 163
  and the cut is three characters — expected, counted, and the reason
  `sensor get <key>` is the way to read one key
  (`tasks/phase-3/reports/03.md`). **Five keys on the first page holds while the
  version string is 70 characters or fewer**, which today it is exactly on a
  release image; a longer MeshCore tag or a longer release version moves
  `clock.state` to the second page and makes this line read `... next:4`.
  `test/test_settings`' last case is that bound, so a bump that crosses it is
  red in CI before anybody reads it here (`tasks/phase-3.3/reports/01.md`).
- [ ] **p1-06** **The boot-voltage question, which is the reviewer's.** Upstream
  will not boot a node whose battery reads between 1000 and 3300 mV
  (`PWRMGT_VOLTAGE_BOOTLOCK`), and on this board the reading it checks is 0 at
  that moment, so the protection never fires — `reports/06.md` says why and what
  enabling it would cost. Confirm on the bench that a node **does** come up with
  the pack at 3.2 V, which is the behaviour this firmware chose, and read
  `cli "get pwrmgt.bootmv"` (it says `0 mV`), `cli "get pwrmgt.support"` (it
  says `supported`) and `cli "get pwrmgt.bootreason"` while there:
  → ______________

## 6. The mesh client

`docs/link.md` §2e and §2f are the pages: the job, its path, the extra peer, and
the five sends. Task 01 is the state; task 02 puts packets on the air, so its
walk needs a **second repeater** (any stock MeshCore repeater on the bench
channel) and a **companion as an observer**.

- [ ] **p2-01** **With a job begun and nothing sent, the repeater behaves
  exactly as before.** `job begin <any 64-hex key>` over the link, then from a
  phone: log in as admin, run a CLI command, read `status`; and a message
  through the unit from another node still passes. The extra peer is only ever
  in front of upstream's own while a reply is awaited, which this task cannot
  yet cause (no send), so none of the repeater's own admin or routing is
  touched. `status` shows flag bit 0 (`job begun`) set and bit 1 clear, and the
  job ends itself after 120 s with no further request on the link.
- [ ] **p2-02** **A login.** `job begin <its key>`, `login` (blank): answered at
  guest if the second repeater has no guest password, silence if it has one —
  both are right, and `reply` says which (state 3, a response, or state 2,
  timeout).
- [ ] **p2-02** **An admin session.** `login -` with its admin password:
  answered. Then `status-of-peer`, `neighbours-of-peer` and `send-cli "get
  name"` are each answered (`reply` shows state 3, 3 and 4), and `path` now
  shows the route the first reply came back by — the flood taught it.
- [ ] **p2-02** **One packet per send.** The observer counts **exactly one**
  packet from this unit for each of the sends above, and a login's packet is not
  readable as a password (it is an anonymous request whose body is ciphertext).
  `counters` shows `client_tx` rising by one per send and by nothing else:
  sends → ______, `client_tx` → ______.
- [ ] **p2-04** **The ceiling, and that a correct Walter never meets it.**
  `counters` first (`client_tx` → ______, `cli "sensor get client.refused"` →
  ______, which should be `0`). Then send **seven** things inside a minute,
  two seconds apart — a login, `status-of-peer`, `neighbours-of-peer`,
  `send-cli "get name"`, `discover`, a trace, and one more: the **seventh is
  refused `ERR_BUSY`**, `status` shows flag bit 1 **clear** (so it was the
  ceiling and not a reply being awaited), the observer sees **no packet** for
  it, `client_tx` has risen by six and `client.refused` reads `1`. Wait a
  minute and the next send goes. `docs/link.md` §2h is the page.
- [ ] **p2-04** **And the two-second gap.** Two sends as fast as `linkcli.py`
  can issue them (`--timeout-ms 0 --no-wait` twice in one shell line): the
  second is `ERR_BUSY`, nothing leaves for it, `client.refused` goes up by one
  → ______. Repeating is unaffected throughout, and `client.refused` is
  readable from the USB console and from a mesh admin's phone — but **not** over
  the link, on purpose.
- [ ] **p2-02** **A trace.** `trace <the learned path>` comes back with an SNR
  per hop (`reply` state 5), and `trace ... --hash-size 3` is refused
  `ERR_RANGE` before anything leaves.
- [ ] **p2-02** **Repeating is unaffected throughout.** While all of the above
  runs, a message through the unit from another node still arrives, and a phone
  can still log in to the unit itself and read its `status` — the extra peer is
  in front only for the seconds a reply is awaited.
- [ ] **p2-02** **A peer that is also an admin of this unit.** `job begin` with
  the key of a repeater that is also in this unit's own ACL as an admin: its CLI
  reply is read as the job's **reply** and nothing is executed on this unit
  (`docs/ARCHITECTURE.md` §2, the reason the peer is in front). This is the one
  that would go wrong if the extra peer were behind upstream's own.
- [ ] **p2-02** **A reply's SNR is the packet's own.** `reply` and
  `EV_MESH_REPLY` carry a real SNR for every reply. Its `rssi` comes from the
  note `logRx` takes, which task 03 added, so it reads a number now and not
  *unknown* — §7's last line but one is where that is walked. An *unknown*
  `rssi` with a sensible SNR is not a fault: it means the note for that packet
  was pushed out of a 32-entry table while the packet waited in the queue.

## 7. Observations

`docs/link.md` §2g is the page. It needs **ten minutes on the roof antenna** and
nothing else: no second radio, no companion — the mesh advertises by itself.

- [ ] **p2-03** **The adverts are there, and they look like a neighbourhood.**
  After ten minutes on the roof antenna, `linkcli.py obs` lists the repeaters in
  range with plausible hops, SNR and RSSI, oldest first (records → ______,
  `adverts` → ______). **A direct neighbour
  has hops 0 and an RSSI**; a record with hops has the path it arrived by and no
  RSSI only if it waited a long time in the inbound queue. `obs --names` shows
  the names, decoded on the laptop. `counters`' `adverts` matches roughly what a
  console `neighbors` and the air would suggest, and `adverts_drop` is 0.
- [ ] **p2-03** **`neighbors` on the unit's own console still lists its
  neighbours as before.** Upstream's table is filled first, from the same
  advert, and nothing of ours can change what it made of it — the one thing that
  would go wrong if `onAdvertRecv` did not call its base first.
- [ ] **p2-03** **A pull empties what it returns, and a retry does not.**
  `obs 2` twice hands over four different records; `status`' `obs_pending` falls
  by two each time. With `-v` and a disconnected TX for one second (or
  `--attempts 3` against a node that is busy), the retried frame comes back with
  the *same* records and `obs_pending` falls once.
- [ ] **p2-03** **With the laptop away for a day, `status` shows the ring full
  and a loss count, and the repeater is none the worse.** `obs_pending` 64,
  `obs_dropped` well above zero, and the first `obs` then reports that same
  `dropped` once and zero on the next pull. Repeating, a phone's login and
  `neighbors` are all unaffected.
- [ ] **p2-03** **A reply's RSSI is a number now.** With a job and a login to
  the second repeater (§6), `reply` shows an `rssi` in dBm and not *unknown* —
  the note `logRx` takes is what fills it, and this is the line that was owed
  from task 02.
- [ ] **p2-03** **`obs.wake`, on.** `sensor set obs.wake 1` on the console, then
  `sleep 600` from the laptop and leave the ring to fill past 48 records: within
  one advert of crossing it the node raises `EV_ATTENTION` reason 5 with the
  records waiting as its `detail`, at most once an hour, and `sensor get
  obs.wake` reads 1. With the setting back at 0 — the default, and what a fresh
  boot gives — a ring that fills right up raises nothing.

## 8. What an image says it is

`spec/FIRMWARE-MANIFEST.md` §4 is the page. Nothing here needs a radio: a
release's files, a flash, and one `hello` over the link.

- [x] **p3-01** **The file you are about to flash says what it is, before you
  flash it.** On a release's `.bin` (or on
  `out/build/repeater_rak4630_single/firmware.bin`, which `tools/verify.ps1`
  leaves): `python spec/tools/manifestref.py descriptor <file>` prints one
  descriptor and its offset (role → ______, board → ______, band → ______,
  version → ______, offset → ______). Two descriptors or none is a refusal, and
  the build would have failed first — what is being checked on the bench is
  that the file which left the release is still that file. (bench, 2026-10-22, 9fb721b)
- [ ] **p3-01** **The unit then says the same thing.** After flashing it,
  `linkcli.py hello`'s `role`, `board` and `fw_version` are the three the
  descriptor gave, and `slot` is **0** on the single-RAK adapter. They are read
  out of the descriptor object inside the running firmware, so a disagreement
  here means the image on the board is not the image that was checked.
- [x] **p3-01** **The console agrees too.** `sensor get fw.version` on the USB
  console reads `twinoak-repeater <version> on MeshCore <tag> (<pin>)` with the
  same version, and upstream's own `ver` line is unchanged. (bench, 2026-10-22, 9fb721b)
- [ ] **p3-01** **The Walter's half, in the Walter's repository.** That a Walter
  reads the descriptor out of the file it downloaded, compares it with the
  manifest entry and with the radio position it is about to flash, and refuses
  an image for another board or band, is `twinoak.meshcore.walter-firmware`'s
  own tests against the same fixtures (`spec/fixtures/manifest/`). No session
  and no bench of this repository can show it; it is listed here so that the
  two halves are ticked together when that firmware exists.

## 9. NoiseScope

`docs/noisescope.md` is the page. The scanner is the second image of the
family: it is flashed in place of the repeater, and the repeater is flashed
back. Both halves of that round trip are here, and the second one is the point
of the first.

- [x] **p3-02** **Flash the scanner over USB.** The UF2 from
  `out/build/scanner_rak4630_single/firmware.uf2` on the bootloader's drive (or
  `adafruit-nrfutil dfu serial -pkg firmware.zip -p COMx -b 115200`). The USB
  console at 115200 should show one `boot` line within a second: `fw` →
  `noisescope`, `role` → `scanner`, `board` → `rak4630-single`, `ver` →
  ______, `reset` → ______, **`radio_ok` → ______**.
  **If `radio_ok` is false, the power-enable pin is the first thing to look
  at** (`SX126X_POWER_EN`, P1.05): this port drives it high where NoiseScope
  never did, and no session can tell whether the stamp needed it
  (`tasks/phase-3/reports/02.md`) — on the first real board it read **true**,
  p3.4-02. The `boot` line itself is a race (§0a): when it was gone before the
  port came back, `info`'s own `fw`, `role`, `board`, `ver`, `reset` and
  `radio_ok` are what this line is held to. (bench, 2026-10-22, 9fb721b)
- [x] **p3-02** **`info`, `stat` and `sweep` behave as the page says**, on the
  USB console and on `Serial1` — both at 115200, both serving the same
  protocol. `info` answers one line; `stat` answers one immediately and one
  every 10 s; `sweep` acks `started` and answers a `sweep` line of **281
  points** in about 7 s (n → ______, rbw → ______). Then `hist 869.4 869.8 25`
  (the RAM patch uploads: lines, or an `err` → ______), `dwell 869.618 30`,
  `cad`, and `stop` in the middle of a sweep — which must be answered between
  steps and not after the pass. (bench, 2026-10-22, 9fb721b)
- [ ] **p3-02** **[boards] `pwr` matches the repeater's `power` numbers for the
  same board.** `stat`'s `pwr` object against what the repeater answers for
  POWER_GET on the same connector board, within a reading: vpanel → ______,
  ipanel → ______, vbat → ______, ibat → ______, vload → ______, iload →
  ______. The two now read the part through the same code
  (`src/common/power/`), so a disagreement here is a disagreement between two
  runs and not between two firmwares — which is exactly why this line is worth
  walking.
- [x] **p3-02** **Every line is JSON, and the chatter says so.** A minute of
  output through `python tools/noisescope/waterfall_plot.py --port COMx`, which
  drops anything not starting with `{` — or
  `python -c "import sys,json;[json.loads(l) for l in sys.stdin if l.startswith('{')]"`
  over a captured log. No line should fail to parse and none should carry
  `nan`. The native tests hold the writer to this
  (`test/test_scanner`); what the bench adds is a real radio's readings
  going through it, including a window with no sample. (bench, 2026-10-22, 9fb721b)
- [x] **p3-02** **Flash the repeater back, and nothing of the node is gone.**
  The repeater's `.uf2` or `.zip`, then on the USB console: the same
  `Repeater ID:` as before the scanner (→ ______), the same `name`, the same
  admin password still accepted, and the neighbours list filling up again.
  `sensor get fw.version` reads `twinoak-repeater …`. **This is the promise the
  scanner's two rails exist for** — it compiles no file system and never
  transmits — and it is the one thing on this page that only a board can show.
  Expect **two DFUs**: the scanner's watchdog bites in the middle of the first
  one and `bench` runs the second from the bootloader that is left (§0a). (bench, 2026-10-22, 9fb721b)
- [ ] **p3-02** **The watchdog is live.** With the scanner flashed, hold the
  unit in a `hist` pass and pull the SX1262 into a stall if you can contrive
  one, or simply confirm from a `boot` line after an induced hang that `reset`
  reads `wdt`. A reset that boots into passive monitor mode is the safe end of
  a bad day on a mast; a session cannot cause one.

### The fast sweep

`docs/noisescope.md` §6 is the page. Six lines, in this order: the first one
decides whether any of the rest can be walked at all.

- [x] **p3.1-01** **The radio's ROM.** `info` on the RAK4630 and read `rom` →
  ______. The sweep plugin is a program for the radio's own core and is made
  for **`SX1261 V2D 2D02`**; **if it says anything else, stop here** — `scope`
  will refuse and say so, nothing is uploaded, and the rest of this list is
  not walkable on that part. (`sweep`, `hist`, `dwell` and `cad` are
  unaffected: they use no plugin of this kind.) (bench, 2026-10-22, 9fb721b)
- [x] **p3.1-01** **`scope 863 870` over USB, with the plotter.**
  `python tools/noisescope/waterfall_plot.py --port COMx --scope 863 870`.
  What to see: an `ack` naming what was taken (n → ______, rbw → ______, seg
  → ______, cov → ______), then frames arriving at **tens of sweeps a
  second** — `sps` → ______ — a spectrum on top and a waterfall under it, and
  the home channel's own traffic showing as bursts in the waterfall. `sps`
  should agree with the fork's numbers: about 21 ms for 250 points at
  19.5 kHz, about 5 ms at 78 kHz. A `cov` below 1 draws a grey band; at this
  span it should not. (bench, 2026-10-22, 9fb721b)
- [x] **p3.1-01** **The baseline.** A **50 Ω dummy load** in place of the
  antenna, `scope 150 960 2048`, captured to a file (`--save` writes the
  picture; keep the NDJSON too). This is the radio's own spurs and the shape
  of the matching network across the whole tuning range, and every later
  reading is worth more with it to subtract (`--baseline`). Keep the file with
  the site's notes. Peaks seen on the dummy load → ______. (bench, 2026-10-22, 479f482)
- [ ] **p3.1-01** **The image check**, which is what the segments exist for.
  The tinySA Ultra as a generator at a known frequency, through an attenuator
  into the RAK, and a `scope` of a wide span around it: there should be **one
  peak where the generator is**, and nothing at the image (or no more than the
  baseline already had). If a ghost shows, lower `kScopeSegmentMhz` in
  `src/roles/scanner/Sweep.h` until it does not, and **write the number down**
  → ______ (the task's follow-ups are where it goes).
- [ ] **p3.1-01** **The site through the cavity filter.** `scope 791 960` with
  the baseline subtracted: LTE800 downlink (791–821) and GSM900 downlink
  (925–960) as they actually reach the radio. Strongest carrier → ______ at
  ______ MHz. This is the picture to keep beside the repeater's own noise
  figures.
- [ ] **p3.1-01** **The same `scope` from the Walter UART.** Over `Serial1`,
  `scope 863 870 2048`: the `ack`'s `interval` should be **several hundred
  milliseconds** and not 0 — the port's own floor (→ ______ ms) — the frames
  should arrive at about that rate, the `stat` heartbeat should keep coming on
  **both** ports, and after **ten minutes** there should have been no reset
  (`info` again: `reset` still → ______, uptime still climbing). A scope from
  USB must put **no** `scope` line on the UART, and the other way round — the
  easiest check is two consoles open at once.

## 10. First boot and provisioning

`docs/provisioning.md` is the page. This is the one thing on it no session could
check: a board that has never been configured, brought up twice — once at the
USB port and once over the link.

**Do this last, or on a spare module.** The first line erases the file system,
which takes the node's **identity** with it: the key the mesh knows it by, and
the one MeshMaster would have been told. A unit that has been adopted is a unit
not to erase.

- [x] **p3-03, p3.3-01** **A first boot is a first boot.** `erase` on the USB
  console (`File system erase: OK`), then reset. The console prints
  `Repeater ID:` with a **new** key (→ ______), then `First boot: a random admin
  password was set (not shown; …)` and `First boot: the profile ran 2 of 2 lines
  (sensor get firstboot.state says 'done'; …)`. Then, and **`get dutycycle`
  first** — it is the line this whole section exists to prove, because it is the
  one thing a fresh unit was getting wrong (`docs/ARCHITECTURE.md` §7):
  `get dutycycle` → `> 10.0%` (the band's, where upstream's own default is
  `> 50.0%`); `sensor get firstboot.state` → `> done`;
  `get path.hash.mode` → `> 1`; `get radio` → `> 869.618,62.5,8,8`;
  `get name` → `> RAK4631 Repeater` (this build's `-D ADVERT_NAME` in `platformio.ini`;
  upstream's own default is `repeater`); `get guest.password` → `> ` (empty). (bench, 2026-10-22, 9fb721b)
- [x] **p3-03, p3.3-01** **It runs once, and a reboot proves it.** `reboot`, and
  this time there is **no** `First boot:` line at all; `sensor get
  firstboot.state` → `> never`, `get path.hash.mode` is still `> 1` and
  `get dutycycle` is still `> 10.0%` — the profile's work was saved, both lines
  of it, and the profile did not run again. (bench, 2026-10-22, 9fb721b)
- [ ] **p3-03** **A power cut in the middle leaves no half-provisioned node.**
  `erase`, reset, and pull the power **during** the first second (before the
  `First boot:` lines appear). On the next boot one of exactly two things is
  true: the `First boot:` lines appear again and `firstboot.state` says `done`
  (the write never happened, so the whole thing was done again), or they do not
  appear and `get path.hash.mode` says `1` (the write happened and the unit is
  finished). What must **never** be seen is no `First boot:` line together with
  `get path.hash.mode` → `0`, or with `get dutycycle` → `> 50.0%`: that would be
  a unit that is finished as far as it knows and outside the band. Worth three
  or four attempts; the arrangement that makes it so is one preferences write
  (`src/roles/repeater/app/FirstBoot.h`).
- [x] **p3-03** **Provision it at the USB port**, `docs/provisioning.md` §3 line
  for line: `set name`, `set lat`, `set lon`, `set owner.info`,
  `set guest.password`, `region default dk`, `region save`, `password <new>`.
  Every reply should be the one the page quotes — and the one to watch for is
  `region default dk` answering ` default scope is now dk` rather than `OK`.
  Read it back with `get name`, `get lat`, `get owner.info`, `region default`. (bench, 2026-10-22, 9fb721b)
- [ ] **p3-03** **A phone then logs in with that admin password**, and is an
  admin: `sensor get link.errors` from the phone answers, and
  `sensor set walter.wake 1` raises an attention on the link (§4 has that
  line). The old random password is gone, and so is `not-a-password`.
- [ ] **p3-03** **Provision it over the link.** `erase` and reset again, then
  from the laptop: `python3 tools/linkcli.py --port <port> provision unit.txt`
  with the file of `docs/provisioning.md` §4. Every line should be taken, the
  `password` line should print `password ****` and `accepted`, and **the
  password must appear nowhere on the laptop's screen**. Then take a line out
  of the middle and break it (`set nonsens 1`): the run stops there, says which
  line of the file it was, and `get lat` shows that nothing after it was sent.
- [x] **p3-03** **And the node is the same node afterwards.** `linkcli.py
  identity` reads the public key and the name just set (→ ______), which is
  what MeshMaster has to be told along with the admin password
  (`docs/provisioning.md` §5). Write both down **now**: the password set above
  is the only copy there is, and nothing in this firmware can read it back. (bench, 2026-10-22, 9fb721b)