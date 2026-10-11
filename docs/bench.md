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
  says **`9 vars`** and prints the first five of them -
  `fw.version=twinoak-repeater 0.0.0-dev on MeshCore repeater-v1.17.1
  (d9296435)` (a release build says its version instead), then `walter.wake`,
  `link.errors`, `walter.seen` and `clock.state` - and `... next:5`, because
  upstream stops a listing at 134 bytes; `sensor list 5` is the other four
  (§5's last-but-one line walks the paging). `sensor get fw.version` is the
  same string; `sensor set fw.version x` is refused (`can't find custom var`,
  upstream's wording for a key that took no value).
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
- [ ] **p1-05** `cli "sensor list"` says `9 vars` and then `fw.version`,
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
  `9 vars` and prints the first five with `... next:5`; `cli "sensor list 5"`
  prints `power.low_mv`, `power.crit_mv`, `obs.wake` and `client.refused`
  (`obs.wake` arrived with p2-03 and `client.refused` with p2-04). On a release
  image (a longer `fw.version`) the first page is two bytes over the window and
  comes back cut at 159 characters — expected, counted, and the reason
  `sensor get <key>` is the way to read one key (`reports/06.md`).
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
