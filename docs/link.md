# The box link, from a laptop

`spec/LINK-PROTOCOL.md` is the protocol — the UART between this firmware on the
RAK4630 and the Walter in the LTE box. The Walter does not exist yet, so this
page is how a person talks to the link instead: a 3.3 V USB-UART dongle and
[`tools/linkcli.py`](../tools/linkcli.py).

What the firmware answers today (phase 1's tasks 03 to 06, and phase 2's
four): `HELLO`, `PING`, `STATUS`, `TIME_SET`, `TIME_GET`, `IDENTITY`,
`REBOOT`, `CLI`, `SLEEP_NOTICE`, `POWER_GET`, `ENV_GET`, `LPP_GET`, the four job
requests `JOB_BEGIN`, `JOB_END`, `PATH_SET` and `PATH_GET`, the five sends
`SEND_LOGIN`, `SEND_REQUEST`, `SEND_CLI`, `SEND_DISCOVERY` and `SEND_TRACE`, the
reply with `REPLY_GET`, `OBS_PULL` and `MESH_COUNTERS`, and `EV_BOOT`,
`EV_ATTENTION` and `EV_MESH_REPLY` as events — which is **every type protocol
1.0 defines** outside its two reserved ranges (provisioning, phase 3's task 03;
the bridge role, later). `HELLO`'s `caps` is `0x7f`, every bit of it, because a
capability is an interface that is there (`src/common/link/session/Interfaces.h`).
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
python3 tools/linkcli.py --port COM7 login -      # log in to the peer (password from a prompt)
python3 tools/linkcli.py --port COM7 status-of-peer   # ask it, and read the reply
python3 tools/linkcli.py --port COM7 send-cli "get name"   # run a CLI line on the peer
python3 tools/linkcli.py --port COM7 discover     # flood a path discovery to it
python3 tools/linkcli.py --port COM7 reply        # the reply the last send is waiting for
python3 tools/linkcli.py --port COM7 counters     # what the radio and the client have sent
python3 tools/linkcli.py --port COM7 job end      # forget the peer
python3 tools/linkcli.py --port COM7 obs          # collect the adverts the node heard
```

`hello` is the first request of a session and may be repeated at any time. It
is how you learn what you are talking to:

```
HELLO
  protocol      1.0
  role          1 (repeater)
  slot          0
  max_payload   240
  caps          0x7f (CLI, POWER, ENV, MESH_CLIENT, OBSERVATIONS, ATTENTION, TIME_PERSIST)
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
- `caps` is `0x7f` — every bit protocol 1.0 defines. A capability is an
  interface that is there, so a build that left one out would simply not
  advertise it and would answer `ERR_UNSUPPORTED` for its requests.

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

### Every request this firmware answers, and the bit that carries it

A capability is an interface that is there (`src/common/link/session/Interfaces.h`), so
this table is `caps` read the other way round: what each bit buys, and what a
build without it would answer. `0x7f` is every bit protocol 1.0 defines, which
is what this role has.

| Request | Capability bit | Where it is walked |
|---|---|---|
| `HELLO`, `PING`, `STATUS`, `IDENTITY`, `REBOOT` | — (always) | §2 |
| `TIME_GET`, `TIME_SET` | `TIME_PERSIST` `0x40` | §2a |
| `CLI` | `CLI` `0x01` | §2b |
| `SLEEP_NOTICE` | `ATTENTION` `0x20` | §2c |
| `POWER_GET` | `POWER` `0x02` | §2d |
| `ENV_GET` | `ENV` `0x04` | §2d |
| `LPP_GET` | — (gated on `CLI` by §4) | §2d |
| `JOB_BEGIN`, `JOB_END`, `PATH_SET`, `PATH_GET` | `MESH_CLIENT` `0x08` | §2e |
| `SEND_LOGIN`, `SEND_REQUEST`, `SEND_CLI`, `SEND_DISCOVERY`, `SEND_TRACE`, `REPLY_GET`, `MESH_COUNTERS` | `MESH_CLIENT` `0x08` | §2f, §2h |
| `OBS_PULL` | `OBSERVATIONS` `0x10` | §2g |

| Event | Capability bit |
|---|---|
| `EV_BOOT` | — (always) |
| `EV_ATTENTION` | `ATTENTION` `0x20` |
| `EV_MESH_REPLY` | `MESH_CLIENT` `0x08` |

A request whose bit is absent is answered **`ERR_UNSUPPORTED`** and nothing
else; a request type this protocol version does not define at all is
`ERR_UNSUPPORTED` too. Nothing in the firmware keeps a second list of what a
build can do, so a `caps` of `0x7f` and this table cannot drift apart.

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
  password back. A test walks `src/common/link/` and fails on any print-like call
  (`test/test_rails`).

### Our settings, from here, from the console and from the mesh

The nine keys of `docs/ARCHITECTURE.md` §4 are on upstream's own `sensor` CLI,
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
| `obs.wake` | `0`, or `1` when a filling observation ring may wake a sleeping Walter | `0` or `1`; **off at boot and per boot** (§2g) |
| `client.refused` | mesh-client sends the RAK's ceiling has refused since boot — `0` on a healthy unit | — (§2h says why it is not settable) |

A `set` of any other key — or of `walter.wake` with any value but `1` — is
answered `can't find custom var`, which is upstream's wording and its only other
word. The two thresholds are refused the same way when the value is not a plain
decimal number, when it is outside 2500–4300 mV, or when `low` would not sit
more than 100 mV above `crit`; a `set` that is accepted is written to a small
file of this firmware's own (`/power`) and survives a restart.

**Nine keys no longer fit one `sensor list`.** Upstream stops a listing once it
has written 134 bytes into a 160-byte reply, so a freshly booted node prints the
first five and `... next:5`, and **`cli "sensor list 5"`** is how the other four
are read; a node whose counters have grown wide prints four and `... next:4`.
`sensor get <key>` always works whatever the paging does, and
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
  asking again (`src/common/power/PowerReader.h`).
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

## 2f. The five sends, the reply and the counters

With a job begun, the link lends the repeater's identity to the five exchanges
an exit node makes with the peer. Each send puts **one packet** on the air (or
none) and is answered at once; the reply comes back later, as an `EV_MESH_REPLY`
event and on `reply`.

```
$ python3 tools/linkcli.py --port COM7 job begin <peer-key>   # a peer, flooded until a path is learnt
$ python3 tools/linkcli.py --port COM7 login -                # the admin password, from a prompt
SEND_LOGIN **********
  send          1 (sent as a flood)
  tag           0x00000000  (the next reply from the peer)
  sender_ts     1791720050 (2026-10-11 ...)
REPLY
  state         3 (response)
  tag           0x6acb7a72
  snr           6.25 dB
  rssi          -97 dBm
  elapsed_ms    1240
  data          0100016a...                                   # the login reply, after its tag
```

- **`login [password]`** is a login as ourselves. With no argument it is the
  **ACL login** — our key and no secret — which a repeater answers at guest
  level if it has no guest password and ignores if it has one; both are right,
  and `reply` says which. With `-` the password is read from a prompt (never the
  command line, and printed as stars); a literal password as the argument works
  too but is seen by the shell. A login's `tag` is **0**: its reply carries the
  *repeater's* timestamp, not ours, so the next response from the peer is the
  one (the agent firmware learnt this on the air).
- **`request <type> [hex]`** is a raw request datagram; **`status-of-peer`**
  (type 1) and **`neighbours-of-peer`** (type 6) are conveniences that send the
  two commonest and leave the reply's bytes for a person to read. A request's
  `tag` is the timestamp the repeater reflects back.
- **`send-cli "<line>"`** runs one CLI line on the **peer**, with the session's
  privileges, and its reply is a text that carries no tag. `--sensitive` tells
  the RAK the line holds a password, so it wipes it after the send — but the RAK
  wipes every send's buffer regardless, so a forgotten flag leaks nothing.
- **`discover`** floods a path-discovery request **whatever path the job holds**;
  its reply comes back as a returned path, which becomes the job's path, and
  `path` then reads the route. **`trace <hashes>`** traces a round-trip path,
  always direct, and comes back with an SNR per hop; a trace cannot carry
  three-byte hashes (`--hash-size 3` is `ERR_RANGE`), only 1, 2, 4 or 8.
- Every send takes **`--timeout-ms`** (how long the RAK awaits the reply; 0
  awaits nothing) and **`--no-wait`** (print the send's own answer and stop).
  While a reply is awaited a second send is **`ERR_BUSY`**; with no job the
  three that need one are **`ERR_STATE`** (a trace needs none). A send that
  cannot be put on the air — a password over 15 bytes, a line over 148 — is
  **`ERR_RANGE`** or **`ERR_MALFORMED`** and nothing is sent. A send over the
  RAK's own ceiling — 6 in a minute, 2 seconds apart — is **`ERR_BUSY`** as
  well, and §2h is how the two are told apart.
- **`reply`** reads the record as it stands (`pending`, `response`, `text`,
  `trace` or `timeout`); the RAK sends one `EV_MESH_REPLY` when the reply
  arrives or the wait times out, and keeps the record until the next send,
  `job begin` or `job end`. A lost event costs one `reply`, never a lost reply.
  While the Walter is believed asleep (`sleep`) the event is held and `reply` is
  the way to it.
- **`counters`** is `MESH_COUNTERS`: `rx` and `tx` from the radio, `client_tx`
  the mesh client's own — **one per send and nothing else**, which is what an
  exit node's transaction count is held against — the noise floor and last
  RSSI/SNR, and the observation ring's three counts (`adverts`, `adverts_drop`
  and `obs_dropped`; §2g).
- **`STATUS`'s flag bit 1** (`reply awaited`) is on from a send with a timeout
  until its reply or its deadline.

## 2g. Observations: the adverts the node heard

Every advert this repeater hears — once its signature has verified — is kept
for the Walter, with the path it arrived by and how well it was heard. That is
the free half of what an exit node tells MeshMaster: who is out there, and how
they are reached from here. The ring holds **64** records and nothing is ever
pushed: the Walter collects them when it is awake.

```
$ python3 tools/linkcli.py --port COM7 obs
OBS_PULL
  count         5
  remaining     12
  dropped       0  (lost to a full ring since the last pull)
    key        hops     snr     rssi      age
    a1b2c3d4   0       6.00  -84 dBm      41s
    7f3a9c02   2      -2.50  unknown     300s  via a1b27f3a (2-byte)
    ...
```

- **`obs [max]`** asks for at most `max` records and gets as many as fit **one
  payload** — one to five of them, depending on how long each record's path and
  application data are. **The records it returns leave the ring**, so this is a
  collection and not a view: `remaining` says how many are still there, and a
  second `obs` fetches the next ones. A retry of the same request (§3) gets the
  *same* records back and not the next ones, because the node remembers the
  response it gave.
- `hops` **0 with an RSSI** is a direct neighbour, which is the thing to look
  for first. A record with hops carries the path the advert arrived by and the
  hash size it was flooded at.
- `age` is **measured** on the node's millisecond clock and not computed from
  its epoch, so it is right whatever the clock has been told since; `adv_ts` in
  the record is the *advertiser's* own timestamp.
- `rssi` reads `unknown` for an advert whose RSSI could not be kept: the node
  notes it while the packet is still the one the radio just received, and a
  flood held back behind more than 32 others loses the note. The SNR is the
  packet's own and is always there.
- **`--names`** decodes each advert's name, role and position from its
  application data **on this laptop**. The node copies those bytes and never
  looks inside them: the name is something a stranger chose, and neither end of
  this link logs it (`spec/LINK-PROTOCOL.md` §7).
- `status` carries `obs_pending` (waiting in the ring) and `obs_dropped` (lost
  to a full ring since boot); `counters` carries `adverts` (adverts that
  verified), `adverts_drop` and the same `obs_dropped`. A ring that is full and
  losing records is a node nobody has collected from and not a fault — the
  repeater is none the worse for it.
- **`sensor set obs.wake 1`**, from the console or from a mesh admin, lets a
  ring at three quarters full ask for a sleeping Walter (`EV_ATTENTION` reason
  5, with the records waiting as its `detail`). It is **off by default**,
  because a unit on a busy mesh would otherwise wake its box every hour for
  adverts alone and each wake costs an LTE attach; the default is "collect them
  when the Walter is awake anyway". It is a per-boot setting, like the clock: a
  Walter that wants it says so after each `EV_BOOT`.

## 2h. The two ceilings

Two things bound what this unit puts on the air, and they are not the same
thing.

| | what it counts | the limit | what happens over it |
|---|---|---|---|
| **the client's ceiling**, ours (`src/roles/repeater/meshclient/SendCeiling.h`) | the sends the Walter asks for, one per `SEND_*` — the same count as `counters`' `client_tx` | **6 in any 60 s**, and **2 s** between two | the send is **`ERR_BUSY`** and nothing leaves; `sensor get client.refused` counts it |
| **the duty cycle**, upstream's (`mesh::Dispatcher`) | *every* packet the radio transmits, the hundreds it forwards included | an airtime budget of a one-hour window — **half** of it by default, and `cli "set dutycycle <1..100>"` changes it | the radio waits; nothing is refused and nothing is lost |

The first is a backstop on a mistake in the Walter. Its agent paces itself to a
budget the MeshMaster server sets — `budget 2/min gap 20 s` by default — so a
healthy unit never comes near 6 a minute; a Walter in a loop, or one being
written, does, and the RAK refuses rather than becoming a nuisance on a mesh
other people use. The limits are build constants, because a limit the Walter
could raise would be no backstop against the Walter.

- **An `ERR_BUSY` has two causes and `status` tells them apart**: flag bit 1
  (`reply awaited`) is set when the refusal was "one exchange at a time", and
  clear when it was the ceiling. `counters`' `client_tx` does not move either
  way — a refused send is not a send.
- **A refusal costs nothing.** It does not enter the 60-second window and does
  not restart the 2-second gap, so hammering the link does not push the next
  legitimate send further away.
- **`sensor get client.refused`** reads the count, from the USB console or from
  a mesh admin's phone — deliberately not over the link, because a Walter that
  is being refused is the one party whose own counters cannot be trusted about
  it.

The second is the band's, and only upstream can hold it: it is the one
accounting that sees forwarded traffic. Our sends go through the same outbound
queue and are decremented from the same budget, so the two do not need to agree
about anything (`docs/ARCHITECTURE.md` §7 reads the lines). Six of the longest
packets this client can build is about 13 seconds of transmission a minute,
which is some 22 % against the default 50 %; the exchange a server actually
asks for is nearer 2 %. On a unit somebody has set to a **low** duty cycle, a
run-away Walter could meet upstream's budget as well as the ceiling — at which
point upstream delays the next transmission and nothing is lost.

**Repeating comes first, and is not slowed by this.** A packet of ours is
queued at the same priority as a forward of routed traffic and ties go to
whichever was queued first, so a send can delay a packet being forwarded by its
own airtime and no more — about 1.7 s for the largest, and six times a minute
at the very most. §7 of `docs/ARCHITECTURE.md` has the priorities and the lines
they come from.

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
`src/common/link/session/` — the same portable code, compiled for the host — with
fixed fakes where the board would be, pumping stdin to stdout.
`tools/linkcli.py --pipe <program>` then speaks the real protocol to the real
session with no port and no radio:

```bash
bash tools/linkloop/check.sh
# or, with no host compiler (Windows):
docker run --rm -v "$PWD:/w" -w /w python:3.11-slim bash tools/linkloop/check.sh
```

That builds the loopback, runs `hello`, `ping`, `status`, `identity`, `time`,
`time set`, `cli`, `power`, `env`, `lpp`, the job, the five sends, `obs`,
`sleep`, an attention and `reboot` against it, and times one pass of the link.
It is not part of `tools/verify.ps1`: it needs a host compiler, and what it
proves about the session itself is already a native test
(`test/test_link_session`).

Six things in that program are the firmware's own code and not a stub - the
clock keeper, the attention machine, the settings table, the power and
environment readers, the mesh client's job and the observation ring - so `cli
"sensor set walter.wake 1"` through the pipe really does raise an attention,
`--wake` raises one at startup so that `listen` shows the whole sequence with
nothing but a laptop, `power`, `env` and `lpp` go through the firmware's own
conversion and its own cache over a fake I2C bus (`--drop-ina` is the "unplug
the B-board" case), `--job` begins a job at startup so that `path` reads a route
back through the real `Job`, and `--adverts <n>` hears that many synthetic
adverts so that `obs` empties the firmware's own ring with its own payload
fitting (a job and a ring are both per-boot RAM, so each one-shot `linkcli`
invocation is otherwise a fresh boot with neither). `--obs-wake` turns the
`obs.wake` setting on.

## 5. Inside the firmware

Three layers, and `docs/ARCHITECTURE.md` §4 is the design:

| | Where | What |
|---|---|---|
| codec | `src/common/link/codec/` | CRC-16/CCITT-FALSE, COBS, packets, frames, one function per message. Portable. |
| session | `src/common/link/session/` | the dispatcher, the one-entry retry memory, the event counter, and the interfaces it reaches the firmware through. Portable. |
| attention | `src/common/link/attention/` | section 8's machine: the queue, the 300 ms gap, the ten repeats, the hourly limit, and the belief that the Walter is asleep. Portable. |
| power | `src/common/power/` | the INA3221's registers and per-channel shunts, the BME280, the two readers' schedules, the battery thresholds and the Cayenne-LPP. Portable but for `BoardPower.cpp`. |
| mesh client | `src/roles/repeater/meshclient/` | the job: one peer, its path in MeshCore's own encoding, what is awaited, the shared secret, the 120-second silence rule; the five sends behind `MeshPort`; the reply record and its matching rules; `PeerSlot`, the index arithmetic of the extra peer; the observation ring, its pull and the RSSI note table. Portable; the peer overrides, the packet building and the advert capture are in `src/roles/repeater/app/TwinOakMesh.cpp`. |
| transport | `src/common/link/SerialTransport.*` | `Serial1`, and one bounded slice of each `loop()`. |
| the board's half | `src/roles/repeater/link/BoardLink.*` | the uptime, the reset reason, the boot identifier, the restart, the identity, upstream's interpreter behind `CLI` — and the one call in `setup()` and one in `loop()`. |

One pass of `loop()` reads what the UART has (bounded), handles **at most one**
request, and writes what fits without blocking. A request that completes while
the previous answer is still unsent waits in the receiver; the answer is never
overwritten and nothing ever blocks. That is what keeps the link from ever
holding up repeating.
