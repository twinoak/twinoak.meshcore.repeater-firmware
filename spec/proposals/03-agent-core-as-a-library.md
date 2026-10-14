# Proposal 03 — the agent's job logic as a library

**For:** `twinoak/twinoak.meshmaster.exitnode-firmware` (named `meshmaster-agent-firmware` until 2026-10-11). **Status:** accepted 2026-10-14 — built as that repository's pile 1.5, tasks 15–19, consumed by the Walter's phase 4. Written 2026-10-11 while
that repository was closed for other work. Read against its `src/` at `be73444` (local `master`).

## Why

The Walter should be a MeshMaster agent. Everything hard about being one is already written, tested
and proven on the air in the agent firmware: the login sequence and its classes, the ACL restore
after an admin session, one send per command, the config read and apply engines, sessions, the
airtime budget, the result buffer, the server policy. Writing it a second time for the Walter would
be writing it worse.

What stops the Walter using it today is not the logic. It is three assumptions around it.

## The three assumptions

| The agent firmware assumes | On the Walter |
|---|---|
| the mesh is in the same process: `JobRunner` and `AgentLoop` hold an `AgentMesh&` | the mesh is across a UART, behind the box link |
| the uplink is WiFi with `WiFiClientSecure` / `HTTPClient` | the uplink is an LTE-M modem with its own HTTP and TLS |
| the platform is Arduino on ESP32 (`millis`, `Stream`, `esp_system.h`, NVS through `Preferences`) | ESP-IDF |

Twenty-three files are already free of all three — they are the ones `[env:native]` compiles. The
proposal is to move the line so that the *job logic* is on the portable side too.

## The seams

**1. `IMesh`** — what `JobRunner` and `LoginSequence` call. It is `AgentMesh`'s public surface and
nothing more:

```
beginJob(key, path) · setDirectPath() · forgetPath() · endJob() · hasPath() · describePath()
sendLogin(password) · sendRequest(type, data) · sendCli(text, sensitive) · sendDiscovery() · sendTrace(hashes)
awaitReply(tag, timeout) · awaitText(timeout) · reply() · uniqueTime() · counters() · overheard()
```

`AgentMesh` implements it in-process (nothing changes for the Heltec). The Walter implements it as
a link client: each call is one request of `LINK-PROTOCOL.md` §6, and `awaitReply` is the
`EV_MESH_REPLY` or a `REPLY_GET`. The link was designed against this list (`SYSTEM.md` S-4).

One difference to settle: `sendCli(text, timestamp)` takes a timestamp today, drawn from
`uniqueTime()` a line earlier. Across the link the RAK draws it at the moment it builds the packet
and returns it. The interface should return the timestamp rather than take it; the in-process
implementation loses nothing.

**2. `IUplink`** — what `AgentClient` is to its callers: `fetchConfig`, `postHeartbeat`,
`postObservations`, `claimJobs`, `postJobResult`, each returning the `Call` record
`ServerPolicy` reads. The WiFi implementation stays where it is. The Walter's speaks through the
modem. `TlsRoots.h` and the "verified, never bypassed" rail apply to both.

**3. A small platform seam** — a millisecond clock, a wall clock, a log sink, and a wipe. Most of
the portable files already take these as parameters.

## The shape

- `src/agent/` (and `mesh/ReplyCodec`, `Backoff`, `LogRing`) becomes a PlatformIO **library inside
  the repository** (`lib/agent-core/`) with a `library.json`, no Arduino include anywhere in it,
  compiled by `[env:native]` whole — `JobRunner` and `AgentLoop` included, against fakes of the
  three seams. That alone is a gain for the Heltec: the host harness of task p1.1-08 becomes the
  ordinary way the runner is tested.
- The board targets link that library and supply `AgentMesh`, the WiFi uplink and the Arduino
  platform.
- The Walter firmware consumes the library **at a pinned commit**, as this repository consumes
  MeshCore: a dependency, an upstream watcher, a sync task when a bump does not compile. As an
  ESP-IDF component it is a `CMakeLists.txt` over the same sources.

## What it costs here

One pile: introduce the three interfaces; move `JobRunner`, `AgentLoop` and `AgentClient`'s
callers onto them; move the files; widen `[env:native]`. No behaviour changes, no contract fixture
moves, and the proof is that every existing native test passes and the board build is the same
size to within the cost of three vtables.

## What it gives the Walter

Its agent pile becomes: implement `IMesh` over the link, `IUplink` over the modem, the platform
seam over ESP-IDF, and wire `AgentLoop` into the wake cycle — with the interval and budget of
proposal 01. The job types, their rules and their tests arrive with the library.

## If this is not wanted

The alternative is a copy of the portable files at a commit, re-synced by script as the contract
fixtures are, and a second `JobRunner` written for the Walter. It works and it rots: every rule
learned on the bench would have to be learned twice.
