# The box link, from a laptop

`spec/LINK-PROTOCOL.md` is the protocol — the UART between this firmware on the
RAK4630 and the Walter in the LTE box. The Walter does not exist yet, so this
page is how a person talks to the link instead: a 3.3 V USB-UART dongle and
[`tools/linkcli.py`](../tools/linkcli.py).

What the firmware answers today (phase 1's tasks 03 to 06, and phase 2's task
01): `HELLO`, `PING`, `STATUS`, `TIME_SET`, `TIME_GET`, `IDENTITY`, `REBOOT`,
`CLI`, `SLEEP_NOTICE`, `POWER_GET`, `ENV_GET`, `LPP_GET`, the four job requests
`JOB_BEGIN`, `JOB_END`, `PATH_SET` and `PATH_GET`, and `EV_BOOT` and
`EV_ATTENTION` as events. Everything else of the protocol answers
`ERR_UNSUPPORTED` until the task that owns it lands — the five sends, the reply
and the counters of phase 2's task 02, the observation ring of its task 03 —
and `HELLO`'s `caps` is `0x6f` (`CLI`, `POWER`, `ENV`, `MESH_CLIENT`,
`ATTENTION`, `TIME_PERSIST`) and nothing else for exactly the same reason: a
capability is an interface that is there (`src/link/session/Interfaces.h`).
`LPP_GET` has no capability bit of its own: §4 gates it on `CLI`.

## 1. Wiring the dongle

**3.3 V, not 5 V.** The RAK4630 is a 3.3 V part and the lines are push-pull
with no protection beyond the bulkhead filter.

| | RAK side | Dongle |
|---|---|---|
| RAK receives | P0.15 (`Serial1` RX) | **TX** |
| RAK transmits | P0.16 (`Serial1` TX) | **RX** |
| Reference | GND | GND |

Two places to land it, from the hardware repository's `INTERCONNECT.md`:

- **At the adapter**, on the link header — the jumpers **J4/J5 on 1–2**, which
  is what puts `Serial1` on the D-sub instead of the WisBlock GPS the stock
  MeshCore variant expects.
- **At the D-sub**, pins **8 and 9** through the harness, which is the real
  path: 470 Ω in series and about 2.2 nF to ground on each line, twice (one
  bulkhead filter per box). That is the version worth trusting, because it is
  the one the Walter will see.

115200 baud, 8 data bits, no parity, 1 stop bit, **no flow control**. Leave the
dongle's RTS/CTS and DTR alone; `linkcli.py` opens the port with them off.

Nothing on the link is powered by the dongle. Either end may be unpowered or
resetting while the other is alive, and neither waits for the other.

## 2. A session

```bash
python3 tools/linkcli.py --port COM7 hello        # /dev/ttyUSB0 on Linux
python3 tools/linkcli.py --port COM7 ping
python3 tools/linkcli.py --port COM7 status
python3 tools/linkcli.py --port COM7 time         # read the clock
python3 tools/linkcli.py --port COM7 time set     # set it to this laptop's
python3 tools/linkcli.py --port COM7 identity
python3 tools/linkcli.py --port COM7 cli "get radio"   # a console command line
python3 tools/linkcli.py --port COM7 power        # panel, battery and load
python3 tools/linkcli.py --port COM7 env          # the air inside the box
python3 tools/linkcli.py --port COM7 lpp          # what a phone's telemetry view gets
python3 tools/linkcli.py --port COM7 sleep 600    # "this end is asleep for 10 min"
python3 tools/linkcli.py --port COM7 listen 30    # the events, for 30 s
python3 tools/linkcli.py --port COM7 reboot 0     # and then `listen` for EV_BOOT
python3 tools/linkcli.py --port COM7 job begin <key> --path aabb,ccdd  # a peer and a route
python3 tools/linkcli.py --port COM7 path         # the route the job holds
python3 tools/linkcli.py --port COM7 job end      # forget the peer
```

`hello` is the first request of a session and may be repeated at any time. It
is how you learn what you are talking to:

```
HELLO
  protocol      1.0
  role          1 (repeater)
  slot          0
  max_payload   240
  caps          0x6f (CLI, POWER, ENV, MESH_CLIENT, ATTENTION, TIME_PERSIST)
  boot_id       0x5eed1234
  fw_version    0.0.0-dev
  board         rak4630-single
  upstream      repeater-v1.17.1+d9296435
```

- `boot_id` is random and drawn once per boot, from the radio's noise listener.
  **A changed `boot_id` means the node restarted** and every piece of per-boot
  state is gone.
- `fw_version` is the release's own version — `0.0.0-dev` in a local build —
  and `upstream` is the MeshCore it is built on, `<tag>+<pin8>`. The USB
  console's `sensor get fw.version` says both in one sentence instead.
- `caps` grows as the later tasks land: `0x6f` today, and phase 2's task 03
  adds `OBSERVATIONS`. Nothing else of this page changes with it.

`ping` proves the main loop is turning, which a reset line cannot: it echoes
the nonce and reports the uptime. `status` is the one to read when something is
wrong — the reset reason, the clock's state, the flag bits, and
`link_errors`, which counts the frames this node's receiver had to drop.

`reboot <mode>`: 0 restarts the application, 1 enters the UF2 bootloader, 2
serial DFU, 3 over-the-air DFU. **All four work on this board**, and 1 to 3
hand the radio to a bootloader that this link cannot talk to — they exist for a
person at the field USB port, and the way back is a power cycle or a flash. The
response leaves the UART before the restart, so `reboot 0 --delay-ms 500` is
answered and then goes quiet; `listen` after it shows the `EV_BOOT` of the
boot that follows.

`-v` prints every frame as hex, in both directions, which is the thing to paste
into a report when a frame is refused.

## 2a. The clock

The first-batch boards have **no RTC fitted**, so a repeater that restarts has
no idea what time it is — and MeshCore judges replays by timestamps, so a clock
that goes backwards makes this node's own adverts look like replays to every
node that heard the later ones. `docs/ARCHITECTURE.md` §5 is the design; from a
laptop it looks like this:

```
$ python3 tools/linkcli.py --port COM7 time
TIME_GET
  epoch         1791740555 (2026-10-11 17:42:35 UTC)
  time_state    1 (restored)

$ python3 tools/linkcli.py --port COM7 time set
TIME_SET 1791736955 (2026-10-11 16:42:35 UTC), source 3 (a person at a console)
  prev_epoch    1791740555 (2026-10-11 17:42:35 UTC)
  epoch         1791736955 (2026-10-11 16:42:35 UTC)
  time_state    2 (set)
```

- `time_state` **0 unset** is the firmware's built-in date in 2024 and is never
  right. **1 restored** is carried over from before the reset: never earlier
  than anything this node has already sent, but up to an hour *ahead* of the
  truth, because the node cannot know how long it was down. **2 set** means
  somebody set it over this link (or at the console) since boot. **3 rtc** means
  a hardware RTC answered, which no batch-1 board can do.
- **A set may always move the clock forward.** Backwards it is refused with
  `ERR_RANGE` unless `--force` is given, *or* the state is still `unset` or
  `restored` and nothing carrying a timestamp has left the node since it booted
  — which is the normal case, because the boot advert is held back until the
  clock is set. So a Walter (or a person) that sets the time promptly may
  correct a restored clock in either direction; one that waits may not, and
  `--force` is then the override, on a person's instruction.
- An epoch before 2026 or after 2099 is `ERR_RANGE` whatever `--force` says:
  this firmware did not exist then.
- `status` says the same thing in `time_state` and `epoch`, and its **flag bit
  3** (`adverts held`) is on while the boot advert is waiting for a clock. It
  clears when the clock is set or after 60 seconds, whichever comes first. The
  USB console's `sensor get clock.state` is the same state by name, readable
  over the mesh by an admin as well.
- The node keeps its own lower bound in a file (`/clock`, rewritten at most once
  an hour and always just before a restart it performs itself), which is what
  `restored` is restored from. There is **no** surviving-RAM copy on this board;
  `tasks/phase-1/reports/04.md` says why, and the consequence is that every
  reset costs up to an hour of being ahead until something sets the clock.

## 2b. The console, over the link

`cli "<line>"` runs one command line on the node **as if it had been typed on
the USB console** — with console privileges, which is more than a mesh admin
gets: `stats-core`, `get prv.key`, `set freq` and `erase` all work. The Walter
is trusted with that; it already holds the SWD lines.

```
$ python3 tools/linkcli.py --port COM7 cli "get radio"
CLI get radio
  > 869.618,62.5,8,8

$ python3 tools/linkcli.py --port COM7 cli "sensor list"
CLI sensor list
  5 vars
  fw.version=twinoak-repeater 0.0.0-dev on MeshCore repeater-v1.17.1 (d9296435)
  walter.wake=0
  link.errors=0
  walter.seen=0
  clock.state=set
```

- **A line is 1 to 160 bytes of UTF-8 text with no control characters in it.**
  Nothing else is run: an empty line, a longer one, a tab or a newline in it, a
  byte that cannot be UTF-8 — each is `ERR_MALFORMED`, and the interpreter is
  never asked.
- **The reply is at most 159 characters**, because that is the buffer upstream's
  own console gives its interpreter and several of its commands assume exactly
  that number. A reply that would be longer is cut, not spilled: `sensor list`
  pages with `... next:<n>` by itself (so on a node that has been up a long
  time, `sensor list 4` is how the last key is read), and `neighbors` stops
  adding records near the limit.
- **Four commands never answer**, because they restart the node: `reboot`,
  `clkreboot`, `poweroff` and `shutdown`. Silence after one of those is the
  expected outcome, and `linkcli.py` says so rather than calling the link down.
  `listen` afterwards shows the `EV_BOOT`.
- **`region load` is the one line not to send from here.** It puts upstream's
  interpreter into a mode where every following line is a region definition
  until a *blank* one ends it — and a blank line is not a legal `CLI` request.
  The way out is one space (`cli " "`), which upstream reads as blank, or a
  `reboot`.
- Nothing about a `CLI` line or its reply is ever printed by the firmware: a
  line can be `password <new>`, and upstream's reply to that one echoes the
  password back. A test walks `src/link/` and fails on any print-like call
  (`test/test_rails`).

### Our settings, from here, from the console and from the mesh

The seven keys of `docs/ARCHITECTURE.md` §4 are on upstream's own `sensor` CLI,
which means three ways to reach them: `cli "sensor get <key>"` over this link,
the same line typed on the USB console, and — for a **mesh admin**, logged in
with the admin password from a phone — the same line as a message. Upstream's
permission check is the whole gate (`MyMesh::onPeerDataRecv` runs a text message
through the interpreter only for a client whose ACL entry is admin).

| Key | `get` | `set` |
|---|---|---|
| `fw.version` | this firmware and the MeshCore it is built on, in one sentence | — |
| `walter.wake` | `0`, or `1` while an attention is pending | **`1` asks for the Walter's attention** (reason 4) |
| `link.errors` | frames this node's receiver dropped since boot | — |
| `walter.seen` | seconds since the last valid request on the link, or `never` | — |
| `clock.state` | `unset`, `restored`, `set` or `rtc` | — |
| `power.low_mv` | the *battery low* threshold in millivolts (3500 by default) | a decimal number of millivolts |
| `power.crit_mv` | the *battery critical* threshold (3300 by default) | the same |

A `set` of any other key — or of `walter.wake` with any value but `1` — is
answered `can't find custom var`, which is upstream's wording and its only other
word. The two thresholds are refused the same way when the value is not a plain
decimal number, when it is outside 2500–4300 mV, or when `low` would not sit
more than 100 mV above `crit`; a `set` that is accepted is written to a small
file of this firmware's own (`/power`) and survives a restart.

**Seven keys no longer fit one `sensor list`.** Upstream stops a listing once it
has written 134 bytes into a 160-byte reply, so a freshly booted node prints the
first five and `... next:5`, and **`cli "sensor list 5"`** is how the two
thresholds are read; a node whose counters have grown wide prints four and
`... next:4`. `sensor get <key>` always works whatever the paging does, and
`tasks/phase-1/reports/06.md` has the arithmetic.

## 2c. Attention, and a Walter that sleeps

The Walter is powered from the same battery and spends most of its life asleep.
`sleep [seconds]` is what it sends last before it goes, and this end can send it
too to watch what follows:

```
$ python3 tools/linkcli.py --port COM7 sleep 600
SLEEP_NOTICE, back in 600 s
  status        0 (OK)
```

From then until **any** valid request arrives, the node believes this end is
asleep (`status` flag bit 2) and sends no events but `EV_BOOT` and
`EV_ATTENTION`. Any request at all wakes it again; nothing else does.

An attention is how the node asks for the box when something happened that
should not wait for its next wake. It sends `0x00 0x00` — two bytes to wake a
UART, which `linkcli.py` reads as an empty frame and ignores — waits 300 ms, and
then sends `EV_ATTENTION` every second with `repeat` counting from 0, **ten
times at most**, stopping at the first valid request:

```
$ python3 tools/linkcli.py --port COM7 listen
listening (control-C to stop)
EV_ATTENTION seq 7
  reason        4 (a person asked for the Walter)
  detail        0 (nothing)
  repeat        0 of 10
EV_ATTENTION seq 8
  ...
  repeat        1 of 10
```

| `reason` | | `detail` |
|---:|---|---|
| 1 | the node restarted | the reset reason |
| 2 | battery low | battery millivolts |
| 3 | battery critical | battery millivolts |
| 4 | a person asked for the Walter | 0 |
| 5 | the observation ring is nearly full | records waiting |
| 6 | a fault the node wants reported | a code of the firmware's own |

- **One at a time, with a queue behind it**, and the same reason is never
  queued twice: the second battery-low of one discharge is the same news as the
  first.
- **One new attention per reason per hour**, counted from when it was raised and
  kept in RAM, because each one costs the box an LTE attach. A second
  `sensor set walter.wake 1` inside the hour raises nothing — `sensor get
  walter.wake` still reads `1` while the first is pending.
- **The reason stays in `status`'s `attention` field** until a request has been
  answered, used up or not; the answer to the request that ends it still carries
  it, so a Walter that woke for its own reasons still learns why it was wanted.
- **Reason 1 is not sent on this board.** It is owed only to a Walter that was
  asleep when the node restarted, and nothing in this board's RAM survives a
  reset to say whether it was (`tasks/phase-1/reports/04.md`). `EV_BOOT` tells
  an awake Walter everything that event would.
- **Reasons 2 and 3 are the battery**, raised from channel 2's bus voltage with
  hysteresis and a minimum time below, so that a transmit burst's sag raises
  nothing: the voltage has to stay under the threshold for a **minute**.
  `detail` is the millivolts. The thresholds are `power.low_mv` and
  `power.crit_mv` above, and §2d has the rest. Reasons 5 and 6 arrive with
  phase 2; the machine they go through is this one.

**The out-of-band way in.** A unit whose Walter is asleep can be woken by
anything that can reach the repeater on the mesh:

```
(from a phone, logged in as admin, to this repeater)
  sensor set walter.wake 1
  -> ok

(on the link, a second later)
EV_ATTENTION reason 4 (a person asked for the Walter), repeat 0 of 10
```

## 2d. Power and environment

The unit knows its own power: what the panel gives, what the battery takes or
gives, what the load draws — **each with its own shunt** — and the temperature,
humidity and pressure inside the enclosure. Stock MeshCore applies one shunt
value (0.1 Ω) to all three INA3221 channels; on this hardware that is right for
channel 1 and wrong by a factor of five and two for the others, and it is the
reason the unit runs custom firmware at all (the hardware repository's
`CURRENT-SENSE.md` §4).

```
$ python3 tools/linkcli.py --port COM7 power
POWER_GET
  valid         0x07 (channel 1, channel 2, channel 3)
  1 panel      18096 mV     +120 mA   100 mohm, + = into the charger
  2 battery     4008 mV      -52 mA   20 mohm, + = charging
  3 load        3984 mV      +85 mA   50 mohm, + = drawn
channel 2 is negative, so the pack is **discharging** (5.8)

$ python3 tools/linkcli.py --port COM7 env
ENV_GET
  valid         0x07 (temperature, humidity, pressure)
  temperature   -3.50 C
  humidity      81.25 %
  pressure      101325 Pa (1013.25 hPa)
```

- **Read the sign first, then the size.** Channel 2 is the only one that goes
  negative, and it means the pack is being drained; positive is charging.
  Channels 1 and 3 are positive in normal service.
- **A channel whose `valid` bit is clear reads 0** — a missing reading, never a
  stale one. With the B-board unplugged, `power` answers `valid 0` and the
  repeater keeps repeating: nothing on this bus can hold up forwarding.
- **No request ever touches the I2C bus.** The three channels are swept once a
  second and the BME280 every five, both out of the firmware's own `loop()`;
  every request answers out of that cache. A sensor that stops answering is a
  missing reading and the firmware leaves it alone for five seconds before
  asking again (`src/power/PowerReader.h`).
- The battery voltage a `status` reports over the mesh, and the one upstream's
  own power management reads, is **channel 2's bus voltage** — not the WisBlock
  ADC pin, which is not wired on this adapter.

### The LPP channel table

`lpp [mask]` answers with exactly the Cayenne-LPP bytes a mesh telemetry request
at those permissions would get — which is how the Walter reports what a phone
was told. **These channel numbers are chosen once and never renumbered**:

| LPP channel | What | Fields | Whose |
|---:|---|---|---|
| 1 | the node itself | battery voltage, MCU temperature | upstream's (`TELEM_CHANNEL_SELF`) |
| 2 | panel | voltage, current | ours |
| 3 | battery | voltage, current | ours |
| 4 | total load | voltage, current | ours |
| 5 | the enclosure | temperature | ours |

```
$ python3 tools/linkcli.py --port COM7 lpp
LPP_GET perm_mask 0x05 (base, environment)
  36 byte(s): 017401900274071102750078037401900375ffcc0474018e047500550567ffdd016700d4
  channel 1  self       voltage        4.000 V
  channel 2  panel      voltage       18.090 V
  channel 2  panel      current        0.120 A
  channel 3  battery    voltage        4.000 V
  channel 3  battery    current       -0.052 A
  channel 4  load       voltage        3.980 V
  channel 4  load       current        0.085 A
  channel 5  enclosure  temperature   -3.500 C
  channel 1  self       temperature   21.200 C
```

- Channels 2 to 5 need the **environment** permission (`0x04`), which is the
  gate upstream's own sensor manager uses. `lpp 1` — base alone, which is what a
  guest gets — is channel 1 and nothing else.
- **The enclosure's humidity and pressure are not in the LPP.** CayenneLPP
  1.6.1's own type gate does not list relative humidity or barometric pressure,
  so `addRelativeHumidity` and `addBarometricPressure` write nothing at all —
  upstream's library, not our choice. `ENV_GET` on this link carries all three,
  which is where to read them until that is fixed upstream
  (`tasks/phase-1/reports/06.md`).
- A voltage is 0.01 V per step and a current 0.001 A, both truncated towards
  zero, so `4008 mV` reads back as `4.00 V`. That is Cayenne-LPP's resolution
  and it is the same on the mesh.

## 2e. The mesh client's job

A unit is one MeshCore node with one identity (`spec/SYSTEM.md` S-1). With
`MESH_CLIENT`, the link lends that identity to the Walter for the handful of
exchanges an exit node makes with *another* repeater — a login, a request, a
CLI line, a path discovery, a trace. Phase 2's task 01 is the state those
exchanges hang on: a **job** with one peer and a path. The sends themselves are
task 02; nothing is transmitted on the mesh yet.

```
$ python3 tools/linkcli.py --port COM7 job begin a1b2…8f90 --path a1b2,7f3a
JOB_BEGIN a1b2c3d4..., path hops
  status        0 (OK)

$ python3 tools/linkcli.py --port COM7 path
PATH_GET
  path_mode     2 (hops)
  hash_size     2
  hop_count     2
  hashes        a1b27f3a
```

- **One peer at a time.** `job begin <key>` ends any job in progress first,
  then makes that 64-hex-character key the peer. A second `job begin` ends the
  first. `job end` forgets the peer, the path and any awaited reply, and wipes
  the shared secret.
- **The path** is how the next send goes out: `--path aabb,ccdd` is the hops to
  the peer (comma-separated hex, each hop 1 to 3 bytes); `--direct` is a direct
  neighbour, zero hops; and neither flag is a flood. `path` reads back the route
  the job holds now — which, once task 02 lands, is also the route a flooded
  send's reply came back by. `PATH_SET` replaces it, and `path_mode` 0 forgets
  it; `PATH_SET` with no job is `ERR_STATE`, and a path the protocol does not
  allow (a hash outside 1–3 bytes, more than 63 hops or 64 bytes, a hop count
  that disagrees with the bytes) is `ERR_MALFORMED` and changes nothing.
- **`STATUS`'s flag bit 0** (`job begun`) is on while a job is; bit 1 (`reply
  awaited`) is task 02's.
- **The job ends itself after 120 seconds of link silence** while one is begun
  — a Walter that crashed must not leave a peer open. Any valid request on the
  link restarts that count.
- **Only the job's peer is ever decrypted for the client, and only while a
  reply is awaited** (`docs/ARCHITECTURE.md` §2). Outside that window nothing of
  the repeater's own business is disturbed: the four peer overrides are
  upstream's own, untouched.

## 3. When it says nothing

- `linkcli.py` retries exactly as the protocol says: the same frame, the same
  `seq`, 1000 ms apart, three attempts, and then it says *the link is down*.
  The same frame twice is safe by design — the node remembers the last
  `(type, seq)` it executed and sends the remembered response rather than doing
  the work again.
- **Nothing at all** means the wrong pins, the wrong jumpers, RX and TX not
  crossed, or a board that is in its bootloader (`reboot 1` with no way back
  but a power cycle).
- **`dropped a frame (crc)`** on the way to the laptop means the line is noisy:
  check the ground, and that the dongle really is 3.3 V.
- **`power` answering `valid 0`** is the INA3221 not answering, not a flat
  battery: check the ribbon to the B-board and its always-on 3.3 V (the part
  stays powered when the LoRa rail is killed). `env` answering `valid 0` is a
  unit with no BME280 fitted, or one whose A-board rail is down.
- `link.errors` on the USB console — or `link_errors` in `status` — is the same
  count in the other direction, the frames the *node* refused. It should stay
  at 0 on a good link, and it is also readable over the mesh by an admin
  (`sensor get link.errors`), together with `walter.seen`, which is how long
  ago the node last heard a valid request.

NoiseScope, the scanner firmware a repeater can be flashed with instead, speaks
NDJSON text lines on this same UART and not frames. A `hello` that gets no
answer while lines beginning with `{` appear is a scanner, not a broken link
(`spec/LINK-PROTOCOL.md` §9).

## 4. Testing the tool without a board

[`tools/linkloop/`](../tools/linkloop/) is the firmware's own
`src/link/session/` — the same portable code, compiled for the host — with
fixed fakes where the board would be, pumping stdin to stdout.
`tools/linkcli.py --pipe <program>` then speaks the real protocol to the real
session with no port and no radio:

```bash
bash tools/linkloop/check.sh
# or, with no host compiler (Windows):
docker run --rm -v "$PWD:/w" -w /w python:3.11-slim bash tools/linkloop/check.sh
```

That builds the loopback, runs `hello`, `ping`, `status`, `identity`, `time`,
`time set`, `cli`, `power`, `env`, `lpp`, `sleep`, an attention and `reboot`
against it, and times one pass of the link. It is not part of `tools/verify.ps1`: it needs a host
compiler, and what it proves about the session itself is already a native test
(`test/test_link_session`).

Five things in that program are the firmware's own code and not a stub - the
clock keeper, the attention machine, the settings table, the power and
environment readers and the mesh client's job - so `cli "sensor set walter.wake
1"` through the pipe really does raise an attention, `--wake` raises one at
startup so that `listen` shows the whole sequence with nothing but a laptop,
`power`, `env` and `lpp` go through the firmware's own conversion and its own
cache over a fake I2C bus (`--drop-ina` is the "unplug the B-board" case), and
`--job` begins a job at startup so that `path` reads a route back through the
real `Job` (a job is per-boot RAM, so each one-shot `linkcli` invocation is
otherwise a fresh boot with no job).

## 5. Inside the firmware

Three layers, and `docs/ARCHITECTURE.md` §4 is the design:

| | Where | What |
|---|---|---|
| codec | `src/link/codec/` | CRC-16/CCITT-FALSE, COBS, packets, frames, one function per message. Portable. |
| session | `src/link/session/` | the dispatcher, the one-entry retry memory, the event counter, and the interfaces it reaches the firmware through. Portable. |
| attention | `src/link/attention/` | section 8's machine: the queue, the 300 ms gap, the ten repeats, the hourly limit, and the belief that the Walter is asleep. Portable. |
| power | `src/power/` | the INA3221's registers and per-channel shunts, the BME280, the two readers' schedules, the battery thresholds and the Cayenne-LPP. Portable but for `BoardPower.cpp`. |
| mesh client | `src/meshclient/` | the job: one peer, its path in MeshCore's own encoding, what is awaited, the shared secret, the 120-second silence rule — and `PeerSlot`, the index arithmetic of the extra peer. Portable; the peer overrides are in `src/app/TwinOakMesh.cpp`. |
| transport | `src/link/SerialTransport.*` | `Serial1`, and one bounded slice of each `loop()`. |
| the board's half | `src/link/BoardLink.*` | the uptime, the reset reason, the boot identifier, the restart, the identity, upstream's interpreter behind `CLI` — and the one call in `setup()` and one in `loop()`. |

One pass of `loop()` reads what the UART has (bounded), handles **at most one**
request, and writes what fits without blocking. A request that completes while
the previous answer is still unsent waits in the receiver; the answer is never
overwritten and nothing ever blocks. That is what keeps the link from ever
holding up repeating.
