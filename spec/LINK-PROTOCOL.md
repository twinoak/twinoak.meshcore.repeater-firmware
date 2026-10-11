# The box link — protocol 1.0

The UART between the LoRa box and the LTE box of a TwinOak standard repeater v2: the RAK4630 on one
end, the Walter on the other. This document is the specification. `tools/linkref.py` is the same
thing as code, and `fixtures/link/link-v1.json` is what it generates; both firmware repositories
carry a copy of the fixtures and test their codecs against every case in it.

**Status:** 1.0, written 2026-10-11, before either firmware exists. It changes only here, and a
change is a new minor or major version with new fixtures — never an edit in one firmware.

Words used: **RAK** is the radio firmware in the LoRa box (a repeater, later a bridge), **Walter**
is the management firmware in the LTE box. *Must* and *never* are requirements; everything else is
description.

---

## 1. The wire

| | |
|---|---|
| Electrical | `hardware/pcb/INTERCONNECT.md`: D-sub pins 8 and 9, push-pull 3.3 V, 470 Ω in series and about 2.2 nF to ground on each line (two bulkhead filters). **Frozen.** |
| RAK side | `Serial1` — P0.15 RX, P0.16 TX (adapter jumpers J4/J5 on 1–2) |
| Walter side | GPIO7 TX, GPIO8 RX |
| Format | 115200 baud, 8 data bits, no parity, 1 stop bit, no flow control |
| Idle | high. GPIO8 is an RTC-capable pin on the ESP32-S3, so a start bit can wake a sleeping Walter (§8) |

Either end may be unpowered, resetting, or running other firmware while the other is alive. The
Walter tristates GPIO7 before it kills the LoRa rail and whenever it believes the RAK is unpowered
(`INTERCONNECT.md`, "Kill line behaviour"). Neither end may block waiting for the other: the RAK
repeats with nobody on the link, and the Walter can recover a RAK that says nothing.

## 2. Frames

A **packet** is

```
type : u8      what this is (§4)
seq  : u8      which one (§3)
payload : 0..240 bytes
crc  : u16     little-endian, over type, seq and payload
```

so a packet is 4 to 244 bytes. The CRC is **CRC-16/CCITT-FALSE**: polynomial `0x1021`, initial value
`0xFFFF`, no reflection, no final XOR. Its check value over the ASCII string `123456789` is `0x29B1`.

A **frame** is the packet encoded with **COBS** (consistent overhead byte stuffing) followed by one
`0x00`. COBS removes every zero from the packet, so `0x00` appears on the wire only as the frame
terminator. Because a packet is at most 244 bytes, no run inside it reaches 254 bytes: the overhead is
exactly one byte, and the `0xFF` code of general COBS never occurs. An encoder need not implement
it; a decoder that meets it refuses the frame.

Rules for a sender:

- If the line has been idle for 100 ms or more, send one `0x00` before the frame. It ends whatever
  garbage the receiver may hold and costs nothing (an empty frame is not an error).
- Send a frame in one piece. Do not interleave anything inside it.

Rules for a receiver:

- Collect bytes until `0x00`. Zero bytes collected: ignore. Otherwise decode COBS, check the length
  (4..244) and the CRC. A frame that fails any of these is **dropped and counted**, and nothing is
  sent in reply — the sender's timeout is the recovery.
- A receive buffer needs 246 bytes. A frame that overflows it is dropped and counted when its
  terminator arrives.
- Never interpret bytes of a frame before its CRC has passed.

All multi-byte integers in payloads are **little-endian**. `i8`, `i16` are two's complement. A
`str8` is a `u8` length followed by that many bytes of UTF-8, no terminator. *rest* means every
remaining byte of the payload.

## 3. Requests, responses, events

| `type` | Kind | Direction in 1.0 |
|---|---|---|
| `0x01`–`0x5F` | **request** | Walter → RAK |
| `0x81`–`0xDF` | **response**: the request's type with bit 7 set, the request's `seq` | RAK → Walter |
| `0x60`–`0x7F` | **event**: no answer | RAK → Walter |

In 1.0 only the Walter sends requests and only the RAK sends events. The RAK never needs an answer
from the Walter to do its job.

**Responses.** Every response payload begins with `status : u8`. When `status` is not `OK` the
payload is that one byte and nothing else.

| `status` | Name | Meaning |
|---:|---|---|
| 0 | `OK` | done; the body follows |
| 1 | `ERR_UNSUPPORTED` | this firmware does not know the type, or lacks the capability |
| 2 | `ERR_MALFORMED` | the payload is too short or a field is not a legal value |
| 3 | `ERR_BUSY` | a mesh exchange is still waiting for its reply |
| 4 | `ERR_STATE` | not possible now (no job begun, radio not up, …) |
| 5 | `ERR_RANGE` | a value is outside what is accepted |
| 6 | `ERR_FAILED` | tried and failed (no packet buffer, I2C error, …) |
| 7 | `ERR_DENIED` | this role refuses it |

A request of an unknown type is answered `ERR_UNSUPPORTED` with that type's response code. That is
how a newer Walter discovers what an older RAK cannot do.

**Sequence numbers and retries.** The Walter picks `seq` for each new request, any value different
from the previous request's. It waits **1000 ms** for the response. On silence it sends **the same
frame again, same `seq`**, at most twice more; after three silences the link is down, and that is
the supervisor's business (§9).

The RAK remembers the `(type, seq)` of the last request it executed and the response it gave. A
request with the same `(type, seq)` is **not executed again**: the remembered response is sent
again. This is what makes a retry safe for the requests that transmit on the mesh — a lost response
must never become a second transmission. The memory is one entry and is forgotten on reboot (a
reboot also changes `boot_id`, §5.1, and the Walter starts over).

The RAK answers every request within **250 ms**. A request whose work takes longer (a mesh
exchange) is answered at once and its outcome arrives later as an event that can also be asked for
(§6.3).

**Events** carry their own `seq`, counted by the RAK from 0 and wrapping, so the Walter can see
that one was lost. An event is never the only way to learn something: every event's content can
also be fetched with a request.

## 4. Message table

| `type` | Name | Capability | Section |
|---:|---|---|---|
| `0x01` | `HELLO` | always | 5.1 |
| `0x02` | `PING` | always | 5.2 |
| `0x03` | `STATUS` | always | 5.3 |
| `0x04` | `TIME_SET` | always | 5.4 |
| `0x05` | `TIME_GET` | always | 5.4 |
| `0x06` | `REBOOT` | always | 5.5 |
| `0x07` | `IDENTITY` | always | 5.6 |
| `0x08` | `SLEEP_NOTICE` | always | 8 |
| `0x09` | `CLI` | `CLI` | 5.7 |
| `0x0A` | `POWER_GET` | `POWER` | 5.8 |
| `0x0B` | `ENV_GET` | `ENV` | 5.8 |
| `0x0C` | `LPP_GET` | `CLI` | 5.8 |
| `0x10`–`0x1F` | *reserved: provisioning* | | |
| `0x20` | `JOB_BEGIN` | `MESH_CLIENT` | 6.1 |
| `0x21` | `JOB_END` | `MESH_CLIENT` | 6.1 |
| `0x22` | `PATH_SET` | `MESH_CLIENT` | 6.1 |
| `0x23` | `PATH_GET` | `MESH_CLIENT` | 6.1 |
| `0x24` | `SEND_LOGIN` | `MESH_CLIENT` | 6.2 |
| `0x25` | `SEND_REQUEST` | `MESH_CLIENT` | 6.2 |
| `0x26` | `SEND_CLI` | `MESH_CLIENT` | 6.2 |
| `0x27` | `SEND_DISCOVERY` | `MESH_CLIENT` | 6.2 |
| `0x28` | `SEND_TRACE` | `MESH_CLIENT` | 6.2 |
| `0x29` | `REPLY_GET` | `MESH_CLIENT` | 6.3 |
| `0x2A` | `OBS_PULL` | `OBSERVATIONS` | 7 |
| `0x2B` | `MESH_COUNTERS` | `MESH_CLIENT` | 6.4 |
| `0x30`–`0x3F` | *reserved: bridge role* | | |
| `0x60` | `EV_BOOT` | always | 5.1 |
| `0x61` | `EV_MESH_REPLY` | `MESH_CLIENT` | 6.3 |
| `0x62` | `EV_ATTENTION` | `ATTENTION` | 8 |

## 5. Session and local management

### 5.1 `HELLO` and `EV_BOOT`

`HELLO` is the first request of a session and may be repeated at any time.

Request:

| Field | Type | |
|---|---|---|
| `proto_major` | u8 | 1 |
| `proto_minor` | u8 | 0 |
| `peer_kind` | u8 | 1 = Walter firmware, 2 = a bench tool |
| `max_payload` | u8 | the largest payload the sender accepts; 240 |
| `fw_version` | str8 | the sender's own version string |

Response body (after `status`):

| Field | Type | |
|---|---|---|
| `proto_major` | u8 | the version the RAK will speak in this session |
| `proto_minor` | u8 | |
| `role` | u8 | 0 unknown, 1 repeater, 2 bridge, 3 scanner |
| `slot` | u8 | 0 on a single-radio unit; the radio's position on a dual adapter |
| `max_payload` | u8 | the largest payload the RAK accepts; 240 |
| `caps` | u32 | capability bits, below |
| `boot_id` | u32 | random, drawn once per boot |
| `fw_version` | str8 | e.g. `0.3.0+abc1234` |
| `board` | str8 | e.g. `rak4630-single` |
| `upstream` | str8 | what it is built on, e.g. `repeater-v1.17.1+d9296435` |

Capability bits: `0x01 CLI`, `0x02 POWER`, `0x04 ENV`, `0x08 MESH_CLIENT`, `0x10 OBSERVATIONS`,
`0x20 ATTENTION`, `0x40 TIME_PERSIST`. Unknown bits are ignored.

**Versions.** A RAK that speaks the request's major answers with the highest minor both ends know.
A RAK that does not speak that major answers `ERR_UNSUPPORTED`, and the Walter may try another
major it knows. Within a major, a minor adds things — new types, new capability bits, new fields
*at the end* of a payload — and never changes what exists. A receiver ignores trailing bytes it does
not know.

A changed `boot_id` means the RAK restarted: every piece of per-boot state the Walter holds about it
(a job, a pending reply, the time it set) is gone.

**`EV_BOOT`** is sent once, when the RAK's link comes up after a reset. Its payload is
`reset_reason : u8` (the codes of §5.3) followed by the `HELLO` response body without the status
byte. It saves the Walter a round trip and tells a sleeping Walter that the RAK restarted. The
Walter does not rely on it: it sends `HELLO` when it wants to know.

### 5.2 `PING`

Request `nonce : u32`. Response `nonce : u32` (echoed), `uptime_s : u32`. The supervisor's
heartbeat: it proves the firmware's main loop is turning, which a reset line cannot.

### 5.3 `STATUS`

Request: empty. Response body:

| Field | Type | |
|---|---|---|
| `uptime_s` | u32 | seconds since boot |
| `reset_reason` | u8 | 0 unknown, 1 power-on, 2 reset pin, 3 watchdog, 4 software, 5 lockup, 6 wake from system-off, 7 left the bootloader |
| `time_state` | u8 | §5.4 |
| `epoch` | u32 | the RAK's clock, Unix seconds |
| `flags` | u16 | bit 0 a job is begun · bit 1 a reply is awaited · bit 2 the Walter is believed asleep · bit 3 adverts are held back until the clock is set · bit 4 the radio initialised |
| `obs_pending` | u8 | observations waiting in the ring (§7) |
| `obs_dropped` | u16 | observations lost to a full ring since boot, saturating |
| `link_errors` | u16 | frames dropped by the RAK's receiver since boot, saturating |
| `attention` | u8 | the reason of a pending attention (§8), or 0 |

### 5.4 The clock: `TIME_SET`, `TIME_GET`

The first-batch boards have no RTC fitted. The RAK's clock is therefore lost at every reset and
every kill cycle, and **the Walter sets it after every `EV_BOOT` and every `HELLO` whose `boot_id`
it has not seen** — before anything else it asks the RAK to do.

`time_state`:

| | | |
|---:|---|---|
| 0 | unset | the firmware's built-in default; never right |
| 1 | restored | carried over from before the reset: never earlier than anything this node has already sent, but not the real time — off by however long the node was down, or by the margin it added to stay ahead of its own past |
| 2 | set | set over this link since boot |
| 3 | rtc | read from a hardware RTC |

`TIME_SET` request: `epoch : u32`, `source : u8` (1 the cellular network, 2 the server, 3 a person
at a console), `flags : u8` (bit 0 = *force*). Response: `prev_epoch : u32`, `epoch : u32`,
`time_state : u8`.

A MeshCore node judges replays by timestamps, so a clock that goes backwards makes its own
adverts and requests look like replays to everyone who heard the later ones. The RAK therefore
refuses (`ERR_RANGE`) an `epoch` earlier than its current clock **unless** *force* is set, or its
`time_state` is 0 or 1 **and it has sent nothing that carries a timestamp since it booted**. That
second case is the normal one: the RAK holds its boot advert until the clock is set, so a Walter
that sets the time promptly may move a restored clock either way. *Force* is for a clock that was
set wrong; the Walter uses it only on a person's instruction.

`TIME_GET` request: empty. Response: `epoch : u32`, `time_state : u8`.

A RAK with `TIME_PERSIST` holds its boot advert back until its clock is set or a timeout passes
(the timeout is the firmware's, and `STATUS` flag bit 3 says the hold is on).

### 5.5 `REBOOT`

Request: `mode : u8` (0 restart the application, 1 UF2 bootloader, 2 serial DFU, 3 over-the-air
DFU), `delay_ms : u16`. The response is sent first; the restart follows after the delay, which
leaves time for the response to leave the UART. A mode the board cannot do is `ERR_RANGE`.

Modes 1–3 hand the radio to a bootloader that this link cannot talk to. They exist for a person at
the field USB port. The Walter's own way to replace firmware is SWD, never these.

### 5.6 `IDENTITY`

Request: empty. Response: `pubkey : 32 bytes`, `name : str8`. The node's MeshCore public key and
advert name.

### 5.7 `CLI`

Request: one command line, 1 to 160 bytes of UTF-8, no terminator. Response: the reply text, 0 to
160 bytes.

The line is run by the firmware's own command interpreter **as a command typed on its serial
console** — so everything the console can do, this can do, including the commands the firmware
refuses over the mesh (`stats-core`, `get prv.key`, `set freq`, `erase`). The Walter is trusted with
that: it already holds the SWD lines.

Some commands restart the node and never answer (`reboot`); the Walter treats silence after such a
line as the expected outcome, not as a dead link.

### 5.8 Telemetry: `POWER_GET`, `ENV_GET`, `LPP_GET`

`POWER_GET` — request empty; response:

| Field | Type | |
|---|---|---|
| `valid` | u8 | bit 0 channel 1, bit 1 channel 2, bit 2 channel 3 read successfully |
| 3 × `bus_mv` | u16 | bus voltage in millivolts |
| 3 × `current_ma` | i16 | current in milliamps, the board's per-channel shunt already applied |

Each channel is `bus_mv` then `current_ma`; the three channels follow one another. On the standard
B-board: channel 1 = panel (positive = into the charger), channel 2 = battery (**positive =
charging, negative = discharging**), channel 3 = total load (positive = drawn). The shunts are
100 mΩ, 20 mΩ and 50 mΩ (`hardware/pcb/CURRENT-SENSE.md`), and applying them is the RAK's job, so
the numbers on this link are already amps and volts. A channel whose bit is clear reads 0.

`ENV_GET` — request empty; response: `valid : u8` (bit 0 temperature, bit 1 humidity, bit 2
pressure), `temp_centi : i16` (°C × 100), `rh_centi : u16` (% × 100), `pressure_pa : u32`.

`LPP_GET` — request `perm_mask : u8` (MeshCore's telemetry permission bits: `0x01` base, `0x02`
location, `0x04` environment); response: the Cayenne-LPP bytes the node would answer a mesh
telemetry request with at those permissions. It is how the Walter reports exactly what a mesh
client would have been told.

## 6. The mesh client

A unit is one MeshCore node with one identity. With `MESH_CLIENT`, the RAK lends that identity to
the Walter for the handful of exchanges an exit node makes with *another* repeater: a login, a
request, a CLI line, a path discovery, a trace. The messages mirror, one for one, the calls of
`AgentMesh` in `twinoak/meshmaster-agent-firmware` (`src/mesh/AgentMesh.h`), because the Walter is
meant to run that firmware's job logic on top of them.

The rules of that firmware hold here too, and the RAK enforces them:

- **One peer at a time, one exchange at a time.** Between `JOB_BEGIN` and `JOB_END` there is one
  peer. While a reply is awaited, a second send is `ERR_BUSY`.
- **One packet per send.** A `SEND_*` puts exactly one packet on the air, or none. Nothing is
  retried by the RAK. A link-level retry of the same request (§3) transmits nothing.
- **Nothing from a stranger is decrypted for the client.** Only while a reply is awaited, and only
  from the job's peer.
- **A password is in RAM for the length of the send.** `SEND_LOGIN` and a `SEND_CLI` flagged
  sensitive are wiped from every buffer the RAK copied them to as soon as the packet is built. They
  are never stored and never printed.
- **Floods the client originates use two-byte path hashes**, never one.
- Being a repeater comes first: nothing here may hold up forwarding.

### 6.1 The job and its path

A **path** on this link is:

| Field | Type | |
|---|---|---|
| `path_mode` | u8 | 0 none: the next send floods · 1 direct: zero hops, sent direct with an empty path · 2 hops |
| `hash_size` | u8 | bytes per hop, 1–3; 0 unless `path_mode` is 2 |
| `hop_count` | u8 | 1–63; 0 unless `path_mode` is 2 |
| `hashes` | `hash_size × hop_count` bytes, at most 64 | the hops, outward order; absent unless `path_mode` is 2 |

`JOB_BEGIN` — request: `pubkey : 32 bytes`, then a path. It ends any job in progress first, then
makes this key the peer. Response: status only.

`JOB_END` — request empty. Forgets the peer, the path and any awaited or held reply, and wipes the
shared secret. Response: status only. The RAK also ends a job by itself when **120 seconds** pass
with no request on the link while a job is begun: a Walter that crashed must not leave a peer open.

`PATH_SET` — request: a path. Replaces the job's path (mode 0 forgets it). `ERR_STATE` with no job.

`PATH_GET` — request empty. Response: the path the job holds now. A reply that came back as a
returned path changes it (§6.3), so this is how the Walter reads the route a flood discovered.

### 6.2 The five sends

Every send request begins with `timeout_ms : u32`, how long the RAK waits for the reply, and every
send is answered at once with:

| Field | Type | |
|---|---|---|
| `send` | u8 | 0 nothing was sent · 1 sent as a flood · 2 sent direct |
| `tag` | u32 | what the reply will be matched by; 0 means "the next reply from the peer" |
| `sender_ts` | u32 | the timestamp the RAK put in the packet |

`timeout_ms` of 0 means nothing is awaited. Otherwise the RAK is *awaiting* from the moment the
packet is queued until a matching reply arrives or the time is up, and it then reports the outcome
exactly once (§6.3). Sending and starting to wait are one step, so no reply can slip between them.

All timestamps the client sends come from the RAK's clock through MeshCore's unique-time source,
which never hands out the same second twice. The peer judges replays by them, and a repeater also
uses that source for its own traffic, so the Walter never supplies one.

| Request | After `timeout_ms` | What goes on the air | `tag` |
|---|---|---|---|
| `SEND_LOGIN` | `password` : rest, 0–15 bytes | an anonymous request carrying the timestamp and the password. With no password it is the ACL login: the node's own key and nothing else | 0 — a login reply carries the peer's timestamp, not ours |
| `SEND_REQUEST` | `req_type : u8`, `data` : rest | a request datagram: timestamp, type, data | the timestamp |
| `SEND_CLI` | `flags : u8` (bit 0 sensitive), `text` : rest, 1–148 bytes | a text datagram of CLI type: timestamp, flags byte, the line | 0 — a CLI reply carries no tag |
| `SEND_DISCOVERY` | — | the path-discovery request (a base-telemetry request), **always flooded** | the timestamp |
| `SEND_TRACE` | `hash_size : u8` (1, 2, 4 or 8), `hashes` : rest | a trace along the given round-trip path, always direct | the timestamp |

`SEND_LOGIN`, `SEND_REQUEST` and `SEND_CLI` go direct when the job holds a path (modes 1 and 2) and
flood otherwise. `SEND_TRACE` needs no job; the others answer `ERR_STATE` without one. A trace
cannot express three-byte hashes; `hash_size` 3 is `ERR_RANGE`.

What the RAK awaits depends on the send: a login, a request and a discovery await a **response**
(alone, or riding in a returned path); a CLI line awaits a **text**; a trace awaits a **trace**.
A response that arrives while a text is awaited, a response with another tag, and anything from
anyone but the peer are dropped unread.

### 6.3 The reply: `EV_MESH_REPLY` and `REPLY_GET`

A **reply record**:

| Field | Type | |
|---|---|---|
| `state` | u8 | 0 none · 1 pending · 2 timeout · 3 response · 4 text · 5 trace |
| `tag` | u32 | the awaited tag (or the reply's own, for state 3 and 5) |
| `via_path` | u8 | 1 when the response came inside a returned path |
| `snr_q4` | i8 | the reply's SNR at this node, in quarter-decibels |
| `rssi` | i16 | its RSSI in dBm, or `0x7FFF` when not known |
| `elapsed_ms` | u32 | from the send to the reply (or to the timeout) |
| state 3 | `data` : rest | the response after its four tag bytes |
| state 4 | `text` : rest | the CLI reply's text |
| state 5 | `trace_flags : u8`, `final_snr_q4 : i8`, `hash_bytes : u8`, `hashes`, `snr_count : u8`, `snrs` | as MeshCore delivers a completed trace |

When an awaited reply arrives or times out, the RAK sends **one** `EV_MESH_REPLY` whose payload is
the record (state 2–5), and keeps the record until the next send, `JOB_BEGIN` or `JOB_END`.

`REPLY_GET` — request empty; response: the record as it stands. State 1 while still waiting, 0 when
nothing is held. The Walter runs its own timer of `timeout_ms` plus a margin and asks with
`REPLY_GET` if the event did not come: a lost event costs one request, never a lost reply.

**A returned path is learned.** When the reply to a flooded send comes back as a returned path, the
job's path becomes that path before the event is sent; the rest of the job goes direct, and
`PATH_GET` reads it.

### 6.4 `MESH_COUNTERS`

Request empty. Response: `rx : u32`, `tx : u32` (every packet this node transmitted, forwards
included), `client_tx : u32` (packets the mesh client originated — **one per send, and nothing
else**; an exit node's transaction count is held against it), `adverts : u32` (adverts that
verified), `adverts_dropped : u32`, `obs_dropped : u32`, `noise_floor : i16` (dBm), `last_rssi :
i16`, `last_snr_q4 : i8`.

## 7. Observations: `OBS_PULL`

With `OBSERVATIONS`, the RAK keeps the adverts it hears in a ring of at least 32 records, newest
overwriting oldest when full (and counting the loss). An advert is recorded after its signature
verified. The Walter collects them when it is awake; nothing is pushed.

Request: `max : u8`, the most records wanted. Response: `remaining : u8` (still in the ring after
this answer), `dropped : u16` (lost since the last `OBS_PULL`), `count : u8`, then `count` records,
oldest first. The RAK returns as many as fit in one payload and no more than `max`. Records handed
out are removed from the ring.

Because a response can be repeated (§3) and the records are gone from the ring once sent, the
remembered response of a pull is what a retry gets — the same records, not the next ones.

One record:

| Field | Type | |
|---|---|---|
| `pubkey` | 32 bytes | the advertising node |
| `adv_ts` | u32 | the timestamp inside the advert |
| `age_s` | u32 | seconds since this node heard it — measured, so it is right whatever the clock said |
| `snr_q4` | i8 | quarter-decibels |
| `rssi` | i16 | dBm, or `0x7FFF` |
| `path_len` | u8 | MeshCore's encoded path length: bits 7–6 are the hash size minus one, bits 5–0 the hop count |
| `path` | (hash size) × (hop count) bytes | the path the advert arrived by; empty for a direct neighbour |
| `app_len` | u8 | 0–32 |
| `app_data` | `app_len` bytes | the advert's application data, untouched: flags, position, name |

The name inside `app_data` is something a stranger chose. Neither end logs it.

## 8. A Walter that sleeps: `SLEEP_NOTICE` and `EV_ATTENTION`

The Walter is powered from the same battery and spends most of its life asleep.

`SLEEP_NOTICE` — request: `expect_back_s : u32` (0 = unknown). The Walter sends it as its last
request before sleeping. From then until the next valid request arrives, the RAK believes the
Walter is asleep (`STATUS` flag bit 2): it sends **no** events except `EV_BOOT` and
`EV_ATTENTION`.

`EV_ATTENTION` — payload: `reason : u8`, `detail : u16`, `repeat : u8`.

| `reason` | | `detail` |
|---:|---|---|
| 1 | the node restarted | the reset reason |
| 2 | battery low | battery millivolts |
| 3 | battery critical | battery millivolts |
| 4 | a person asked for the Walter (a console or mesh admin command) | 0 |
| 5 | the observation ring is nearly full | records waiting |
| 6 | a fault the node wants reported | a code of the firmware's own |

To raise an attention the RAK sends `0x00 0x00`, waits 300 ms, and sends the event; then again
every **1000 ms**, with `repeat` counting from 0, until any valid request arrives or it has sent it
**10** times. The first bytes wake the Walter (a start bit on GPIO8); a Walter coming out of deep
sleep needs a few hundred milliseconds before its UART listens, which is why the event is repeated
and why losing the first ones is expected. A pending attention stays visible in `STATUS` until the
Walter has made a request, so a Walter that woke for its own reasons sees it too.

Attention is rate-limited by the RAK — at most one new attention per reason per hour — because each
one costs the battery an LTE attach.

## 9. What each end does when the other is not there

**The RAK**, with nobody answering: it repeats, exactly as before. It keeps observations in the
ring (dropping the oldest), keeps a pending attention, and never retries anything on the link.

**The Walter**, when three attempts at a request go unanswered: the link is down. Whether that
means "the RAK is running other firmware", "the RAK is hung" or "the RAK is unpowered" is for the
supervisor to work out with the reset line, the kill line and SWD — in that order, slowly, and
within daily limits (`walter-firmware`, the supervisor).

**Telling the firmware apart.** NoiseScope 0.1 speaks NDJSON lines on this same UART, not frames.
A Walter that gets no `HELLO` response but sees lines beginning with `{` — or gets one in answer to
`info\n` — is talking to a scanner and treats the UART as text until the next reset of the RAK.
Text is never mixed with frames in one session.

## 10. Timing summary

| | |
|---|---|
| RAK answers a request within | 250 ms |
| Walter waits for a response | 1000 ms, then resends; 3 attempts |
| Idle before a sender prefixes `0x00` | 100 ms |
| Job ended by the RAK after link silence | 120 s |
| Attention repeat | every 1000 ms, 10 times |
| Spacing of two resets of the RAK by the Walter | ≥ 1 s (two inside 0.5 s enter the UF2 bootloader) |

## 11. The fixtures

`fixtures/link/link-v1.json`, generated by `tools/linkref.py gen`:

- `crc16` — data and its CRC.
- `cobs` — raw bytes and their encoding.
- `cases` — one per message shape: `kind`, `type`, `wire_type` (with the response bit), `seq`, the
  `fields` as a JSON object (byte strings as lowercase hex, text as text), and the resulting
  `payload`, `packet` and `frame` as hex. An implementation must produce `payload` from `fields`,
  `fields` from `payload`, and `(wire_type, seq, payload)` from `frame`.
- `invalid` — frames a receiver must refuse, with the reason the reference gives. The reason is
  informative: a receiver has to drop and count the frame, not agree on the word.
- `stream` — a byte stream with noise in it, the packets a receiver must deliver from it, and how
  many bad frames it must count.

A firmware's codec is correct when every case passes. The semantics above the codec — what a
request *does* — are tested in each repository against its own fakes.
