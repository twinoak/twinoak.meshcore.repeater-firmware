# How this firmware is put together

The design the task piles build. It was written from MeshCore's source at the pin
(`d92964352441e53b93e8667b802e04f6e072b39e`, `repeater-v1.17.1`) and from the agent firmware's,
before a line of this repository compiled. Where it says *upstream does X*, a file and a function
are named so that the claim can be checked; phase 1's first task checks the ones everything rests
on, and this file is corrected by whichever task finds it wrong.

## 1. Layout

```
platformio.ini          the pin, the libraries, one environment per role and board, [env:native]
boards/                 board definitions PlatformIO must see before it clones a dependency
src/
  main.cpp              ours: setup() and loop(), replacing upstream's example main
  app/                  TwinOakMesh (the subclass), first-boot provisioning, the version string,
                        Settings (our keys on upstream's CLI, portable)
  board/                target.h / target.cpp for our adapter: the board object, the radio, the clock, the sensors;
                        UpstreamHeaders.h, the one place upstream's headers are included from (section 11)
  link/                 the box link: codec (portable), session and dispatcher (portable),
                        attention (the machine of section 8, portable), UART transport
  clock/                ClockKeeper (the rules, portable), BoardClock (the board's ports and
                        its flash file), LinkClock (the keeper as the link's ClockAccess)
  power/                INA3221 with per-channel shunts, BME280, the two readers' schedules,
                        the battery thresholds and the Cayenne-LPP (all portable but BoardPower)
  meshclient/           the job, the five sends, the reply, the observation ring (state portable)
  fwdesc/               the image descriptor
test/                   native tests, one directory per subject
spec/                   a copy of the system specification and its fixtures - never edited here
keys/                   release.pub, the public half of the release key - and nothing secret, ever
tools/                  verify.ps1, release/ (the release tooling and its tests), pio/ (build scripts)
docs/                   this file, release.md, upstream.md, link.md, bench.md
tasks/                  the piles
```

"Portable" means: no Arduino include, no MeshCore include, compiled by `[env:native]` beside its
tests. The rule of thumb is the agent firmware's — if it can be a pure function or a class behind
an interface, it is, and the file that touches the board is too thin to hide a bug in.

## 2. Upstream's repeater, and ours on top of it

MeshCore is a PlatformIO dependency. Its `library.json` runs `build_as_lib.py`, which reads a few
defines to decide which of its own sources to compile; `-D BUILD_EXAMPLE=simple_repeater` compiles
`examples/simple_repeater/*.cpp`, and `-D EXCLUDE_FROM_EXAMPLE=main.cpp` leaves one of them out.
That is the whole mechanism: **upstream's `MyMesh.cpp` is compiled from the pin, byte for byte, and
only its `main.cpp` is replaced by ours.** Two details of it, from the script at the pin: the
defines are read in order and `EXCLUDE_FROM_EXAMPLE` must come after `BUILD_EXAMPLE` (the script
indexes the example's name when it sees the exclude); and `UITask.cpp`, the example's display task,
is still compiled - a second `EXCLUDE_FROM_EXAMPLE` would be a macro redefinition - but without
`DISPLAY_CLASS` nothing references it and the linker leaves it out (section 11).

`MyMesh` (`examples/simple_repeater/MyMesh.h`) is `class MyMesh : public mesh::Mesh, public
CommonCLICallbacks`. Most of its state is private — the ACL, the preferences, the neighbour table.
We need none of it, because what we add hangs on three kinds of thing that are *not* private:

| We need to | Upstream gives us | Kind |
|---|---|---|
| run a console command with console privileges | `MyMesh::handleCommand(0, command, reply)` | public |
| read and change preferences at first boot | `getNodePrefs()`, `savePrefs()` | public |
| add our own settings to the CLI, over the mesh too | `sensor get/set/list` → `SensorManager::getSettingName/Value`, `setSettingValue` (`CommonCLI.cpp`) | a virtual seam |
| put our sensors in a telemetry reply | `sensors.querySensors(perm, lpp)`, called by `MyMesh::handleRequest` | a virtual seam |
| report the battery from the INA3221, not an ADC pin that is not wired | `board.getBattMilliVolts()` | virtual on the board |
| see every advert | `onAdvertRecv` | protected virtual |
| know a packet's RSSI while it is still that packet's | `logRx` | protected virtual |
| count what we transmit | `logTx` | protected virtual |
| be a client of one peer | `searchPeersByHash`, `getPeerSharedSecret`, `onPeerDataRecv`, `onPeerPathRecv`, `onTraceRecv` | protected virtual |
| know when a packet of ours carried our clock (§5) | `sendSelfAdvertisement` (public, `CommonCLICallbacks`), `onAnonDataRecv` (protected) | virtual |
| write the clock's file before a restart (§5) | `MainBoard::reboot()`, `MainBoard::powerOff()` | virtual on the board |
| originate a packet | `createAnonDatagram`, `createDatagram`, `createTrace`, `sendDirect`, `sendFlood` | public on `Mesh` |

`TwinOakMesh : public MyMesh` overrides those virtuals, calls the base for everything that is
not ours, and adds the link. `main.cpp` constructs it where upstream's constructs `MyMesh`.
Nothing of upstream's names `the_mesh` but its own `main.cpp`, so the type of ours is nobody's
business but ours. One of them, `onAdvertRecv`, is declared in `MyMesh.h` without the `override`
keyword; it is virtual all the same, through `mesh::Mesh`, and ours says `override`.

### The one piece of cleverness: an extra peer in front

`Mesh::onRecvPacket` (`src/Mesh.cpp`) handles a packet addressed to us from a source hash by
asking `searchPeersByHash(hash)` how many known peers match, then, for each index `j`, taking
`getPeerSharedSecret(secret, j)`, trying the MAC, and on the first that verifies calling
`onPeerDataRecv(pkt, type, j, …)` (or `onPeerPathRecv`). Upstream's `MyMesh` answers from its ACL
and keeps the matches in a private array indexed by `j`.

The mesh client has one peer of its own while a job is waiting for a reply. `TwinOakMesh` puts it
**in front**:

```
searchPeersByHash(hash):
    n = MyMesh::searchPeersByHash(hash)          // fills upstream's private match array for 0..n-1
    extra = awaiting a reply  AND  the job's peer matches hash
    return n + (extra ? 1 : 0)

getPeerSharedSecret(dest, j):   extra ? (j == 0 ? ours : MyMesh::…(dest, j - 1)) : MyMesh::…(dest, j)
onPeerDataRecv(…, j, …):        the same shift;  j == 0 with extra  →  the mesh client takes it
onPeerPathRecv(…, j, …):        the same shift
```

In front, not behind, because of the one case where it matters: if the job's peer is also an admin
in our own ACL, both entries have the same shared secret and the first one tried wins. Behind, a
CLI *reply* from that peer would be run by upstream as a CLI *command*. In front, it is read as the
reply it is. The price is that a request from that same peer during the seconds we are awaiting
its reply is dropped — which is the price a single-peer client pays anyway.

Outside "awaiting a reply", `extra` is false and every call is upstream's own, untouched. A stock
repeater does not receive `RESPONSE` or `TRACE` packets for itself, so nothing upstream does is
shadowed.

`onTraceRecv` is not overridden by upstream; ours takes a completed trace whose tag is the awaited
one.

### What breaks this, and how it is noticed

A pin bump that makes one of those virtuals private or non-virtual, renames the example, or changes
`build_as_lib.py`'s defines. Each is a compile error or a missing symbol, in CI, on the bump's own
pull request (`docs/upstream.md`). None can fail silently — except a change in the *order*
`Mesh::onRecvPacket` tries peers in, which is why that function is cited, with the pin, in the
comment above the override, and listed among the places a bump re-reads.

## 3. The board target

`src/board/target.{h,cpp}` is ours: MeshCore's `variants/rak4631/target.cpp` assumes a WisBlock
base — a GPS on `Serial1`, a display, a battery divider on an ADC pin. On our adapter `Serial1` is
the link, there is no display or GPS, and the battery is read by the INA3221.

- `board` — `TwinOakBoard`, a subclass of upstream's `RAK4631Board` (the SX1262 wiring inside the
  RAK4630 stamp is the stamp's, so the radio pins and the DC/DC setup are upstream's unchanged),
  with `getBattMilliVolts()` returning **INA3221 channel 2's bus voltage** out of the reader's
  cache, or 0 when that channel is not valid (§6). Upstream reads `PIN_VBAT_READ` (P0.05), which
  is not wired on this adapter. `NRF52Board` is a *virtual* base of `RAK4631Board` (through
  `NRF52BoardDCDC`), so the most-derived class is the one that constructs it, with the OTA name
  upstream passes; a board added in phase 3 does the same.
- `radio_driver`, `radio_init()`, `radio_new_identity()` — as upstream's.
- `rtc_clock` — upstream's `AutoDiscoverRTCClock` over a volatile clock, so a hand-fitted RV-3028
  is found by itself; the clock keeper (§5) sits above it.
- `sensors` — `TwinOakSensors : EnvironmentSensorManager`. **Both** of upstream's sensor drivers
  this unit would have used are left out of the build, and `test/test_rails` holds them out:
  `ENV_INCLUDE_INA3221` because it applies one shunt value to all three channels and ours are
  100 mΩ, 20 mΩ and 50 mΩ; `ENV_INCLUDE_BME280` because it samples the part *inside*
  `querySensors` (`MODE_FORCED`, then `takeForcedMeasurement()` spins on the status register) and
  `querySensors` is on the path of the box link's `LPP_GET`, which must never perform I2C. Ours
  reads both parts itself, on a schedule, into one cache (§6). The class adds three things and
  substitutes none: `begin()`/`loop()`, where those readers are driven — the manager is what
  `src/main.cpp` already calls once in `setup()` and once in `loop()`; `querySensors()`, which
  appends this unit's own telemetry after whatever upstream's manager adds (nothing, on this
  board); and the settings seam (§4), where our keys follow upstream's own (none here, since
  there is no GPS) in `sensor list`, `sensor get` and `sensor set`.

Upstream's variant sources we still need (`RAK4631Board.cpp`, the Arduino `variant.cpp`) are
compiled from the pin by a small PlatformIO script (`tools/pio/meshcore_variant.py`), the way the
agent firmware compiles `lib/ed25519` — not copied. `variant.cpp` is not a duplicate of the core's:
the Adafruit core at the pin has **no** `WisCore_RAK4631_Board` variant directory (PlatformIO
builds an empty "variant library" from the missing path without complaint), so the pin map comes
from MeshCore's tree in upstream's build and in ours. `boards/rak4631.json` is the one file that
has to be vendored, for the reason `boards/README.md` gives: PlatformIO reads `boards_dir` before
it clones a dependency. And `MC_VARIANT` is *not* set: with it, `build_as_lib.py` compiles all of
`variants/rak4631/` — upstream's `target.cpp` included — and the script refuses to run.

Upstream's `MyMesh.h` includes `<target.h>` unqualified, so ours replaces theirs by include order:
`src/board` is first on the path, `variants/rak4631` (still needed for `variant.h`'s LoRa pins and
`RAK4631Board.h`) after it. The MeshCore library's own files that include `target.h` —
`MyMesh.cpp`, `UITask.cpp`, `helpers/NRF52Board.cpp` — get ours too, which is why its globals keep
upstream's names and base types.

I2C runs at 100 kHz or below: one set of 1 k pull-ups, through a 1000 pF bulkhead filter
(`INTERCONNECT.md`).

## 4. The link, inside the firmware

`spec/LINK-PROTOCOL.md` is the protocol. In the firmware it is three layers:

1. **Codec** (portable, `src/link/codec/`, proved by p1-02): `Crc16`, `Cobs`, `Packet`,
   `FrameReceiver` (which counts what it drops) and `FrameWriter` are the framing; `Numbers.h` is
   every type, status, mode and limit of the protocol in one header; `Bytes.h`'s `Reader` and
   `Writer` are the little-endian scalars; `Messages.{h,cpp}` is a plain struct per payload with
   the one direction this side of the link needs — **a decoder for every request, an encoder for
   every response and every event**, because in 1.0 only the Walter sends requests and only the
   RAK sends events, and the other half of each pair is the Walter's firmware's. Tested against
   every case of `spec/fixtures/link/link-v1.json` (`test/test_link_codec`), with a test that
   fails when the fixture gains a case nothing exercises.
2. **Session** (portable, `src/link/session/`, proved by p1-03): a receiver that turns bytes into
   packets and counts what it drops; a dispatcher from request type to handler; the one-entry
   memory of the last request and its response, which is what makes a retried request harmless;
   the event counter. It talks to the rest of the firmware through the interfaces of
   `Interfaces.h` — the system, the identity, and one each for the clock, the console,
   attention, power, the environment, the mesh client and the observation ring — so that the
   native tests run it whole against fakes (`test/test_link_session`). **A capability is an
   interface that is there**: `HELLO`'s `caps` is built from which of those pointers are non-null
   and from nothing else, so a firmware that has not implemented a thing does not advertise it and
   answers `ERR_UNSUPPORTED` for it. Nothing keeps a second list of what a build can do.
3. **Transport** (`src/link/SerialTransport.*`): `Serial1` at 115200. Read what is there, write
   what fits, never wait. `src/link/BoardLink.*` is the board's half of those interfaces —
   uptime, the reset reason, the per-boot identifier from the radio's RNG, the restart, the
   identity — and the one call `src/main.cpp` makes in each of `setup()` and `loop()`.

**The link never stalls the mesh.** `loop()` gives it a bounded slice: it drains the UART's receive
buffer, handles at most one request, and queues its answer. A handler that would take time — a
mesh exchange — returns at once and reports later (the protocol is built that way). A handler that
runs a console command inherits that command's cost, which is upstream's (a `set` writes
preferences to flash); that is the worst case and it is the same as typing the command on USB.
A request that completes while the previous answer is still unsent waits in the receiver — the
transport stops reading and the UART's own buffer holds the rest — so neither is lost and
nothing blocks. `Serial1`'s receive ring and transmit buffer are raised to 256 bytes from the
core's 64 (`platformio.ini`), which is what makes a 246-byte frame fit in one read and leave in
one write that cannot block; p1-03's report has the measurement of what one pass costs.

**`CLI` is upstream's own interpreter** (§5.7, p1-05). The handler hands
`the_mesh.handleCommand(0, line, reply)` a copy of the line — timestamp 0 is what makes it a
command typed on the console, and the copy is because the interpreter writes into it — and sends
back what it wrote. The reply window is the 160 bytes upstream's own console loop gives it, with
64 bytes of guard behind it that the session checks after every line, because upstream's
`sensor list` can write past 160 on a console that has nothing behind it at all. Four console
commands never return (`reboot`, `clkreboot`, `poweroff`, `shutdown`); the response to those is
not owed and the handler does not try to out-run the restart. **Nothing under `src/link/` prints**,
and a rails test holds that: a `CLI` line can be `password <new>`, whose reply echoes it back.

**Attention** (`src/link/attention/`, §8) is one small portable machine: one reason pending with
a queue behind it, the two `0x00` wake bytes, 300 ms, then `EV_ATTENTION` every 1000 ms with
`repeat` counting from 0 and at most ten sends, ended by any valid request, and at most one new
attention per reason per hour. It also holds the belief that the Walter is asleep
(`SLEEP_NOTICE`): while that is set the session sends no events but `EV_BOOT` and `EV_ATTENTION`.
The session asks it once a pass what to put on the wire and tells it when the bytes have gone, so
every rule about *when* is in one file with a fake clock in front of it.

Eight more ways in, through `TwinOakSensors`' settings — the keys and what each one does are
`src/app/Settings.{h,cpp}`, portable and tested; the board's half is a dozen one-line readers and
writers. They work from the USB console and from a mesh admin's `sensor get/set`, in this order,
which is the order `sensor list` prints:

| Key | `get` | `set` |
|---|---|---|
| `fw.version` | `twinoak-repeater <version> on MeshCore <tag> (<pin8>)` | — |
| `walter.wake` | `0`, or `1` while an attention is pending | `1` raises attention reason 4 ("a person asked for the Walter") |
| `link.errors` | frames dropped by the receiver since boot | — |
| `walter.seen` | seconds since the last valid request on the link, or `never` | — |
| `clock.state` | `unset`, `restored`, `set` or `rtc` | — |
| `power.low_mv` | the *battery low* threshold in millivolts | a decimal number of millivolts (§6) |
| `power.crit_mv` | the *battery critical* threshold | the same |
| `obs.wake` | `0` or `1`: whether a filling observation ring may ask for a sleeping Walter | `0` or `1`; off at boot and per boot (§7) |

Eight keys no longer fit one `sensor list` reply: upstream stops a listing at 134 bytes of a
160-byte buffer, so a fresh node prints the first five and `... next:5` and `sensor list 5` is
the rest. The order is task 05's five with the later tasks' appended, which is the only place a
new key can go without renumbering one a bench instruction or a document already names.

`walter.wake` is the out-of-band way to reach a unit whose Walter is asleep: any exit node that
reaches the repeater can ask it to wake its own uplink. Upstream's permission check is the whole
gate — a text message is run through the interpreter only for a client whose ACL entry is
`PERM_ACL_ADMIN` (`MyMesh::onPeerDataRecv`, `examples/simple_repeater/MyMesh.cpp:702` at the
pin) — and `sensor set` carries no `sender_timestamp == 0` condition, so an admin over the mesh
may run it and nobody else may.

## 5. The clock keeper

Batch-1 boards have no RTC - the RV-3028 and its backup capacitors are DNP for a worldwide
allocation (the hardware repository's `PROJECT-KNOWLEDGE.md` S1). Upstream's volatile clock starts
every boot at a fixed date in 2024 (`VolatileRTCClock`, `src/helpers/ArduinoHelpers.h`:
`base_time = 1715770351`), and a node whose clock runs backwards has its adverts and replies taken
for replays by everyone who heard the later ones.

`src/clock/` is three files: `ClockKeeper.{h,cpp}` is every rule, portable and tested natively
against fakes; `BoardClock.{h,cpp}` is the only one that knows what a RAK4630 is; `LinkClock.h` is
twenty lines that make the keeper the box link's `ClockAccess` - and the one place that holds the
keeper's four states against the wire's `time_state` with a `static_assert`.

The keeper reaches the board through four ports, and a null port is a memory this board does not
have: **upstream's clock** (`rtc_clock`, which is what the mesh stamps packets from), **a hardware
RTC**, **RAM that survives a reset**, and **a file**.

- **Two memories, and only one of them exists today.** The file is `/clock`, ten bytes - a magic,
  the time, one flag and a checksum - rewritten when what it holds is more than the save interval
  (an hour) behind the clock, and unconditionally on a restart the firmware itself asks for, where
  it is marked *clean*. The surviving-RAM memory is **not implemented**: no place in this board's
  RAM can be shown to survive, because the core's startup copies `__data_start__` to
  `__bss_start__` out of flash and the C library zeroes `__bss_start__` to `__bss_end__`, which
  leaves only the gap above `__bss_end__` - and that is inside the RAM the Adafruit bootloader uses
  for itself, and the bootloader runs before us on every reset (`tasks/phase-1/reports/04.md` has
  the reading, with the linker script and the bootloader image's own numbers). The port and its
  rules stay, tested, for the day a place is found or an RTC is fitted.
- **At boot**, in order: a hardware RTC whose answer is a sane time wins (`time_state` *rtc*, and
  nothing is written to it); otherwise the surviving RAM value as it stands (*restored*); otherwise
  the file's value **plus the save interval** (*restored*) - the node cannot know how long it was
  down, and being an hour ahead is harmless where being a second behind its own last advert is not.
  The one exception is a file marked *clean* read after a software reset: that value is the second
  the restart happened in, so it needs no margin, and the flag is spent at once so that a later
  crash cannot reuse it. Otherwise upstream's default, which is never right (*unset*).
- **Sane** means 2026-01-01 or later and before 2100: this firmware did not exist before that, so
  an earlier answer is an unset RTC, a part that lost its backup, or a mistake. It is also what
  lets the board tell "an RTC answered" from "upstream's volatile clock answered" when they are the
  same object, which `AutoDiscoverRTCClock` makes them (it keeps no flag a caller can read).
- **`TIME_SET`** (*set*): forward always; backwards only with *force*, or while the state is
  *unset* or *restored* and nothing carrying a timestamp has left this node since it booted - which
  the advert hold makes the normal case. An epoch outside the sane window is `ERR_RANGE` whatever
  *force* says. A set saves both memories, and a forced set that moves the clock *down* brings the
  saved bound down with it.
- **The console keeps working.** Upstream's own `time <epoch>` and `clock sync` set `rtc_clock`
  directly and refuse to go backwards themselves; the keeper **polls** for a forward jump it did
  not make and moves to *set* when it sees one, so not a line of upstream's CLI is patched. A
  backwards jump - only `clkreboot` can make one - is deliberately not followed: the memories are a
  high-water mark.
- **The boot advert waits.** Our `main.cpp` sends it, so ours decides when: as soon as the state is
  *set* or *rtc*, or 60 seconds after boot, and `STATUS` flag bit 3 says so while the hold is on.
  Upstream's *periodic* adverts stay upstream's - the first of them cannot fire for two minutes.
- **What counts as a timestamp leaving the node** is four virtuals of `TwinOakMesh`:
  `sendSelfAdvertisement` (ours, and the console's `advert`), and `onAnonDataRecv`,
  `onPeerDataRecv`, `onPeerPathRecv` - a packet addressed to us that verified, which upstream
  answers with a reply carrying our clock.
- **Every restart the firmware performs writes the file first**: `TwinOakBoard::reboot()` and
  `powerOff()` (which is what the console's `reboot`, `clkreboot` and `poweroff` reach) and the box
  link's own restart, which also covers the three bootloader modes that never touch the board
  object.

## 6. Power and environment

The INA3221 at `0x42` is read directly: shunt voltage and bus voltage per channel, 16-bit
registers, the shunt of each channel applied to its own reading (the arithmetic is the hardware
repository's `CURRENT-SENSE.md` and NoiseScope's `telemetry.cpp`, and is a portable function with
tests). The BME280 at `0x76` is read beside it, in normal mode so that a sample is three register
reads and no waiting. **Both go into one cache**, and the four readers of that cache are the
link's `POWER_GET` and `ENV_GET`; the mesh telemetry reply and the link's `LPP_GET`; the board's
battery voltage (§3); and the battery thresholds.

Nothing reads a bus on demand: the only two calls into I2C are `PowerReader::loop()` and
`EnvReader::loop()`, both driven from `TwinOakSensors::loop()` once a pass (§3). That is what
makes "a request on the box link never performs I2C" a property of the shape rather than of any
one handler — and it is why upstream's own BME280 support had to leave the build.

A sensor that stops answering is a missing reading, never a stalled loop: a channel that failed
loses its validity bit and reads 0, a transaction that goes past its budget puts the whole device
in a five-second back-off, and the sweep is abandoned where it stands. The one thing this layer
cannot do is bound an I2C transaction: the Adafruit core's TWIM driver busy-waits on the
peripheral's event registers with no deadline, and the ERROR event an absent device raises is what
makes the realistic failure fast. `tasks/phase-1/reports/06.md` records that residual risk.

The battery thresholds are *low* and *critical* on channel 2's bus voltage, with 100 mV of
hysteresis and a minimum time below of one minute, so that a transmit burst's sag raises nothing;
they are `power.low_mv` and `power.crit_mv` (§4), kept across a restart in a small versioned file
of our own (`/power`, not upstream's `/prefs.json`). The hourly limit is not here: every attention
goes through §4's machine, which already allows at most one new attention per reason per hour,
because each one costs the battery an LTE attach.

The Cayenne-LPP is **one field list and two appliers**: `TwinOakSensors::querySensors` walks it and
calls upstream's `CayenneLPP` for each field, because a mesh telemetry reply is upstream's buffer
and upstream's library; `LPP_GET` walks the same list with the two fields upstream's
`handleRequest` adds around it, and encodes it with a portable mirror of `CayenneLPP::addField`.
The channel numbers are chosen once and written in `docs/link.md`; they are never renumbered.

## 7. The mesh client

A port of `src/mesh/AgentMesh.{h,cpp}` of twinoak/meshmaster-agent-firmware onto `TwinOakMesh`,
with the link's messages where that file has method calls and the reply delivered as an event
where that file blocks in `await()`.

- **State** (portable, tested natively): the job (peer, path), what is awaited and until when, the
  reply record, tag matching, the path learned from a returned path.
- **Sends** (board): build one packet with MeshCore's own primitives, send it direct along the
  job's path or as a flood with two-byte hashes, wipe the plaintext if it held a password, count
  it. Refuse a second send while a reply is awaited.
- **Replies**: through the extra peer of §2, and only while awaiting.
- **Observations** (portable, tested natively): every advert that verified goes to upstream
  first (its neighbour table), then into a ring of **64** records of `LINK-PROTOCOL.md` §7 that
  the Walter empties with `OBS_PULL` — the records a pull hands out leave it, the oldest is
  overwritten when it is full and the loss is counted. The advert's application data is **copied
  and never parsed**: the name inside it is a stranger's, and the agent on the Walter is where it
  is decoded. RSSI is noted in `logRx`, when it still belongs to the packet — a flood can wait up
  to 32 seconds in the inbound queue before `onRecvPacket` sees it — and the same note is what
  fills a reply's `rssi`. `obs.wake`, off by default, is what lets a ring at three quarters full
  ask for a sleeping Walter (§8's reason 5).

What it deliberately is not: a chat client. No contacts, no channels, no acknowledgements, no
reciprocal path returns. One send is one packet.

## 8. The family: roles, boards, bands

An environment is `<role>_<board>` and builds one image:

| Role | What | When |
|---|---|---|
| `repeater` | everything above | phases 0–2 |
| `scanner` | NoiseScope: the SX1262 spectrum scanner, RX only, moved here from the hardware repository | phase 3 |
| `bridge` | two radios of one unit carrying packets between two bands over a UART of their own | later |

| Board | What | When |
|---|---|---|
| `rak4630_single` | the single-RAK adapter that was ordered | phases 0–2 |
| `rak4630_dual_a`, `_b` | the two positions of the dual adapter; `a` owns the I2C bus and the link's telemetry | later |

The band is a build constant (`868` today), in the descriptor and the manifest, because a 433 MHz
module is a different part and must never be given an 868 image.

Every image carries the descriptor of `spec/FIRMWARE-MANIFEST.md` §4, and the same strings come
back in the link's `HELLO`. A role that cannot do something says so in its capability bits rather
than failing a request; the bridge will have its own messages in the reserved range. The scanner
is the exception that proves the rule: NoiseScope 0.1 speaks text lines on the same UART, not
frames, and the Walter tells the two apart as `spec/LINK-PROTOCOL.md` §9 says.

For the bridge, the second UART (`Serial2`, P0.19/P0.20) is reserved to connect the two radios of
a dual adapter. `Serial1` stays the link to the Walter on every board.

## 9. Flash, and what an update may touch

```
0x00000000  MBR and SoftDevice           never written by an update
0x00026000  the application              ◄── an image lives here and nowhere else
   …        (end of the application region: from the linker script; recorded in the manifest)
   …        the file system: identity, preferences, the ACL, regions, the clock file
0x000F4000  the bootloader, its settings
            UICR
```

The Walter flashes the application region over SWD: it erases exactly the pages the new image
covers and writes them. It never erases the chip, the file system or UICR. A firmware change that
moves the end of the application region is a change to the manifest's `app_region` and is checked
by the release tooling against the linker script (`spec/FIRMWARE-MANIFEST.md` §2).

## 10. What is tested where

| | Native tests | Build only | Bench |
|---|---|---|---|
| link codec, session, retry memory | ✔ against the fixtures and fakes | | UART through the bulkheads |
| the console over the link, our settings, the attention machine | ✔ against fakes, and the `CLI`, `SLEEP_NOTICE` and `EV_ATTENTION` fixtures through the real session | upstream's interpreter behind it | every console command, and a mesh admin waking the box |
| clock rules, the boot order, the file's rate | ✔ against fakes, and the `TIME_SET` fixture through the real session | the flash file, the RTC probe | a reboot keeps time moving forward |
| shunt arithmetic, the two readers' schedules, the thresholds, the Cayenne-LPP | ✔ against a fake bus and a fake part, and the `POWER_GET`, `ENV_GET` and `LPP_GET` fixtures through the real session | the I2C transactions, the Adafruit BME280 driver, the preferences file | real currents, sign, calibration; a phone's telemetry view |
| job state, reply matching | ✔ | the overrides, the sends | a login to a real repeater |
| the observation ring, the pull's payload fitting, the RSSI note table, `obs.wake` | ✔ against fakes, and the `OBS_PULL` fixture through the real session | advert capture, the two overrides | adverts heard on the roof |
| descriptor, manifest | ✔ against the fixtures | placement in the image | the Walter reads it |

## 11. Proved by task p1-01

What §2 and §3 rest on, checked against the pinned checkout (`d9296435`, `repeater-v1.17.1`) and
against the build, by the task that first compiled our application onto upstream's repeater
(`tasks/phase-1/reports/01.md`). Line numbers are the pin's.

| Claim | Evidence |
|---|---|
| `build_as_lib.py` compiles `examples/<name>/*.cpp` for `BUILD_EXAMPLE` and leaves one file out for `EXCLUDE_FROM_EXAMPLE` | `build_as_lib.py` lines 42–53: `+<../examples/{example_name}/*.cpp>` then `-<../examples/{example_name}/{exclude_name}>`; the exclude needs `example_name` already set, so the define order matters. The linker map (`out/build/<env>/firmware.map`) has `libMeshCore.a(MyMesh.cpp.o)` and no `main.cpp.o` member of that archive; `setup` and `loop` are in `src/main.cpp.o` |
| `MC_VARIANT` compiles the whole variant directory, `target.cpp` included | `build_as_lib.py` lines 37–40: `+<../variants/{variant_name}>`. Without the define the map has exactly one `target.cpp.o`, ours, and `RAK4631Board.cpp.o` / `variant.cpp.o` in `libmeshcore_variant.a` |
| `MyMesh : public mesh::Mesh, public CommonCLICallbacks`, most state private | `MyMesh.h`: the members from `FILESYSTEM* _fs` to `matching_peer_indexes[MAX_CLIENTS]` precede the first access specifier |
| `handleCommand(0, command, reply)` is public | `MyMesh.h`, under `public:`: `void handleCommand(uint32_t sender_timestamp, char* command, char* reply);`. `sender_timestamp == 0` is the console's privilege (`MyMesh.cpp:1262`, `get acl`) |
| `getNodePrefs()`, `savePrefs()` are public | `MyMesh.h`, under `public:`; `savePrefs()` calls `_cli.savePrefs(_fs)`, which rewrites `/prefs.json` (`CommonCLI.cpp:143`) |
| the settings seam is virtual and the CLI uses it | `SensorManager.h:21–24`: `getNumSettings`, `getSettingName`, `getSettingValue`, `setSettingValue`, all `virtual`; `CommonCLI.cpp:276–316`: `sensor get` (`getSettingByKey`), `sensor set` (`setSettingValue`), `sensor list` (the three by index). `TwinOakSensors` overrides the four with `override` and the build accepts it |
| telemetry goes through `sensors.querySensors(perm, lpp)` | `MyMesh.cpp:250`, in `handleRequest` for `REQ_TYPE_GET_TELEMETRY_DATA`; `querySensors` is `virtual` (`SensorManager.h:19`) |
| `getBattMilliVolts()` is virtual on the board | `MeshCore.h:47`: `virtual uint16_t getBattMilliVolts() = 0;` on `mesh::MainBoard`; `RAK4631Board.h` overrides it with the ADC reading; `MyMesh.cpp:218` and `:244` call it for `status` and the telemetry base channel |
| the eight overrides are virtual and reachable | `MyMesh.h`, under `protected:`: `searchPeersByHash`, `getPeerSharedSecret`, `onPeerDataRecv`, `onPeerPathRecv` (`override`), `onAdvertRecv` (no keyword; virtual via `Mesh.h:122`), `logRx`, `logTx` (`override`); `onTraceRecv` is not in `MyMesh.h` at all — the base is `Mesh.h:107`'s empty virtual. `src/app/TwinOakMesh.h` declares all eight with `override` and the build is green; `nm` shows all eight `TwinOakMesh::` symbols beside the `MyMesh::` ones |
| `Mesh::onRecvPacket` tries matched peers in index order and the first MAC that verifies wins | `Mesh.cpp:147–186`: `searchPeersByHash(&src_hash)`, then `for (int j = 0; j < num; j++) { getPeerSharedSecret(secret, j); … MACThenDecrypt … if (len > 0) { … onPeerPathRecv(pkt, j, …) or onPeerDataRecv(pkt, type, j, …); found = true; break; } }`. Upstream's match array is `matching_peer_indexes[MAX_CLIENTS]` (32), filled at `MyMesh.cpp:623` |
| `onTraceRecv` is called for a direct TRACE at the end of its path | `Mesh.cpp:56–57`: `if (offset >= len) onTraceRecv(pkt, trace_tag, auth_code, flags, pkt->path, &pkt->payload[i], len)` |
| `logRx` sees the packet while its RSSI is still the radio's last | `Dispatcher.cpp:238`: `logRx(pkt, pkt->getRawLength(), score)` in the receive path, after `_radio->getLastSNR()`/`getLastRSSI()` were read for the score and before the packet is queued inbound; `logTx` at `Dispatcher.cpp:109`, when the outbound packet has been handed to the radio |
| `createAnonDatagram`, `createDatagram`, `createTrace`, `sendDirect`, `sendFlood` are public on `Mesh` | `Mesh.h:185–224`, under `public:` |
| "no saved preferences" is readable | `CommonCLI.cpp:30–47`, `loadPrefs`: `/prefs.json` is loaded if it exists, else `/com_prefs` is converted if it exists, else nothing is loaded and the constructor's defaults stand. `src/app/FirstBoot.cpp` tests exactly those two files, before `begin()` runs `loadPrefs` |
| the admin password's field is 16 bytes | `CommonCLI.h:29`: `char password[16];` — fifteen characters and the NUL, which is what `StrHelper::strncpy` leaves (`TxtDataHelpers.cpp:3`) and what the login compares with `strcmp` (`MyMesh.cpp:102`). A sixteen-character password cannot exist at this pin; ours is fifteen |
| upstream's default password is the constructor's and only the constructor's | `MyMesh.cpp:32–34` defaults `ADMIN_PASSWORD` to the word every stock repeater ships with when the define is absent; `MyMesh.cpp:897` copies the define into `_prefs.password`. Our build defines it as `not-a-password` and `FirstBoot.cpp` overwrites it in `setup()`, before `loop()` runs |
| the radio's RNG | `RadioLibWrappers.h:88`: `RadioNoiseListener : mesh::RNG`, radio noise xor the CC310's generator under `USE_CC310_HW_CRYPTO`; `radio_new_identity()` draws the node's key from it and `radio_random()` the first-boot password |
| `-Werror` on `src/` survives upstream's headers | not by itself: under `-Wextra`, `MeshCore.h`, `Dispatcher.h`, the SoftDevice's `nrf_soc.h` and `RAK4631Board.h` warn (unused parameters, a reordered initialiser, a literal as `char*`), and RadioLib's `#warning` for GODMODE is an error under `-Werror=cpp`. `src/board/UpstreamHeaders.h` and `src/app/UpstreamMyMesh.h` include upstream as system headers (`#pragma GCC system_header`, the agent firmware's `MeshCoreHeaders.h`), and `-Wno-error=cpp` keeps the `#warning` a warning. An `-isystem` flag was tried first and made the core's `WString.h` conflict with itself on this toolchain |
| upstream's tree is not written by the build | `git status --porcelain` in `.pio/libdeps/repeater_rak4630_single/MeshCore` is empty after the build, `HEAD` is the pin |

Corrections and additions made above by **p1-06**, power and environment: §1 (what is under
`src/power/`), §3 (`getBattMilliVolts()` is channel 2 now, and `TwinOakSensors` gained
`begin()`/`loop()`/`querySensors()`; **upstream's BME280 support had to leave the build** too, for
a reason this file did not foresee - it samples the part inside `querySensors`, which is on the
path of `LPP_GET`), §4 (the settings table is seven keys and `sensor list` pages), §6 (rewritten:
one cache and its four readers, why no request touches a bus, what the I2C layer cannot promise,
the thresholds' own rules, and the one field list with two appliers) and §10. What that task read
of upstream is cited where it is used - `src/power/Environment.h` for upstream's BME280 query,
`src/power/Telemetry.{h,cpp}` for `handleRequest`'s two fields and CayenneLPP's `addField`,
`src/board/target.h` for what reads `getBattMilliVolts()`, `src/power/BoardPower.cpp` for the
core's TWIM driver - and `docs/upstream.md` place 5 lists them.

Corrections and additions made above by **p1-05**, local management: §1 (`src/app/Settings.*` and
`src/link/attention/`), §4 (what `CLI` does and what it costs; the attention machine; the settings
table gained `fw.version` and the order `sensor list` really prints, and who may run `sensor set`
over the mesh, from the source at the pin) and §10. What that task read of upstream is cited where
it is used — `src/link/BoardLink.cpp` for `handleCommand`, the reply window and the four commands
that never return, `src/app/Settings.{h,cpp}` for the ACL gate and `sensor list`'s 134-byte
cut-off — and `docs/upstream.md` place 5 lists them.

Corrections made above by **p1-04**, the clock: §1 (what is under `src/clock/`), §2 (two more
rows of the table - the virtuals the clock keeper needs, which p1-01 did not know about - and "the
eight" is now ten), §5 (rewritten: the file is the only memory there is, and why; the *clean*
flag; the sane window; the jump poll; which virtuals count as a timestamp leaving the node) and
§10. What that task read of upstream is cited where it is used - `src/clock/BoardClock.cpp` for
the clock helpers and the reset reason, `src/app/TwinOakMesh.h` for the four virtuals and the
advert timers, `src/board/target.h` for `reboot()`/`powerOff()` - and `docs/upstream.md` place 5
lists them.

Corrections made above by that task: §1 (the wrapper headers), §2 (the define order, `UITask.cpp`,
`the_mesh`, `onAdvertRecv`'s missing keyword), §3 (`TwinOakBoard` and the virtual base; the
battery reading stayed upstream's until task 06; `variant.cpp` is the only pin map, not one of
two; `MC_VARIANT` refused; how `<target.h>` resolves). Phase 0's report said the core compiled its
own `WisCore_RAK4631_Board/variant.cpp` beside MeshCore's; it does not, there is no such
directory at the pin.
