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
  app/                  TwinOakMesh (the subclass), first-boot provisioning, the version string
  board/                target.h / target.cpp for our adapter: the board object, the radio, the clock, the sensors;
                        UpstreamHeaders.h, the one place upstream's headers are included from (section 11)
  link/                 the box link: codec (portable), session and dispatcher (portable), UART transport
  clock/                the clock keeper (portable) and its flash file
  power/                INA3221 with per-channel shunts, BME280, thresholds (arithmetic portable)
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
| originate a packet | `createAnonDatagram`, `createDatagram`, `createTrace`, `sendDirect`, `sendFlood` | public on `Mesh` |

`TwinOakMesh : public MyMesh` overrides the protected virtuals, calls the base for everything that
is not ours, and adds the link. `main.cpp` constructs it where upstream's constructs `MyMesh`.
Nothing of upstream's names `the_mesh` but its own `main.cpp`, so the type of ours is nobody's
business but ours. One of the eight, `onAdvertRecv`, is declared in `MyMesh.h` without the
`override` keyword; it is virtual all the same, through `mesh::Mesh`, and ours says `override`.

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
  with `getBattMilliVolts()` returning channel 2's bus voltage from task 06 on - until then it is
  upstream's ADC reading of a pin that is not wired. `NRF52Board` is a *virtual* base of
  `RAK4631Board` (through `NRF52BoardDCDC`), so the most-derived class is the one that constructs
  it, with the OTA name upstream passes; a board added in phase 3 does the same.
- `radio_driver`, `radio_init()`, `radio_new_identity()` — as upstream's.
- `rtc_clock` — upstream's `AutoDiscoverRTCClock` over a volatile clock, so a hand-fitted RV-3028
  is found by itself; the clock keeper (§5) sits above it.
- `sensors` — `TwinOakSensors : EnvironmentSensorManager`: upstream's manager finds the BME280 at
  `0x76` by itself (`ENV_INCLUDE_BME280`); **its INA3221 support is left out of the build**
  (`ENV_INCLUDE_INA3221` undefined) because it applies one shunt value to all three channels, and
  ours are 100 mΩ, 20 mΩ and 50 mΩ. Ours reads the INA3221 itself (§6) and adds the settings seam
  (§4): our keys follow upstream's own (none on this board, since there is no GPS) in
  `sensor list`, `sensor get` and `sensor set`; `fw.version` is the first and is read-only.

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

Two more ways in, both through `TwinOakSensors`' settings, so they work from the USB console and
from a mesh admin's `sensor get/set`:

| Key | `get` | `set` |
|---|---|---|
| `walter.wake` | `0`, or `1` while an attention is pending | `1` raises attention reason 4 ("a person asked for the Walter") |
| `walter.seen` | seconds since the last valid request on the link, or `never` | — |
| `link.errors` | frames dropped by the receiver since boot | — |
| `clock.state` | `unset`, `restored`, `set` or `rtc` | — |

That first key is the out-of-band way to reach a unit whose Walter is asleep: any exit node that
reaches the repeater can ask it to wake its own uplink.

## 5. The clock keeper

Batch-1 boards have no RTC. Upstream's volatile clock starts every boot at a fixed date in 2024,
and a node whose clock runs backwards has its adverts and requests taken for replays by everyone
who heard the later ones.

The keeper (portable rules, a small flash file):

- keeps the newest time it has known twice: in RAM that survives a reset (a watchdog or the reset
  pin loses nothing), and in a file, rewritten at most once an hour and on a clean reboot (a power
  cut loses up to that hour);
- at boot, if no RTC answered, continues from the RAM value, or else from the file's value plus the
  save interval — **never earlier than anything this node has sent**, at the price of being up to
  an hour ahead (`time_state` *restored*);
- takes the time from the Walter (`TIME_SET`, *set*): forward always; backwards only while nothing
  with a timestamp has left the node since boot, or when told to force it, as the protocol says;
- **holds the boot advert** until the clock is *set* or *rtc*, or until a timeout — a repeater
  with no Walter must still announce itself, late and with the best time it has.

## 6. Power and environment

The INA3221 at `0x42` is read directly: shunt voltage and bus voltage per channel, 16-bit
registers, the shunt of each channel applied to its own reading (the arithmetic is the hardware
repository's `CURRENT-SENSE.md` and NoiseScope's `telemetry.cpp`, and is a portable function with
tests). The results feed four readers: the link's `POWER_GET`; the mesh telemetry reply; the
board's battery voltage; and the attention thresholds (battery low / critical, with hysteresis and
a rate limit, because every attention costs the battery an LTE attach).

A sensor that stops answering is a missing reading, never a stalled loop: every I2C transaction
has a timeout, and a channel that failed is marked invalid until it answers again.

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
- **Observations**: every advert that verified goes to upstream first (its neighbour table), then
  into a ring the Walter empties. RSSI is noted in `logRx`, when it still belongs to the packet —
  a flood can wait a second in the inbound queue before `onRecvPacket` sees it.

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
| clock rules | ✔ | the flash file | a reboot keeps time moving forward |
| shunt arithmetic, thresholds | ✔ | I2C reads | real currents, sign, calibration |
| job state, reply matching | ✔ | the overrides, the sends | a login to a real repeater |
| observation ring | ✔ | advert capture | adverts heard on the roof |
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

Corrections made above by that task: §1 (the wrapper headers), §2 (the define order, `UITask.cpp`,
`the_mesh`, `onAdvertRecv`'s missing keyword), §3 (`TwinOakBoard` and the virtual base; the
battery reading stays upstream's until task 06; `variant.cpp` is the only pin map, not one of
two; `MC_VARIANT` refused; how `<target.h>` resolves). Phase 0's report said the core compiled its
own `WisCore_RAK4631_Board/variant.cpp` beside MeshCore's; it does not, there is no such
directory at the pin.
