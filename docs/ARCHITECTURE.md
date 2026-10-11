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
  board/                target.h / target.cpp for our adapter: the board object, the radio, the clock, the sensors
  link/                 the box link: codec (portable), session and dispatcher (portable), UART transport
  clock/                the clock keeper (portable) and its flash file
  power/                INA3221 with per-channel shunts, BME280, thresholds (arithmetic portable)
  meshclient/           the job, the five sends, the reply, the observation ring (state portable)
  fwdesc/               the image descriptor
test/                   native tests, one directory per subject
spec/                   a copy of the system specification and its fixtures - never edited here
tools/                  verify.ps1, the release tooling, PlatformIO helper scripts
docs/                   this file, upstream.md, link.md, bench.md
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
only its `main.cpp` is replaced by ours.**

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

- `board` — a subclass of upstream's `RAK4631Board` (the SX1262 wiring inside the RAK4630 stamp is
  the stamp's, so the radio pins and the DC/DC setup are upstream's unchanged) with
  `getBattMilliVolts()` returning channel 2's bus voltage.
- `radio_driver`, `radio_init()`, `radio_new_identity()` — as upstream's.
- `rtc_clock` — upstream's `AutoDiscoverRTCClock` over a volatile clock, so a hand-fitted RV-3028
  is found by itself; the clock keeper (§5) sits above it.
- `sensors` — `TwinOakSensors : EnvironmentSensorManager`: upstream's manager finds the BME280 at
  `0x76` by itself (`ENV_INCLUDE_BME280`); **its INA3221 support is left out of the build**
  (`ENV_INCLUDE_INA3221` undefined) because it applies one shunt value to all three channels, and
  ours are 100 mΩ, 20 mΩ and 50 mΩ. Ours reads the INA3221 itself (§6) and adds the settings seam
  (§4).

Upstream's variant sources we still need (`RAK4631Board.cpp`, the Arduino `variant.cpp`) are
compiled from the pin by a small PlatformIO script, the way the agent firmware compiles
`lib/ed25519` — not copied. `boards/rak4631.json` is the one file that has to be vendored, for the
reason `boards/README.md` gives: PlatformIO reads `boards_dir` before it clones a dependency.

I2C runs at 100 kHz or below: one set of 1 k pull-ups, through a 1000 pF bulkhead filter
(`INTERCONNECT.md`).

## 4. The link, inside the firmware

`spec/LINK-PROTOCOL.md` is the protocol. In the firmware it is three layers:

1. **Codec** (portable): CRC, COBS, packets, and one encode/decode pair per message, tested against
   every case of `spec/fixtures/link/link-v1.json`.
2. **Session** (portable): a receiver that turns bytes into packets and counts what it drops; a
   dispatcher from request type to handler; the one-entry memory of the last request and its
   response, which is what makes a retried request harmless; the event counter. It talks to the
   rest of the firmware through interfaces — a clock, a CLI, power, the mesh client — so that the
   native tests run it against fakes.
3. **Transport**: `Serial1` at 115200. Read what is there, write what fits, never wait.

**The link never stalls the mesh.** `loop()` gives it a bounded slice: it drains the UART's receive
buffer, handles at most one request, and queues its answer. A handler that would take time — a
mesh exchange — returns at once and reports later (the protocol is built that way). A handler that
runs a console command inherits that command's cost, which is upstream's (a `set` writes
preferences to flash); that is the worst case and it is the same as typing the command on USB.

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
