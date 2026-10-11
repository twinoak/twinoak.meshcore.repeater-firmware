# The box link, from a laptop

`spec/LINK-PROTOCOL.md` is the protocol — the UART between this firmware on the
RAK4630 and the Walter in the LTE box. The Walter does not exist yet, so this
page is how a person talks to the link instead: a 3.3 V USB-UART dongle and
[`tools/linkcli.py`](../tools/linkcli.py).

What the firmware answers today (phase 1's tasks 03 and 04): `HELLO`, `PING`,
`STATUS`, `TIME_SET`, `TIME_GET`, `IDENTITY`, `REBOOT`, and `EV_BOOT` once when
it comes up. Everything else of the protocol answers `ERR_UNSUPPORTED` until the
task that owns it lands — 05 the console and attention, 06 telemetry, phase 2
the mesh client — and `HELLO`'s `caps` is `0x40` (`TIME_PERSIST`) and nothing
else for exactly the same reason: a capability is an interface that is there
(`src/link/session/Interfaces.h`).

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
python3 tools/linkcli.py --port COM7 listen 30    # the events, for 30 s
python3 tools/linkcli.py --port COM7 reboot 0     # and then `listen` for EV_BOOT
```

`hello` is the first request of a session and may be repeated at any time. It
is how you learn what you are talking to:

```
HELLO
  protocol      1.0
  role          1 (repeater)
  slot          0
  max_payload   240
  caps          0x40 (TIME_PERSIST)
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
- `caps` grows as the later tasks land: `0x40` today. Nothing else of this page changes with
  it.

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

That builds the loopback, runs `hello`, `ping`, `status`, `identity` and
`reboot` against it, and times one pass of the link. It is not part of
`tools/verify.ps1`: it needs a host compiler, and what it proves about the
session itself is already a native test (`test/test_link_session`).

## 5. Inside the firmware

Three layers, and `docs/ARCHITECTURE.md` §4 is the design:

| | Where | What |
|---|---|---|
| codec | `src/link/codec/` | CRC-16/CCITT-FALSE, COBS, packets, frames, one function per message. Portable. |
| session | `src/link/session/` | the dispatcher, the one-entry retry memory, the event counter, and the interfaces it reaches the firmware through. Portable. |
| transport | `src/link/SerialTransport.*` | `Serial1`, and one bounded slice of each `loop()`. |
| the board's half | `src/link/BoardLink.*` | the uptime, the reset reason, the boot identifier, the restart, the identity — and the one call in `setup()` and one in `loop()`. |

One pass of `loop()` reads what the UART has (bounded), handles **at most one**
request, and writes what fits without blocking. A request that completes while
the previous answer is still unsent waits in the receiver; the answer is never
overwritten and nothing ever blocks. That is what keeps the link from ever
holding up repeating.
