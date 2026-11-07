# How this firmware is put together

The design the task piles build. It was written from MeshCore's source at the pin
(`d92964352441e53b93e8667b802e04f6e072b39e`, `repeater-v1.17.1`) and from the agent firmware's,
before a line of this repository compiled, and re-read at `81dbae592ffad40b4212bc0b37e82b38da08f4bc`
(`repeater-v1.18.0`) by p3.5-06, the first real bump: the line numbers below are that pin's. Where it says *upstream does X*, a file and a function
are named so that the claim can be checked; phase 1's first task checks the ones everything rests
on, and this file is corrected by whichever task finds it wrong.

## 1. Layout

```
platformio.ini          the pin, the libraries, one [img_*] and one [env:*] per image, the
                        [band_*] blocks, [env:native]
boards/                 board definitions PlatformIO must see before it clones a dependency
src/
  common/               what the roles on this board share. An environment compiles all of it,
                        or - a role that compiles no MeshCore - the files of it that are not
                        MeshCore's side of the build (section 8)
    board/              target.h / target.cpp for our adapter: the board object, the radio, the
                        clock, the sensors; Boards.h (a row per board: the radio slot);
                        RoleHooks.h (the six things common/ asks of the role); UpstreamHeaders.h
                        and CoreHeaders.h, the two places upstream's headers are included from
                        (section 11)
    bands/              Bands.h (a row per band) and the static_assert that holds a build's
                        default frequency to the band it says it is in
    fwdesc/             the image descriptor: the portable builder, and this image's own 96
                        bytes as a const object the firmware reads its HELLO out of
    link/               the box link: codec (portable), session and dispatcher (portable),
                        attention (the machine of section 8, portable), UART transport
    power/              INA3221 with per-channel shunts, BME280, the two readers' schedules,
                        the battery thresholds and the Cayenne-LPP (all portable but BoardPower)
    version/            the version string, "twinoak-<role> <version> on MeshCore <tag> (<pin>)"
  roles/                one directory per role. An environment compiles exactly one of them
    repeater/
      main.cpp          ours: setup() and loop(), replacing upstream's example main
      app/              TwinOakMesh (the subclass), FirstBoot (the one test and the one write),
                        IdentityGuard (a key is made only where no node was; the lost mode),
                        Settings (our keys on upstream's CLI, portable), RoleHooks.cpp (this
                        role's half of them)
      firstboot/        the first-boot profile: a table of console command lines, the runner that
                        judges each reply, and what `sensor get firstboot.state` says (portable)
      clock/            ClockKeeper (the rules, portable), BoardClock (the board's ports and
                        its flash file), LinkClock (the keeper as the link's ClockAccess)
      link/             BoardLink: the one Session and the board facts it answers from
      meshclient/       the job, the five sends, the reply, the observation ring (state portable)
    scanner/            NoiseScope, moved here from the hardware repository (docs/noisescope.md)
      main.cpp          ours: setup() and loop(), the two line buffers, the command dispatch
      Commands, Lines, Sweep
                        portable: what a command means, every NDJSON line, the sweep's points and
                        the RSSI window - compiled by [env:native] beside test/test_scanner
      Radio, Telemetry, Wdt
                        the SX1262 through RadioLib, the RV-3028 and the INA3221, the watchdog
test/                   native tests, one directory per subject
spec/                   a copy of the system specification and its fixtures - never edited here
keys/                   release.pub, the public half of the release key - and nothing secret, ever
tools/                  verify.ps1, release/ (the release tooling and its tests), pio/ (build
                        scripts), noisescope/ (the scanner's waterfall plotter), ci/ (the guard
                        on where each workflow runs - README.md "Where CI runs"), bench/ (the one
                        way a session touches a board: DFU of our own images after its guards,
                        a console that never sends or prints a password, a lock per board,
                        restore - docs/bench.md section 0a; its tests run in the release step)
docs/                   this file, release.md, upstream.md, link.md, bench.md, noisescope.md
tasks/                  the piles
```

**`common/` never includes a role's header.** The six places it would have to are one file,
`src/common/board/RoleHooks.h`: the restart the role gets its last word before, the attention a
battery raises, and the four calls of the role's own settings on upstream's CLI. A role defines
all six and there is no weak default, so a role that forgets one does not link.

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
| make the first boot's one write one write (§4a) | `savePrefs()` (public, `CommonCLICallbacks`) | virtual |
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

`src/common/board/target.{h,cpp}` is ours: MeshCore's `variants/rak4631/target.cpp` assumes a WisBlock
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
  `src/roles/repeater/main.cpp` already calls once in `setup()` and once in `loop()`; `querySensors()`, which
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
`src/common/board` is first on the path, `variants/rak4631` (still needed for `variant.h`'s LoRa pins and
`RAK4631Board.h`) after it. The MeshCore library's own files that include `target.h` —
`MyMesh.cpp`, `UITask.cpp`, `helpers/NRF52Board.cpp` — get ours too, which is why its globals keep
upstream's names and base types.

I2C runs at 100 kHz or below: one set of 1 k pull-ups, through a 1000 pF bulkhead filter
(`INTERCONNECT.md`).

## 4. The link, inside the firmware

`spec/LINK-PROTOCOL.md` is the protocol. In the firmware it is three layers:

1. **Codec** (portable, `src/common/link/codec/`, proved by p1-02): `Crc16`, `Cobs`, `Packet`,
   `FrameReceiver` (which counts what it drops) and `FrameWriter` are the framing; `Numbers.h` is
   every type, status, mode and limit of the protocol in one header; `Bytes.h`'s `Reader` and
   `Writer` are the little-endian scalars; `Messages.{h,cpp}` is a plain struct per payload with
   the one direction this side of the link needs — **a decoder for every request, an encoder for
   every response and every event**, because only the Walter sends requests and only the RAK
   sends events, and the other half of each pair is the Walter's firmware's. Tested against every
   case of **every** file of `spec/fixtures/link/` — `link-v1.json`, which is 1.0's and whose
   bytes never change, `link-v1.1.json`, which is 1.0's cases and the four 1.1 adds, and
   `link-v1.2.json`, which is 1.1's and the fourteen 1.2 adds (`test/test_link_codec`) — with a
   test that fails when a fixture gains a case nothing exercises. `kProtoMinor` is the one place the firmware says which minor it speaks: the image
   descriptor's `link_minor` and the release manifest's `link` are both read from it.
2. **Session** (portable, `src/common/link/session/`, proved by p1-03): a receiver that turns bytes into
   packets and counts what it drops; a dispatcher from request type to handler; the one-entry
   memory of the last request and its response, which is what makes a retried request harmless;
   the event counter. It talks to the rest of the firmware through the interfaces of
   `Interfaces.h` — the system, the identity, and one each for the clock, the console,
   attention, power, the environment, the mesh client and the observation ring — so that the
   native tests run it whole against fakes (`test/test_link_session`). **A capability is an
   interface that is there**: `HELLO`'s `caps` is built from which of those pointers are non-null
   and from nothing else, so a firmware that has not implemented a thing does not advertise it and
   answers `ERR_UNSUPPORTED` for it. Nothing keeps a second list of what a build can do.
3. **Transport** (`src/common/link/SerialTransport.*`): `Serial1` at 115200. Read what is there, write
   what fits, never wait. `src/roles/repeater/link/BoardLink.*` is the board's half of those interfaces —
   uptime, the reset reason, the per-boot identifier from the radio's RNG, the restart, the
   identity — and the one call `src/roles/repeater/main.cpp` makes in each of `setup()` and `loop()`.

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
not owed and the handler does not try to out-run the restart. **Nothing under `src/common/link/` prints**,
and a rails test holds that: a `CLI` line can be `password <new>`, whose reply echoes it back.

**Attention** (`src/common/link/attention/`, §8) is one small portable machine: one reason pending with
a queue behind it, the two `0x00` wake bytes, 300 ms, then `EV_ATTENTION` every 1000 ms with
`repeat` counting from 0 and at most ten sends, ended by any valid request, and at most one new
attention per reason per hour. It also holds the belief that the Walter is asleep
(`SLEEP_NOTICE`): while that is set the session sends no events but `EV_BOOT` and `EV_ATTENTION`.
The session asks it once a pass what to put on the wire and tells it when the bytes have gone, so
every rule about *when* is in one file with a fake clock in front of it.

Ten more ways in, through `TwinOakSensors`' settings — the keys and what each one does are
`src/roles/repeater/app/Settings.{h,cpp}`, portable and tested; the board's half is a dozen one-line readers and
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
| `client.refused` | mesh-client sends the ceiling refused since boot - 0 on a healthy unit (§7) | — |
| `firstboot.state` | `done`, `partial: <n> refused`, or `never` because this boot found settings and ran nothing (§4a) | — |

Ten keys no longer fit one `sensor list` reply: upstream stops a listing at 134 bytes of a
160-byte buffer, so a fresh node prints the first five and `sensor list 5` is the other five. The
order is task 05's five with the later tasks' appended, which is the only place a new key can go
without renumbering one a bench instruction or a document already names.

The tenth key cost one character of that first page, and it is worth knowing where it went:
upstream's own header is `10 vars` rather than `9 vars`, which makes the page 160 characters where
it was 159 — and a reply is 159 characters and a NUL, on the console and on the link alike. So the
digit of `... next:5` is cut. Both the console and the session keep **64 guard bytes** behind
upstream's window, so the overrun lands in the guard and not in a stack frame; the session counts
it (`cliTruncations`), and `src/roles/repeater/main.cpp` gained the same shape for the console,
which until then had nothing behind its window at all.

### 4a. The first boot

A board fresh from the flasher has no `/prefs.json`, and that one fact decides everything a unit
does before anybody has been to the site (`spec/SYSTEM.md` S-12, S-13; `docs/provisioning.md`):

- **the channel is the build's**, from `platformio.ini`'s `[band_*]` block (§8), so a unit is on
  the right frequency even if every step below failed;
- **the admin password is random**, fifteen alphanumerics from the radio's own noise, never
  printed and never on the air (`src/roles/repeater/app/AdminPassword.h`);
- **the profile runs**: a table of console command lines through `handleCommand(0, …)`, the same
  call a person at the USB port and the Walter over the link make
  (`src/roles/repeater/firstboot/Profile.h`). It is text, not code, for three reasons - it touches
  no private member of upstream's, a pin bump that renames a key comes back as a reply that is not
  `OK` rather than as a corrupted preference, and it can be read and changed without reading C++.
  As shipped it is **six** lines, because six are what a document of this project gives a value
  for: `set path.hash.mode 1` (S-12 and meshmaster.dk's D-33: upstream floods this node's adverts
  at `path_hash_mode + 1` bytes); `set dutycycle 10` — the duty cycle the home channel's band
  allows, which is a regulation and not a judgement, so it lives in the band
  (`platformio.ini`'s `[band_868].duty_cycle`, `src/common/bands/Bands.h`) and the line is
  **generated from it** rather than written (§7 has the arithmetic and why upstream's own 50 %
  was the wrong place to start); and the **Danish standard**'s four, which Thomas settled on
  22 October 2026 — `set agc.reset.interval 4`, `set loop.detect moderate`,
  `set flood.max.unscoped 15`, and `set flood.advert.interval <h>` with **`h` derived from the
  node's own public key**, `60 + (k mod 26)` for `k` its first byte, so that units flashed
  together do not flood their adverts in step (upstream gives every unit 47 hours).
  `docs/provisioning.md` §1 is the table with each line's reason and states that rule.

  Two of those six are therefore not *written*: one is built from the band at compile time and one
  from the node at run time. The second is the only growth the runner has ever needed - a
  `ProfileLine` may carry a `build` function instead of being text, and `runProfile` takes the
  node's key (`firstboot::NodeFacts`) - and the table still reads as a table, because such a row
  carries the line as `set flood.advert.interval <h>` for a person to read and for a refusal to
  print. The key exists before the profile runs and no order had to move for it: `main.cpp` loads
  or makes `the_mesh.self_id` a dozen lines above the `firstBoot()` call.

**Once is exact, and it is one boolean.** The test is `hasSavedPrefs()`, taken before
`the_mesh.begin()` runs upstream's `loadPrefs`; the password and the profile both happen inside one
hold of `TwinOakMesh::savePrefs()`, which is the eleventh override and exists only for this. Every
`set` branch of upstream's CLI saves, and `CommonCLI::savePrefs(FILESYSTEM*)` removes
`/prefs.json` and writes it again — so a first boot that saved line by line could be interrupted
with the file present and the work half done, and the next boot would run no profile at all. With
the saves held the file appears **exactly once**, with everything in it: power cut before that
moment and the next boot does the whole thing again; power cut after it and the unit is done.
There is no second marker anywhere to disagree with the file.

A refused line does not stop the run - the lines are independent settings - and the outcome is
readable afterwards from all three ways in: `sensor get firstboot.state`. It is per boot and in
RAM, because the question it answers is "what did *this* boot do".

One consequence of running the profile at all is upstream's and worth recording: `CommonCLI::savePrefs()`
zeroes `advert_interval` when it is below sixty minutes, "now that device has been manually
configured". Upstream's default is two minutes, so the first line of the first-boot profile turns
the *local* advert timer off for the life of the unit. The boot advert (§5) and the flood advert
— every 60 to 85 hours, the number this node's key gives it — are what announce it, which is what
the Danish standard asks for anyway (meshguide.dk recommends `advert.interval 0`).

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

`src/roles/repeater/clock/` is three files: `ClockKeeper.{h,cpp}` is every rule, portable and tested natively
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
  it. Refuse a second send while a reply is awaited, and refuse one that is over the ceiling
  below.
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

### The ceiling on what the client may send

The Walter's agent paces itself to a budget the MeshMaster server sets - `budget 2/min gap 20 s`
by default (twinoak/meshmaster-agent-firmware's README at `51faafb4`). The RAK enforces a ceiling
that budget cannot reach: **at most 6 sends in any 60 seconds and at least 2 seconds between two**
(`src/roles/repeater/meshclient/SendCeiling.h`, portable, tested against a fake clock). Over it a send is refused
`ERR_BUSY` before anything is built and counted in `sensor get client.refused`; the ceiling is
counted in the same two lines as `client_tx`, so the two cannot disagree about what a send is, and
a refused send costs nothing - it does not enter the window and does not restart the gap. The
limits are build constants and not settings: a limit the Walter could raise would be no backstop
against the Walter. A Walter told `ERR_BUSY` tells the two causes apart by `STATUS`'s flag bit 1,
which is set when a reply was awaited and clear when it was the ceiling.

### Repeating comes first

Two questions about upstream, read out of the pinned checkout (`d9296435`, and re-read at
`81dbae59` by p3.5-06, whose line numbers these are), because this is the one place where our
traffic could cost the mesh something.

**The duty cycle covers our packets too.** There is one outbound queue and one budget.
`Dispatcher::sendPacket` puts every packet - ours and every one being forwarded - into the same
`PacketManager::queueOutbound` (`src/Dispatcher.cpp:371-378`); `Dispatcher::checkSend` is the only
thing that takes one out (`:275-354`), and before it does it refuses to transmit at all while
`tx_budget_ms` is below half the estimated airtime of a maximum packet (`:279-286`). When a send
completes, the budget is decremented by its **measured** airtime (`:88-98`), whoever originated it.
So our 6 sends a minute are inside upstream's accounting and not beside it. The budget is
**this repeater's own setting**: `MyMesh` overrides `getAirtimeBudgetFactor()` to return
`_prefs.airtime_factor` (`examples/simple_repeater/MyMesh.h:130-133`), its constructor sets that to
`1.0` (`MyMesh.cpp:891`), and a console `set dutycycle <1..100>` or `set af <x>` changes it and
saves it (`src/helpers/CommonRadioPrefs.cpp:88-100` and `:65-79` - since 1.18.0 every radio key is
that class's, reached from `CommonCLI::handleCommand` before anything else; `set af` also refuses
a factor outside 0-9 there, where 1.17.1 took it and `loadPrefs` clamped it on the next boot). `1.0` is a duty cycle of 1/2 and a
budget of 1 800 000 ms in a 3 600 000 ms window - **which is not what the band allows**, so a unit
of ours does not start there: the first-boot profile's second line is `set dutycycle 10`, built
from `[band_868].duty_cycle` (`src/common/bands/Bands.h`, `src/roles/repeater/firstboot/Profile.cpp`;
869.618 MHz is in the 869.400-869.650 MHz sub-band of ERC Recommendation 70-03, Annex 1, row h1.7
- 500 mW e.r.p. and 10 %, or LBT+AFA instead, which this firmware does not claim). 10 % is also
the lowest this upstream can keep: `loadPrefs` clamps `airtime_factor` to 9.0
(`src/helpers/CommonCLI.cpp:111`), and 9.0 is
exactly `100/10 - 1`.

> The regulation itself is the one thing on this page that was not read out of a file on this
> machine: CEPT's own PDF of ERC Recommendation 70-03 would not fetch from the session that wrote
> this (a TLS certificate it could not verify), so the edition - **June 2024**, the newest CEPT's
> document database lists - the row (**h1.7**) and the pair *500 mW e.r.p., 10 %* come from that
> database's listing and not from the document. The sub-band and the 10 % are also what the
> phase-3 review states and what the Danish standard uses. A reviewer with the PDF open should
> confirm the edition and the row; the number itself is in one place
> (`platformio.ini`'s `[band_868].duty_cycle`) and everything else is built from it.

**The numbers at the band's ceiling.** The budget is then **360 s of each hour**, 6 s a minute.
One 186-byte packet is about 1.7 s of airtime on this channel (869.618 MHz, 62.5 kHz, SF8, CR8, 32
preamble symbols), so six of the longest packets this client can build is some 13 s a minute -
**above** the refill, where against the 50 % default it was comfortably under it. What then
happens is that **upstream delays**: `checkSend` holds the queue until the budget has recovered
(`:279-286`, and the `MIN_TX_BUDGET_RESERVE_MS` push-out at `:99-104`), nothing is refused, nothing
is lost, and the hour's transmission stays inside 360 s whoever asked for it. The exchange a
server actually asks for is nowhere near that: two sends a minute of about 0.65 s each is some
1.3 s a minute - about 2 % of the hour, which is **a fifth of the budget**, leaving the rest for
the hundreds of packets a busy repeater forwards.

**And the client's ceiling stays at 6 a minute.** It was asked whether it should follow the band's
budget down (three a minute would be about 6 s), and it should not:

- the two count different things. The ceiling counts **sends**; the budget counts **airtime**, of
  every packet the radio transmits. Only the second can hold a regulation, and it does - at the
  band's value from the first boot on. A lower ceiling would not make the band any safer;
- 6 is three times the server's default rate of 2 a minute, and that margin is the point of it.
  Three a minute is one and a half times it, which a *correct* paced agent can touch - a login,
  its request, and a retry after a timeout inside the same minute - and `ERR_BUSY` would stop
  meaning "the Walter is broken";
- what a lower ceiling would buy is latency for a packet waiting to be forwarded, and that is
  already bounded by the answer below: one client send can delay a forward by its own airtime and
  no more.

So the ceiling is about the mesh's patience and upstream's budget is about the band: only
upstream's can hold the regulatory limit, because only upstream's sees the hundreds of packets a
busy repeater forwards - and since this task it holds the limit the band actually has.

**A client send can delay a forward by its own airtime and no more.** The queue is a flat table
picked over by priority, lowest number first, and ties go to whichever was added first
(`PacketQueue::get`, `src/helpers/StaticPoolPacketManager.cpp:22-44`). What the numbers are:

| what | priority |
|---|---|
| a forward of routed (direct) traffic | **0** - "Routed traffic is HIGHEST priority" (`src/Mesh.cpp:106`) |
| **our direct sends** (a login, a request, a CLI line along a path) | **0** (`Mesh::sendDirect`, `:693-715`) |
| a forward of a flood | **its hop count after ours is appended**, so 1 for a direct neighbour's and more as it propagates (`Mesh::routeRecvPacket`, `:344-357`) |
| **our floods** | **1** (`Mesh::sendFlood`, `:637-662`; 2 for a PATH, 3 for an advert) |
| a forward of a trace, and **our traces** | **5** (`:64`, `:698-704`) |

So a send of ours never jumps a forward of routed traffic that is already queued - equal priority,
first added wins - and it is ahead of a flood forward that has travelled two hops or more. Being
ahead costs that forward exactly the airtime of our packet: one packet, then the queue is picked
over again. Two things do not change that. While a send is in flight `Dispatcher::loop()` returns
early (`src/Dispatcher.cpp:126`), so nothing else is processed - but that is true of every packet
upstream sends, ours or a forward, and a radio that is transmitting could not be receiving anyway.
And when a completed send leaves `tx_budget_ms` below `MIN_TX_BUDGET_RESERVE_MS` (100 ms) the next
transmission is pushed out (`:99-102`) - which is 100 ms out of a budget of 1 800 000 and needs
the node to have transmitted for half the hour before it can be reached at all. **No fix is
needed, and none is proposed.** What would need one: a client that could queue several packets at
once, which the ceiling's 2-second gap also forbids.

One shared resource is worth naming, because it is not the queue: the **packet pool** is 32
packets (`new StaticPoolPacketManager(32)`, `examples/simple_repeater/MyMesh.cpp:864`) and a
client send holds one from the moment it is built until the radio has sent it. A received packet
that finds the pool empty is dropped (`Dispatcher::checkRecv`, `src/Dispatcher.cpp:201-203`). With
at least 2 seconds between our sends and about 1.7 s of airtime for the longest of them, ours
holds one entry at a time in anything but a pathological case - and the pathological case is a
radio that cannot transmit, where the 31 entries left are not what the node's trouble is.

## 8. The family: roles, boards, bands

An **image** is one role on one board in one band (`spec/FIRMWARE-MANIFEST.md` §2), and it is one
PlatformIO environment, named `<role>_<board with its hyphens as underscores>`:
`repeater_rak4630_single`.

| Role | What | When |
|---|---|---|
| `repeater` | everything above | phases 0–2 |
| `scanner` | NoiseScope: the SX1262 spectrum scanner, RX only, never writes flash (`docs/noisescope.md`) | phase 3 |
| `bridge` | two radios of one unit carrying packets between two bands over a UART of their own | later |

| Board | Slot | What | When |
|---|---|---|---|
| `rak4630-single` | 0 | the single-RAK adapter that was ordered | phases 0–2 |
| `rak4630-dual-a`, `-b` | 0, 1 | the two positions of the dual adapter; `a` owns the I2C bus and the link's telemetry | later |

| Band | What it is | The radio's defaults | When |
|---|---|---|---|
| `868` | 863–870 MHz | 869.618 MHz, 62.5 kHz, SF8, CR8, +22 dBm | phases 0–2 |
| `433` | — | — | when a radio has been measured against it |

### The three names are written in one place

`platformio.ini` keeps one `[img_*]` section per image — `role`, `board`, `band` — and that is
the only place any of the three is written. Everything else derives from them:

| Who | What it reads |
|---|---|
| the environment's name | `<role>_<board>` with underscores, and `[img_<env>]` is named after it |
| `TWINOAK_ROLE`, `TWINOAK_BOARD`, `TWINOAK_BAND` | the defines the environment passes, interpolated from its own `[img_*]` section |
| the image descriptor | `src/common/fwdesc/ImageDescriptor.cpp`, from those three defines, at compile time |
| the link's `HELLO` | `role`, `board` and `version` read **out of the descriptor object** (`src/roles/repeater/link/BoardLink.cpp`); `slot` from the board's row in `src/common/board/Boards.h` |
| the version string | `twinoak-<role> …` (`src/common/version/Version.cpp`) |
| `buildinfo.json` | `tools/pio/buildinfo.py`, from the same three defines |
| the manifest entry | `tools/release/build_release.py`, from `buildinfo.json`; the image `id` is `<role>.<board>.<band>` |

`test/test_image` reads `platformio.ini` and holds each of those against it, and
`tools/release/test_release.py` runs every `[img_*]` section through the release tooling. A name
that drifts is a red test and not a wrong image.

### The band is real

A 433 MHz module is a different part and must never be given an 868 image, so the band is not a
label. Each band is a `[band_*]` block in `platformio.ini` — the frequency, bandwidth, spreading
factor, coding rate and transmit power an image of that band is built with — and a row in
`src/common/bands/Bands.h` saying what the band *is*. `src/common/bands/BandCheck.cpp` is two
`static_assert`s over that table: a build whose `TWINOAK_BAND` has no row, or whose `LORA_FREQ`
lies outside it, does not compile, and the error is a sentence. The mechanism is tested with a
made-up band in `test/test_image`, because `433` has no block yet.

### Every image says what it is

Every image carries the 96 bytes of `spec/FIRMWARE-MANIFEST.md` §4, built at compile time as a
`const` object (`src/common/fwdesc/`) — C++11 `constexpr` is enough for the CRC-32 and the
string fields at the board's standard, so nothing patches the linked image afterwards and no
checksum downstream is of bytes the linker did not produce. The firmware *reads* its `HELLO` out
of that object, which is what keeps the linker from discarding it and what makes the two unable
to disagree.

Three things then check it, in the order a release meets them:

1. **the build** — `tools/pio/descriptor.py`, a post-build script on every board environment: it
   cuts the raw application binary out of the linked `.hex` (leaving it as `firmware.bin`), finds
   the descriptor with `spec/tools/manifestref.py`'s own `find_descriptor`, and fails the build
   unless there is **exactly one** and its fields are the environment's;
2. **the release** — `--require-descriptor`, on in `.github/workflows/release.yml` and in the
   local dry run of `tools/verify.ps1`: no image is published whose descriptor is missing,
   doubled or not what its manifest entry says. The entry also carries `requires.link_major`,
   the box-link major the image speaks;
3. **the Walter** — which reads the bytes out of what it downloaded before it flashes anything
   (`spec/FIRMWARE-MANIFEST.md` §5, step 5), and after the flash holds the `HELLO` it gets
   against the descriptor it wrote.

A role that cannot do something says so in its capability bits rather than failing a request; the
bridge will have its own messages in the reserved range. The scanner is the exception that proves
the rule: NoiseScope 0.1 speaks text lines on the same UART, not frames, and the Walter tells the
two apart as `spec/LINK-PROTOCOL.md` §9 says. It still carries a descriptor, which is how the
Walter knows which of the two it is about to flash before it flashes it.

For the bridge, the second UART (`Serial2`, P0.19/P0.20) is reserved to connect the two radios of
a dual adapter. `Serial1` stays the link to the Walter on every board.

### What a role compiles of `common/`

An environment compiles `common/` beside exactly one directory of `roles/`, and the repeater's
compiles all of it. Three files of `common/` are **MeshCore's side of the build** —
`board/target.cpp`, which exists to be upstream's `<target.h>`; `link/SerialTransport.cpp`, the
box link's frames on a UART; `power/BoardPower.cpp`, which keeps a preferences file in the
internal flash — and a role that compiles no MeshCore cannot have them. The scanner is such a
role, so `[env:scanner_rak4630_single]`'s `build_src_filter` names the files of `common/` it
uses, the way `[env:native]`'s does: the descriptor, the band table, the version, and the
INA3221's arithmetic, reader and bus. A file it needs and that is not listed does not link,
which is the loud end of that trade.

The headers follow the same split. `src/common/board/CoreHeaders.h` is the Arduino core's own two
headers as system headers, and `UpstreamHeaders.h` includes it and adds everything above the core
(section 11); a file of `common/` that needs only `Wire` — today `power/WireBus.cpp` — includes
the first and stops there, so it can be in a build that has no MeshCore at all.

### Adding a role

1. `src/roles/<role>/` with its own `main.cpp`, and a definition of every function of
   `src/common/board/RoleHooks.h` that the files of `common/` the environment compiles reach —
   all six for a role that compiles all of `common/`, and none for one that compiles neither
   `board/target.cpp` nor `power/BoardPower.cpp`. There is no weak default: a role that forgets
   one does not link, and the error names it.
2. `[img_<role>_<board>]` in `platformio.ini` with `role`, `board` and `band`, and
   `[env:<role>_<board>]` beside it: `build_src_filter = +<common/> +<roles/<role>/>` (or the
   list above), the three `TWINOAK_*` defines interpolated from that `[img_*]` section, and the
   band's radio flags from its `[band_*]` block.
3. A row in the **Role** table above. The role's name must be one of the codes of
   `spec/FIRMWARE-MANIFEST.md` §4 — `bridge` and `scanner` are already there;
   `src/common/fwdesc/Descriptor.h`'s `roleCode()` is this firmware's copy of that list, and a
   role it does not know fails a `static_assert`.
4. Nothing else. `tools/verify.ps1` and both workflows read the board environments out of
   `platformio.ini`, so the new image is built, descriptor-checked and released by itself.

### Adding a board

1. A row in `src/common/board/Boards.h`: the board as `FIRMWARE-MANIFEST.md` spells it, the radio
   `slot` its `HELLO` reports, and the `descriptor_slot` an image of it may be put in.
2. `[img_*]` and `[env:*]` as above and — if the pin map or the parts differ — a board target
   beside `src/common/board/target.cpp` with the same globals upstream's `<target.h>` expects
   (section 3).
3. A row in the **Board** table above. A board that brings a new band brings a `[band_*]` block
   and a row in `src/common/bands/Bands.h` with it.

## 9. Flash, and what an update may touch

```
0x00000000  the MBR                      ┐
0x00001000  the SoftDevice S140 6.1.1    │ the base: written once, by a person's `rak provision`
0x00026000  the application              ◄── an image lives here and nowhere else
   …        (end of the application region: from the linker script; recorded in the manifest)
   …        the file system: identity, preferences, the ACL, regions, the clock file
0x000F4000  the bootloader               │ the base
0x000FE000  the MBR's parameter page     │ (UICR.NRFFW[1] points here)
0x000FF000  the bootloader's settings      written by the bootloader itself, not in the base
            UICR: NRFFW[0] = 0x000F4000, NRFFW[1] = 0x000FE000 - the base's; APPROTECT - the
                  Walter's, from the chip's revision, never from a file
```

The Walter flashes the application region over SWD: it erases exactly the pages the new image
covers and writes them. It never erases the chip, the file system or UICR. A firmware change that
moves the end of the application region is a change to the manifest's `app_region` and is checked
by the release tooling against the linker script (`spec/FIRMWARE-MANIFEST.md` §2).

**The file system** is the Adafruit core's `InternalFS`: LittleFS 1.7 in 128-byte blocks over
seven 4096-byte pages, `0xED000`..`0xF4000` (`libraries/InternalFileSytem/src/InternalFileSystem.cpp`
at the pinned core d541301). Two facts of that layer decide what a reset can do to a node. The
core rewrites a page by **erasing it and programming it again** through a one-page cache
(`flash_cache.c`, `flash_cache_flush`), so a reset, a halt over SWD or a power dip inside that
window takes 32 blocks at once - and the first page holds the superblock pair and the root
directory, so a torn first page is a volume that will not mount. And `InternalFileSystem::begin()`
answers a volume that will not mount by **erasing the region and formatting it**. Upstream's
`setup()` calls that and then draws a new key when `/_main.id` is missing, which is how the first
radio became a new node on 6 November 2026 (pile 3.9, task 01). This firmware mounts with the base
class's `begin()` instead - the mount alone - and decides from what it finds
(`src/roles/repeater/app/IdentityGuard.h`): a blank region (the base just written) is formatted
and is a first boot; a mounted volume with neither a key file nor settings is a first boot; the
saved key is the node; and everything else - a torn volume, a key file that does not read,
settings without a key - is a **lost identity**, on which no key is made, nothing is written, the
mesh is not started, and the console and the link say so until a person types `erase` and
`reboot`. The link in that mode is `linkBeginIdentityLost` (section 4): `IDENTITY` is `ERR_STATE`,
`caps` is `CLI | ATTENTION`, and attention reason 6 carries detail 1. The firmware's own writes
to the volume are few and known: the clock file (`src/roles/repeater/clock/`), about once an hour
and **twice in the first seconds of a boot** that restored a clock - the hour's margin, and the
Walter's time set bringing it down - and `/prefs.json` on every `set`; each is one page erased
and programmed.

**The base** - the MBR, the SoftDevice and the bootloader, with the two UICR words the MBR reads -
is not this firmware's and is not built here. A factory RAK4630 does not have it (it carries RAK's
RUI3), and the Walter writes it once, from the release's `provision` file, when a person types
`rak provision` (`spec/SYSTEM.md` S-5a; `spec/FIRMWARE-MANIFEST.md` §4a). The file is vendored byte
for byte under `boards/base/rak4630/` with its source, its hash and its licences written down, and
`tools/base/check.py` holds it to its pin and to §4a's rules - the SoftDevice ends exactly where the
application region starts, the bootloader lies above where it ends, nothing in between is the
base's - against the same linker script the release reads (`docs/release.md`, *The base*). Between
the SoftDevice and the bootloader everything is either the application's or the file system's,
and the base never touches it.

## 10. What is tested where

| | Native tests | Build only | Bench |
|---|---|---|---|
| link codec, session, retry memory | ✔ against the fixtures and fakes | | UART through the bulkheads |
| the console over the link, our settings, the attention machine | ✔ against fakes, and the `CLI`, `SLEEP_NOTICE` and `EV_ATTENTION` fixtures through the real session | upstream's interpreter behind it | every console command, and a mesh admin waking the box |
| clock rules, the boot order, the file's rate | ✔ against fakes, and the `TIME_SET` fixture through the real session | the flash file, the RTC probe | a reboot keeps time moving forward |
| shunt arithmetic, the two readers' schedules, the thresholds, the Cayenne-LPP | ✔ against a fake bus and a fake part, and the `POWER_GET`, `ENV_GET` and `LPP_GET` fixtures through the real session | the I2C transactions, the Adafruit BME280 driver, the preferences file | real currents, sign, calibration; a phone's telemetry view |
| job state, reply matching, the send ceiling | ✔ (the ceiling against a fake clock, and through a real session) | the overrides, the sends | a login to a real repeater |
| the observation ring, the pull's payload fitting, the RSSI note table, `obs.wake` | ✔ against fakes, and the `OBS_PULL` fixture through the real session | advert capture, the two overrides | adverts heard on the roof |
| the first-boot profile: the reply rule, the runner, that it runs once, and the table's own rails | ✔ against a fake command interface (`test/test_firstboot`), and the shipped table through the firmware's own session behind a pipe (`tools/linkloop/check.sh`, which also drives `linkcli.py provision`) | upstream's interpreter behind it, and the one preferences write | `erase`, reset, and `sensor get firstboot.state`; a provisioning session at the port and one over the link |
| the descriptor builder, the names across `platformio.ini` and its consumers, the band table | ✔ against the specification's fixtures and against `platformio.ini` (`test/test_image`), and every `[img_*]` section through the release tooling (`tools/release/test_release.py`) | the `static_assert`s of `src/common/bands/BandCheck.cpp` and `src/common/fwdesc/ImageDescriptor.cpp`; the post-build check of `tools/pio/descriptor.py`, which finds the descriptor in the linked image | the Walter reads it out of what it downloaded, and holds `HELLO` against it |

## 11. Proved by task p1-01

What §2 and §3 rest on, checked against the pinned checkout (`d9296435`, `repeater-v1.17.1`) and
against the build, by the task that first compiled our application onto upstream's repeater
(`tasks/phase-1/reports/01.md`), and **re-read at `81dbae59` (`repeater-v1.18.0`) by p3.5-06**:
every claim held, and the line numbers are that pin's now.

| Claim | Evidence |
|---|---|
| `build_as_lib.py` compiles `examples/<name>/*.cpp` for `BUILD_EXAMPLE` and leaves one file out for `EXCLUDE_FROM_EXAMPLE` | `build_as_lib.py` lines 44–55: `+<../examples/{example_name}/*.cpp>` then `-<../examples/{example_name}/{exclude_name}>`; the exclude needs `example_name` already set, so the define order matters. The linker map (`out/build/<env>/firmware.map`) has `libMeshCore.a(MyMesh.cpp.o)` and no `main.cpp.o` member of that archive; `setup` and `loop` are in `src/roles/repeater/main.cpp.o` |
| `MC_VARIANT` compiles the whole variant directory, `target.cpp` included | `build_as_lib.py` lines 39–42: `+<../variants/{variant_name}>`. Without the define the map has exactly one `target.cpp.o`, ours, and `RAK4631Board.cpp.o` / `variant.cpp.o` in `libmeshcore_variant.a` |
| `MyMesh : public mesh::Mesh, public CommonCLICallbacks`, most state private | `MyMesh.h`: the members from `FILESYSTEM* _fs` to `matching_peer_indexes[MAX_CLIENTS]` precede the first access specifier |
| `handleCommand(0, command, reply)` is public | `MyMesh.h`, under `public:`: `void handleCommand(uint32_t sender_timestamp, char* command, char* reply);`. `sender_timestamp == 0` is the console's privilege (`MyMesh.cpp:1264`, `get acl`) |
| `getNodePrefs()`, `savePrefs()` are public | `MyMesh.h`, under `public:`; `savePrefs()` calls `_cli.savePrefs(_fs)`, which rewrites `/prefs.json` (`CommonCLI.cpp:143`) |
| the settings seam is virtual and the CLI uses it | `SensorManager.h:21–24`: `getNumSettings`, `getSettingName`, `getSettingValue`, `setSettingValue`, all `virtual`; `CommonCLI.cpp:300–340`: `sensor get` (`getSettingByKey`), `sensor set` (`setSettingValue`), `sensor list` (the three by index). `TwinOakSensors` overrides the four with `override` and the build accepts it |
| telemetry goes through `sensors.querySensors(perm, lpp)` | `MyMesh.cpp:250`, in `handleRequest` for `REQ_TYPE_GET_TELEMETRY_DATA`; `querySensors` is `virtual` (`SensorManager.h:19`) |
| `getBattMilliVolts()` is virtual on the board | `MeshCore.h:47`: `virtual uint16_t getBattMilliVolts() = 0;` on `mesh::MainBoard`; `RAK4631Board.h` overrides it with the ADC reading; `MyMesh.cpp:218` and `:244` call it for `status` and the telemetry base channel |
| the eight overrides are virtual and reachable | `MyMesh.h`, under `protected:`: `searchPeersByHash`, `getPeerSharedSecret`, `onPeerDataRecv`, `onPeerPathRecv` (`override`), `onAdvertRecv` (no keyword; virtual via `Mesh.h:122`), `logRx`, `logTx` (`override`); `onTraceRecv` is not in `MyMesh.h` at all — the base is `Mesh.h:107`'s empty virtual. `src/roles/repeater/app/TwinOakMesh.h` declares all eight with `override` and the build is green; `nm` shows all eight `TwinOakMesh::` symbols beside the `MyMesh::` ones |
| `Mesh::onRecvPacket` tries matched peers in index order and the first MAC that verifies wins | `Mesh.cpp:147–186`: `searchPeersByHash(&src_hash)`, then `for (int j = 0; j < num; j++) { getPeerSharedSecret(secret, j); … MACThenDecrypt … if (len > 0) { … onPeerPathRecv(pkt, j, …) or onPeerDataRecv(pkt, type, j, …); found = true; break; } }`. Upstream's match array is `matching_peer_indexes[MAX_CLIENTS]` (32 since 1.18.0, when the duplicate
define left `MyMesh.h` and `ClientACL.h:45` became the one definition; 20 at 1.17.1), filled at
`MyMesh.cpp:623` |
| `onTraceRecv` is called for a direct TRACE at the end of its path | `Mesh.cpp:56–57`: `if (offset >= len) onTraceRecv(pkt, trace_tag, auth_code, flags, pkt->path, &pkt->payload[i], len)` |
| `logRx` sees the packet while its RSSI is still the radio's last | `Dispatcher.cpp:238`: `logRx(pkt, pkt->getRawLength(), score)` in the receive path, after `_radio->getLastSNR()`/`getLastRSSI()` were read for the score and before the packet is queued inbound; `logTx` at `Dispatcher.cpp:109`, when the outbound packet has been handed to the radio |
| `createAnonDatagram`, `createDatagram`, `createTrace`, `sendDirect`, `sendFlood` are public on `Mesh` | `Mesh.h:185–224`, under `public:` |
| "no saved preferences" is readable | `CommonCLI.cpp:30–47`, `loadPrefs`: `/prefs.json` is loaded if it exists, else `/com_prefs` is converted if it exists, else nothing is loaded and the constructor's defaults stand. `src/roles/repeater/app/FirstBoot.cpp` tests exactly those two files, before `begin()` runs `loadPrefs` |
| the admin password's field is 16 bytes | `CommonCLI.h:31`: `char password[16];` — fifteen characters and the NUL, which is what `StrHelper::strncpy` leaves (`TxtDataHelpers.cpp:3`) and what the login compares with `strcmp` (`MyMesh.cpp:102`). A sixteen-character password cannot exist at this pin; ours is fifteen |
| upstream's default password is the constructor's and only the constructor's | `MyMesh.cpp:32–34` defaults `ADMIN_PASSWORD` to the word every stock repeater ships with when the define is absent; `MyMesh.cpp:898` copies the define into `_prefs.password`. Our build defines it as `not-a-password` and `FirstBoot.cpp` overwrites it in `setup()`, before `loop()` runs |
| the radio's RNG | `RadioLibWrappers.h:88`: `RadioNoiseListener : mesh::RNG`, radio noise xor the CC310's generator under `USE_CC310_HW_CRYPTO`; `radio_new_identity()` draws the node's key from it and `radio_random()` the first-boot password |
| `-Werror` on `src/` survives upstream's headers | not by itself: under `-Wextra`, `MeshCore.h`, `Dispatcher.h`, the SoftDevice's `nrf_soc.h` and `RAK4631Board.h` warn (unused parameters, a reordered initialiser, a literal as `char*`), and RadioLib's `#warning` for GODMODE is an error under `-Werror=cpp`. `src/common/board/UpstreamHeaders.h` and `src/roles/repeater/app/UpstreamMyMesh.h` include upstream as system headers (`#pragma GCC system_header`, the agent firmware's `MeshCoreHeaders.h`), and `-Wno-error=cpp` keeps the `#warning` a warning. An `-isystem` flag was tried first and made the core's `WString.h` conflict with itself on this toolchain |
| upstream's tree is not written by the build | `git status --porcelain` in `.pio/libdeps/repeater_rak4630_single/MeshCore` is empty after the build, `HEAD` is the pin |

Re-read by **p3.5-06**, the first real MeshCore bump (`repeater-v1.17.1` → `repeater-v1.18.0`,
`d9296435` → `81dbae59`, 266 commits of upstream's). Every claim of this file held; what moved
under the citations, and is re-cited where it is used: the radio keys of the CLI (`get radio`,
`dutycycle`, `af`, `path.hash.mode`, `agc.reset.interval`, `tx`, the three delays, `int.thresh`,
`cad`, `radio.rxgain`, `multi.acks`) are `CommonRadioPrefs::handleCommand`
(`src/helpers/CommonRadioPrefs.cpp`), reached from `CommonCLI::handleCommand` before anything
else, with every reply text unchanged; `MAX_CLIENTS` is 32 (one definition, `ClientACL.h:45`);
`loop.detect` defaults to `minimal` on a new install (`MyMesh.cpp:911`), which the profile's
`moderate` overrides as before; a repeater whose local *and* flood advert intervals are 0 is a
hidden node and answers no DISCOVER (`MyMesh::isHiddenNode`, `MyMesh.cpp:793`) - never a unit of
ours, whose flood advert is 60-85 hours; `TXT_TYPE_CLI_COMMAND` (3) is accepted as a CLI line
beside `PLAIN` and `CLI_DATA` (`MyMesh.cpp:707`), replies still `CLI_DATA`; a transport code of 0
matches the wildcard region (`RegionMap.cpp:191`) and is forwarded under `flood.max`, not
`flood.max.unscoped`; `Utils::MACThenDecrypt` refuses a ciphertext that is not block-aligned
(`Utils.cpp:150-151`), which our sends - upstream's `encryptThenMAC` - always are; a forwarded
direct ACK is delayed like routed traffic (`Mesh.cpp:90-98`), and §7's priorities stand;
`mesh::MainBoard` gained `loop()` and `handleCommand()` (`MeshCore.h:88-92`), both no-ops on this
board, and upstream's `main.cpp` calls the first, so ours does too and carries its new
NUL-termination clamp. Two lines of upstream's own `platformio.ini` were deliberately not brought
across: the GCC 14 toolchain pin and the removal of `NRF52_POWER_MANAGEMENT` (`platformio.ini`
says why beside each). The application region did not move.

Additions made above by **p3.5-05**, the Danish standard: §4a (the profile is six lines and one
of them is derived from the node's key; what that cost the runner's interface) and §5's flood
advert, which is no longer "the 47-hour one". What that task read of upstream is cited where it is
used - `src/roles/repeater/firstboot/Profile.cpp` names the branch of `CommonCLI::handleSetCmd`
each of the four new lines goes through, upstream's default for each, and - for
`agc.reset.interval` - that the preference is **seconds / 4** in a `uint8_t`
(`CommonCLI.h:51`), which is why the reply is `OK - interval rounded to 4` and why the reply rule
has to accept it. `docs/upstream.md` place 5 lists the four keys for the next bump; at
`repeater-v1.18.0` the `agc.reset.interval` handler moved out of `CommonCLI.cpp` into
`CommonRadioPrefs.cpp` with the same arithmetic and an added `constrain(0, 1020)`, and p3.5-06
re-pointed the citations when it moved the pin (below).

Corrections and additions made above by **p3-03**, the first-boot profile: §1
(`src/roles/repeater/firstboot/`), §2 (one more row: `savePrefs()`, the **eleventh** override -
"the eight" of p1-01 became ten with the clock and is eleven now), §4 (the tenth setting, and what
the tenth one cost the first `sensor list` page), §4a (new: what a unit is before anybody has
provisioned it, and why once is exact) and §10. What that task read of upstream is cited where it
is used - `src/roles/repeater/firstboot/Profile.{h,cpp}` for the acceptance rule, which is read off
every reply `CommonCLI.cpp`'s `set` branch and `MyMesh.cpp` can produce, and for
`MyMesh::sendSelfAdvertisement`'s `path_hash_mode + 1`; `src/roles/repeater/app/TwinOakMesh.h` for
`MyMesh::savePrefs()` and `CommonCLI::savePrefs`'s remove-then-write of `/prefs.json` and its
zeroing of `advert_interval` - and `docs/upstream.md` place 5 lists them. One claim is worth
recording with the rest:

| Claim | Evidence |
|---|---|
| an empty guest password makes a **stranger's blank login succeed** at guest permission | `MyMesh::handleLoginReq`, `examples/simple_repeater/MyMesh.cpp:90-110` at the pin: a blank password (`data[0] == 0`) first looks the sender up in the ACL, and when it is not there falls through to `strcmp((char*)data, _prefs.guest_password)` - which an empty `guest_password` matches, so `perms = PERM_ACL_GUEST` and the client is added. `NodePrefs`' constructor leaves `guest_password[0] = 0` and nothing of ours sets it, so that is what a fresh unit does. `docs/provisioning.md` §1 and §2 say so, and setting one is a decision a person takes rather than a default |

Corrections and additions made above by **p3-02**, NoiseScope: §1 (`src/roles/scanner/`,
`tools/noisescope/` and the second header wrapper) and §8 (*what a role compiles of `common/`*,
which is new, and the two lists after it). Two claims about the pinned checkout were read for it
and are worth recording here with the rest:

| Claim | Evidence |
|---|---|
| the Adafruit core at this pin has no `WisCore_RAK4631_Board` variant, so **both** roles take the Arduino pin map out of MeshCore's tree | `~/.platformio/packages/framework-arduinoadafruitnrf52/variants/` at `d5413016` has thirteen variants and none of them is it; `boards/rak4631.json` names that variant, and `variant.h` resolves through `-I ${meshcore.dir}/variants/rak4631`. `tools/pio/meshcore_variant.py` compiles `variant.cpp` for every role of this board and `RAK4631Board.cpp` only for a role that compiles MeshCore |
| the SX1262 of the RAK4630 stamp is unpowered until `SX126X_POWER_EN` is driven high | `variants/rak4631/variant.h:155` defines it as pin 37 (P1.05) and `RAK4631Board.cpp:58-66` is `pinMode(SX126X_POWER_EN, OUTPUT)`, the boot-voltage check, `digitalWrite(..., HIGH)` and `delay(10)` with the comment "give sx1262 some time to power up"; `initiateShutdown` drives it low again. After a reset the GPIO is an input, so a firmware that never drives it leaves the radio unpowered — which NoiseScope did, and `src/roles/scanner/Radio.cpp` now does not (`tasks/phase-3/reports/02.md`) |

Corrections and additions made above by **p3-01**, roles and boards: §1 (the tree, which is now
`src/common/` and `src/roles/<role>/`, and the one seam between them), §8 (rewritten: an image's
three names and the one place they are written, the band blocks and their check, the descriptor
and the three things that check it, and the *adding a role* / *adding a board* lists) and §10.
Three couplings the split had to break are worth knowing, because they say what `common/` may
not do: `src/common/board/target.cpp` reached the clock keeper and the repeater's settings table
directly, and `src/common/power/BoardPower.cpp` reached the repeater's board link for
`raiseAttention` and upstream's `MyMesh.h` for `FILESYSTEM` — the first three are now the six
functions of `src/common/board/RoleHooks.h`, and the fourth is one more include in
`src/common/board/UpstreamHeaders.h`. No behaviour changed with them.

Corrections and additions made above by **p1-06**, power and environment: §1 (what is under
`src/common/power/`), §3 (`getBattMilliVolts()` is channel 2 now, and `TwinOakSensors` gained
`begin()`/`loop()`/`querySensors()`; **upstream's BME280 support had to leave the build** too, for
a reason this file did not foresee - it samples the part inside `querySensors`, which is on the
path of `LPP_GET`), §4 (the settings table is seven keys and `sensor list` pages), §6 (rewritten:
one cache and its four readers, why no request touches a bus, what the I2C layer cannot promise,
the thresholds' own rules, and the one field list with two appliers) and §10. What that task read
of upstream is cited where it is used - `src/common/power/Environment.h` for upstream's BME280 query,
`src/common/power/Telemetry.{h,cpp}` for `handleRequest`'s two fields and CayenneLPP's `addField`,
`src/common/board/target.h` for what reads `getBattMilliVolts()`, `src/common/power/BoardPower.cpp` for the
core's TWIM driver - and `docs/upstream.md` place 5 lists them.

Corrections and additions made above by **p1-05**, local management: §1 (`src/roles/repeater/app/Settings.*` and
`src/common/link/attention/`), §4 (what `CLI` does and what it costs; the attention machine; the settings
table gained `fw.version` and the order `sensor list` really prints, and who may run `sensor set`
over the mesh, from the source at the pin) and §10. What that task read of upstream is cited where
it is used — `src/roles/repeater/link/BoardLink.cpp` for `handleCommand`, the reply window and the four commands
that never return, `src/roles/repeater/app/Settings.{h,cpp}` for the ACL gate and `sensor list`'s 134-byte
cut-off — and `docs/upstream.md` place 5 lists them.

Corrections made above by **p1-04**, the clock: §1 (what is under `src/roles/repeater/clock/`), §2 (two more
rows of the table - the virtuals the clock keeper needs, which p1-01 did not know about - and "the
eight" is now ten), §5 (rewritten: the file is the only memory there is, and why; the *clean*
flag; the sane window; the jump poll; which virtuals count as a timestamp leaving the node) and
§10. What that task read of upstream is cited where it is used - `src/roles/repeater/clock/BoardClock.cpp` for
the clock helpers and the reset reason, `src/roles/repeater/app/TwinOakMesh.h` for the four virtuals and the
advert timers, `src/common/board/target.h` for `reboot()`/`powerOff()` - and `docs/upstream.md` place 5
lists them.

Corrections made above by that task: §1 (the wrapper headers), §2 (the define order, `UITask.cpp`,
`the_mesh`, `onAdvertRecv`'s missing keyword), §3 (`TwinOakBoard` and the virtual base; the
battery reading stayed upstream's until task 06; `variant.cpp` is the only pin map, not one of
two; `MC_VARIANT` refused; how `<target.h>` resolves). Phase 0's report said the core compiled its
own `WisCore_RAK4631_Board/variant.cpp` beside MeshCore's; it does not, there is no such
directory at the pin.
