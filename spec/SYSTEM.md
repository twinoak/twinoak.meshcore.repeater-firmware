# TwinOak standard repeater v2 — the software plan

The plan for the two firmwares of a repeater-v2 unit and how they meet MeshMaster. Written
2026-10-11, the day after the first boards were ordered. It is the map; the detail is in the
documents it names and in each firmware repository's `tasks/`.

## 1. What is being built, and where

| Repository | What it is | State on 2026-10-11 |
|---|---|---|
| `twinoak.meshcore.standard-repeater-design-v2` | the hardware, and **this specification** (`software/spec/`) | boards ordered, interfaces frozen |
| `twinoak.meshcore.repeater-firmware` | the firmware family for the LoRa box (RAK4630): the repeater, later a bridge, and NoiseScope | empty; piles written |
| `twinoak.meshcore.walter-firmware` | the firmware for the LTE box (Walter, ESP32-S3 + LTE-M) | empty; piles written |
| `twinoak/meshmaster.dk` | the backend: register, exit nodes, jobs, configuration | running; other agents at work — proposals only (§8) |
| `twinoak/meshmaster-agent-firmware` | the exit node that is one board (Heltec) | running; the model for how these two are built |

This directory holds what both firmwares must agree on, and nothing else:

- [`LINK-PROTOCOL.md`](LINK-PROTOCOL.md) — the UART between the two boxes, with fixtures.
- [`FIRMWARE-MANIFEST.md`](FIRMWARE-MANIFEST.md) — what a release says about itself and how it is
  signed, with fixtures.
- [`proposals/`](proposals/) — what the MeshMaster side needs, written down for the day those
  repositories are open for it.

Each firmware repository carries a **copy** of this directory under `spec/`, held byte for byte
against a manifest of hashes, exactly as the agent firmware carries meshmaster.dk's contract
fixtures. The copy is what a session reads and what CI tests against; this is where it is changed.

## 2. One unit

```
            LoRa box                                   LTE box
   ┌─────────────────────────┐   UART 115200   ┌──────────────────────────┐
   │ RAK4630                 │◄───────────────►│ Walter (ESP32-S3)        │──── LTE-M ───► meshmaster.dk
   │ repeater firmware       │   box link      │ walter firmware          │
   │  · MeshCore repeater    │                 │  · supervisor of the RAK │
   │  · the unit's identity  │   SWD, nRESET   │  · SWD flasher, images   │
   │  · mesh client for the  │◄────────────────│  · exit-node agent       │
   │    Walter               │   kill line     │  · check-in and sleep    │
   └───────────┬─────────────┘◄ ─ ─ ─ ─ ─ ─ ─ ─└──────────────────────────┘
               │ I2C (the RAK is the only master)
        INA3221 (panel / battery / load) · BME280 · (RTC, not fitted in batch 1)
```

**The repeater does not need the Walter.** It boots, repeats and answers mesh administration with
the LTE box asleep, dead or absent. **The Walter does not need the repeater's firmware.** With the
reset line, the kill line and SWD it can bring back a RAK whose application is gone. Neither
firmware waits for the other at boot. Everything else follows from those two sentences.

## 3. Decisions

Numbered `S-n` so that tasks and reports can cite them. A decision changes here first.

**S-1 · One node, one identity.** A unit is one MeshCore node. The repeater's key is the unit's
key, on the mesh and towards MeshMaster. When the unit acts as an exit node for its neighbours, it
logs in to them *as the repeater*. There is no second identity sharing the radio. *Why:* one radio
has one receive path and one transmit queue; a second identity would need a second `Mesh` instance
fed by a shim that MeshCore does not have and upstream would not keep working for us. What an exit
node needs of the mesh is five kinds of exchange with one peer at a time (S-4), and those can be
added to a repeater without touching its routing. *Cost:* MeshMaster sees one key as both a
repeater and an exit node's radio (`proposals/01`).

**S-2 · The repeater is upstream's, compiled unmodified.** MeshCore is a dependency pinned at one
commit, as in the agent firmware. Its `simple_repeater` example is compiled from that pin, not
copied; our application subclasses its `MyMesh` and replaces only `main.cpp` and the board's
`target.cpp`. Not one file of upstream's tree is edited. *Why:* a repeater's behaviour — routing,
regions, loop detection, the ACL, the CLI — moves with every upstream release, and a copy would be
a fork to merge by hand. *How it is possible:* the three calls a mesh client needs to intercept
(`searchPeersByHash`, `getPeerSharedSecret`, `onPeerDataRecv`, with `onPeerPathRecv` and
`onTraceRecv`) are virtual and protected, and everything local management needs is public
(`handleCommand`, the `sensors` settings seam). The first task of the repeater's phase 1 proves it
by building it, and says so if it is wrong.

**S-3 · A purpose-built link, not the companion protocol.** The two boxes speak
[`LINK-PROTOCOL.md`](LINK-PROTOCOL.md): framed, checksummed, request/response from the Walter with
events from the RAK. *Why not the companion frame protocol:* it is the interface of a chat client
— contacts, channels, message queues — and serving it would mean running the chat client inside
the repeater.

**S-4 · The mesh client mirrors `AgentMesh`.** The link's mesh messages are, one for one, the
public calls of `src/mesh/AgentMesh.h` in the agent firmware: begin a job with a peer and a path,
send a login / a request / a CLI line / a discovery / a trace, take the reply. The RAK's
implementation is a port of that file. *Why:* that code has been on the air against real
repeaters, its rules are hard-won (a login reply carries no tag of ours; the path a reply returns
by is learned; one packet per command), and the job logic above it is what the Walter should run
(`proposals/03`).

**S-5 · The RAK's firmware is replaced over SWD, by the Walter, and only the application.** No
bootloader DFU in the field path, no chip erase, ever, by anything automatic. The Walter keeps a
known-good image of the RAK's firmware in its own flash and puts it back when a new one fails its
health check. *Why:* the project exists because remote repeaters were bricked by OTA. The RAK
cannot brick itself if it never updates itself.

**S-6 · The Walter updates itself A/B with rollback**, and a new image is confirmed only after it
has booted, spoken to the RAK and reached the server. A Walter that cannot do all three goes back.

**S-7 · Never both at once.** One wake updates the RAK or the Walter, not both. When the link's
major version changes, the Walter goes first and speaks both majors until the RAK has followed.

**S-8 · Releases are signed where they are built** (Ed25519 over the manifest) **and checked where
they are used** ([`FIRMWARE-MANIFEST.md`](FIRMWARE-MANIFEST.md)). The server delivers; it is not
trusted.

**S-9 · The Walter is built with ESP-IDF**, pinned, in Espressif's Docker image — the same image
locally and in CI — with the `walter-modem` component. The repeater is built with PlatformIO, as
MeshCore and the agent firmware are.

**S-10 · Local first.** A task is verified on the machine it is written on, by one script that CI
also runs. CI confirms; it is not where debugging happens. A red CI after a green local run is a
finding about the script, and stops the task.

**S-11 · A family, not a firmware.** The repeater repository builds *roles* (repeater; later
bridge; NoiseScope) for *boards* (the single adapter; later the dual adapter's two positions) in
*bands*. Every image says which it is (the descriptor), every release lists them (the manifest),
and the running firmware says it again in the link's `HELLO`. The Walter addresses radios by
*slot*; there is one slot today.

**S-12 · The rails of the agent firmware hold here.** Nothing the mesh says is logged as text.
A password is in RAM for the length of the exchange and nowhere else. One packet per command.
TLS is verified. Path hashes this firmware chooses are two bytes or more (meshmaster.dk D-33).
Each has a test or a grep, not a promise.

**S-13 · No default password is ever on the air.** A repeater that boots with no settings makes
itself a random admin password that nobody knows; provisioning — by the Walter over the link or a
person at the USB port — sets the real one.

**S-14 · The trunk is `main`** in the two new repositories (GitHub made it; the hardware
repository uses it too).

## 4. The repeater firmware

`twinoak.meshcore.repeater-firmware`, PlatformIO, nRF52840.

| Layer | What |
|---|---|
| upstream, from the pin | MeshCore's core, its `simple_repeater` example (`MyMesh`), the RAK4631 board support |
| ours, `src/` | `main.cpp`; `TwinOakMesh : MyMesh`; the board target; the link; the clock keeper; power and environment; the mesh client; the observation ring |
| ours, host-portable | the link codec, the clock rules, the shunt arithmetic, the job state, the ring — compiled beside the native tests |

What it adds to a stock repeater:

1. **The link** — local management for the Walter with console privileges; the unit's telemetry
   with the right shunt per channel.
2. **A clock that survives having no RTC** — a lower bound kept in flash so time never runs
   backwards across a reset; the boot advert held until the Walter has set the time.
3. **The mesh client** (S-4) and the **observation ring** — the radio half of an exit node.
4. **Attention** — a way to wake the Walter: low battery, a restart, or an admin on the mesh asking
   for it.
5. **A descriptor** in every image, and a manifest for every release.

## 5. The Walter firmware

`twinoak.meshcore.walter-firmware`, ESP-IDF, ESP32-S3 with a Sequans LTE-M modem.

| Part | What |
|---|---|
| board | the pins of `INTERCONNECT.md` and their rules: reset is open-drain and never driven high; tristate before kill; two resets at least a second apart |
| link client | `LINK-PROTOCOL.md` from the other end: retries, the remembered response, events, telling a repeater from NoiseScope |
| SWD | a bit-banged debug port (100 kHz to start, through 470 Ω and 2.2 nF), the nRF52's memory access port, its flash controller — and a simulator of all three for the tests |
| flasher | an image, a region, page erases of exactly what the image covers, write, verify, the bootloader's settings page; refusal of everything outside the region |
| images | a store for the RAK's golden image, a candidate and a scanner image; manifest, signature, descriptor |
| updates | the RAK's update with health check and rollback (S-5); the Walter's own (S-6) |
| supervisor | the RAK's heartbeat; the ladder from "ask again" to "reflash the golden image", with daily limits |
| uplink | the modem: attach, time, HTTPS with a verified chain, resumable downloads |
| duty cycle | wake, do one thing well, sleep; the check-in interval follows the battery |
| agent | the MeshMaster agent — **not in these piles** (§8) |

## 6. Updating a unit

```
release (signed) ─► catalogue ─► Walter: verify ─► store as candidate
                                    │
     battery ok, link up, time set ─┤
                                    ▼
              flash candidate over SWD (application region only)
                                    │
                    reset ─► HELLO within the window, version and role as flashed,
                             link steady, clock set, the radio initialised
                              │ yes                          │ no
                              ▼                              ▼
                 candidate becomes golden        flash golden back; the candidate's hash
                                                 is remembered and never tried again
```

Every step is journalled in the Walter's flash before it is taken, so a Walter that resets in the
middle resumes or rolls back; it never guesses. A RAK that will not answer after the golden image
is back is a hardware fault, reported, and left alone.

## 7. What can be checked before the boards arrive, and what cannot

The piles were written for a week without hardware. Their rule: **logic is portable and tested on
the host; the part that touches a pin is thin and sits behind an interface with a fake.** So the
link codec is tested against the fixtures, the SWD engine against a simulated nRF52, the update
procedure against a simulated flash that loses power at every step, the supervisor against a RAK
that goes silent on cue.

What no session can check is listed, task by task, under *Not verified here*, and collected in
each repository's `docs/bench.md` in the order `hardware/pcb/BENCH-BREADBOARD.md` §7 brings the
rig up: UART, reset, SWD, flash, then the kill line and I2C. With the rig on USB, a later pile lets
the sessions flash and read the consoles themselves.

## 8. MeshMaster

MeshMaster already has the place for this unit. Its spec describes a **pack agent**: agent kind
`direct`, transport `native`, bolted to one repeater, serving `status` / `config.read` /
`config.apply` over the UART at no cost to the mesh, plus its own job types — power, power-cycle,
OTA, a NoiseScope session, a console line (`spec/PLUGINS.md` §3 and §6, `spec/EXIT-NODES.md` §2).
Phase 5, where it was to be built, is postponed (D-26) because no repeater ran this design. One
will.

What is new since that was written, and therefore proposed rather than assumed:

| | |
|---|---|
| [`proposals/01`](proposals/01-the-unit-in-meshmaster.md) | the unit is a `direct` agent **and** an exit node for its neighbours, on one key, heard from rarely: what that asks of the agent kinds, health, routing and association |
| [`proposals/02`](proposals/02-firmware-catalogue-and-updates.md) | a firmware catalogue fed by signed manifests, and update jobs with a canary, preconditions and an audit trail |
| [`proposals/03`](proposals/03-agent-core-as-a-library.md) | the agent firmware's job logic as a library with a mesh seam and an uplink seam, so the Walter runs the same code as the Heltec |

**Decided 2026-10-14 (Thomas).** All three proposals are accepted, with two choices: the TwinOak
features are built **in MeshMaster's core, in their own folders**, not through the pack registry
`PLUGINS.md` designed (its D-45); and the unit is a **`mesh` agent with `servesRepeaterId`**,
transport `native`, not the `direct` kind. The work is three piles — `meshmaster.dk` pile 4.14
(*the unit*: the sleeping exit node, the firmware catalogue served with Range, the update jobs and
the canary), the exit-node firmware's pile 1.5 (*the agent core as a library*) and the Walter's
phase 4 (*the agent*) — in that order of dependency. Until phase 4 lands, the Walter's wake cycle
still stops at its seam.

## 9. Order of work

| | Repeater firmware | Walter firmware |
|---|---|---|
| phase 0 | toolchain, CI, release with a manifest, the upstream watch | toolchain, CI, release with a manifest, the board's pins |
| phase 1 | our application on upstream's repeater; the link; the clock; local management; power | the link from the other end; SWD; the nRF52; the flasher |
| phase 2 | the mesh client; observations; the rails | manifests and signatures; the image store; the RAK's update; its own; the supervisor |
| phase 3 | roles and boards; NoiseScope moves in; first-boot provisioning | the modem; HTTPS; settings and console; the duty cycle; the wake cycle |
| phase 3.1, 3.2 | the fast sweep; a MeshCore bump can be green | — |
| phase 4 | — | the agent: the library at a pin, the mesh over the link, the uplink over the modem, the firmware jobs on the update machines |
| later | the bench pile; the bridge role; the dual adapter | the bench pile; NoiseScope as a job |

The two repositories do not wait for each other: what they share is fixed here first. Phase 4 of
the Walter waits for two piles elsewhere (§8): `meshmaster.dk` 4.14 and the exit-node firmware's
1.5, in that order, because 1.5's last task syncs fixtures 4.14 changes.

## 10. Open, and whose

1. **The release key** — Thomas makes it (`manifestref.py keygen`) and stores the private half as
   the release workflows' secret. Until then no unit accepts an update, which is correct.
2. **APPROTECT on the production modules** (`BENCH-BREADBOARD.md` caveat 6) — read `INFO.VARIANT`
   on the first board. If the access port is closed, opening it is a bench procedure with a chip
   erase and a full reflash, done once per module by a person; the flasher detects it and refuses.
3. **The bootloader's settings page** after an SWD flash — the Adafruit bootloader checks a CRC it
   recorded at the last DFU; the flasher has to leave that page in a state the bootloader accepts.
   The flasher task reads the bootloader's source and decides; the bench confirms.
4. **Scoped floods** — whether the mesh client's floods should carry the repeater's default-scope
   transport codes. They follow the agent firmware (unscoped) until the Danish network's use of
   scopes says otherwise.
5. **The Walter's draw asleep, and the check-in interval the battery can pay for** — measured on
   the bench; the policy table has placeholders until then.
6. **Whether the Walter stays** (the cheaper-MCU question of 2026-09) — deferred by the order; the
   Walter firmware keeps the modem and the board behind interfaces so that an answer later costs a
   port and not a rewrite.
