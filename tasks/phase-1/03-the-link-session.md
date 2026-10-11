# 03 — The link session

**Depends on:** 02.
**Spec:** [`spec/LINK-PROTOCOL.md`](../../spec/LINK-PROTOCOL.md) §1–§3, §5.1–§5.3, §5.5, §5.6, §9, §10; [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §4.
**Size:** one session.
**Model:** claude-opus-5

## Goal

The link is alive on `Serial1`. A peer that sends `HELLO` learns what this
firmware is; `PING` proves the loop is turning; `STATUS`, `IDENTITY` and
`REBOOT` work; a restart announces itself with `EV_BOOT`; and a request sent
twice is executed once. And because the Walter does not exist yet, a small
tool on a laptop speaks the same protocol through a USB-UART dongle.

## Deliverables

- **The session** (`src/link/session/`, portable): a dispatcher from request
  type to handler; the **one-entry memory** of the last `(type, seq)` executed
  and its response, so that a repeated request gets the remembered response
  and its handler is *not* called again (LINK-PROTOCOL §3); a request of an
  unknown type answered `ERR_UNSUPPORTED`; the event sequence counter; the
  "idle for 100 ms" rule for the leading `0x00`. It reaches the rest of the
  firmware only through small interfaces it is given — the system (uptime,
  reset reason, restart in a mode), the identity (key, name), and one each for
  what tasks 04 to 06 and phase 2 add — so the native tests run it whole
  against fakes, and a capability whose interface is absent is simply not
  advertised.
- **Handlers**: `HELLO` (version negotiation as §5.1 says: our major or
  `ERR_UNSUPPORTED`; the highest minor both know; `role` repeater, `slot` 0,
  `max_payload` 240, `caps` built from the interfaces present, a `boot_id`
  drawn once per boot from the radio's RNG, our version string, the board name
  `rak4630-single`, and `upstream` as `<tag>+<pin8>`), `PING`, `STATUS` (the
  fields later tasks own read as zero and their flag bits clear), `IDENTITY`,
  `REBOOT` (the response leaves the UART before the restart; a mode the board
  cannot do is `ERR_RANGE` — say which of the four the RAK4630 can do, from
  upstream's board code at the pin).
- **`EV_BOOT`** once, when the link comes up after a reset, with the reset
  reason mapped from the nRF52's `RESETREAS` to the codes of §5.3 (a portable
  mapping function with a test).
- **The transport** (`src/link/SerialTransport.*`): `Serial1` at 115200, and
  the rule of ARCHITECTURE §4 made concrete — per `loop()`: read what the
  UART has (bounded), handle **at most one** request, write what fits without
  blocking. Raise `Serial1`'s receive buffer if the core's default cannot hold
  a full frame between two passes of the loop, and say what it was and is.
  Measure the worst-case time the link takes out of one pass (a toggle and a
  counter are enough) and put the number in the report.
- **Wired into `main.cpp`**: one call in `setup()`, one in `loop()`.
- **Two settings** in `TwinOakSensors`: `link.errors` and `walter.seen`
  (ARCHITECTURE §4, read-only).
- **`tools/linkcli.py`** — a bench tool for a laptop with a USB-UART dongle on
  the adapter's link pins (3.3 V!). It imports `spec/tools/linkref.py` for
  every byte it sends and reads, uses `pyserial`, identifies itself with
  `peer_kind` 2, retries as §3 says, and has one sub-command per request this
  firmware answers (`hello`, `ping`, `status`, `identity`, `reboot <mode>`),
  plus `listen` to print events. Later tasks add their requests to it.
  `docs/link.md` starts here with how to wire the dongle and a session.

## Tests (native)

`test/test_link_session/`, the whole session against fakes:

- each request case of the fixtures that this task handles, fed as its `frame`,
  answered with exactly the fixture's response frame when the fakes return the
  fixture's values;
- the same request frame twice: one handler call, two identical responses; a
  different `seq`: two calls;
- an unknown type: `ERR_UNSUPPORTED` with that type's response code;
- `HELLO` with a major we do not speak; `HELLO` with a higher minor;
- two frames in one read, a frame split over three reads, noise between
  frames;
- a request while the writer still holds an unsent response (it must not be
  lost, and must not block);
- the reset-reason mapping.

## Acceptance criteria

- [ ] `tools/verify.ps1` green; flash and RAM cost of the link in the report.
- [ ] The repeated-request test proves a handler is not called twice.
- [ ] The report states the measured worst-case time the link takes from one pass of `loop()`, and the `Serial1` buffer sizes.
- [ ] `python tools/linkcli.py --help` works on the host, and `linkcli.py` run against a loopback of the session (the native build's session behind a pipe, or a recorded exchange) completes `hello`, `ping` and `status` — so the tool is tested before a board exists.

## Not verified here (Thomas, with a board)

Add to `docs/bench.md`, "2. The link from a laptop":

- A 3.3 V USB-UART dongle on the adapter's UART pins (J4/J5 on 1–2), or on
  D-sub pins 8/9 through the harness: `linkcli.py hello` prints the version,
  role and capabilities; `ping` answers; `status` shows the uptime and reset
  reason; `reboot 0` restarts and `listen` then shows `EV_BOOT`.
- Through both bulkhead filters (470 Ω and 2.2 nF on each line): the same, and
  `link.errors` stays 0 over an hour of `ping` once a second.
- Repeating is unaffected while `ping` runs at ten a second (a message through
  the repeater still arrives).

## Out of scope

`TIME_*` (04), `CLI`, `SLEEP_NOTICE`, attention (05), telemetry (06), the mesh
client (phase 2).
