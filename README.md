# TwinOak repeater firmware

The firmware family for the LoRa box of a
[TwinOak standard repeater v2](https://github.com/twinoak/twinoak.meshcore.standard-repeater-design-v2):
a RAK4630 behind a cavity filter, in a shielded box, with a second box beside it
that holds an LTE modem and is allowed to reflash this one.

It is a **MeshCore repeater** — upstream's, compiled unmodified from a pinned
commit — with four things added:

1. a **link** to the management MCU in the other box (the Walter): local
   management with console privileges, and the unit's power and environment
   telemetry with the right shunt for each channel;
2. a **clock** that copes with a board that has no RTC;
3. the **radio half of a MeshMaster exit node**: on the Walter's behalf it can
   log in to a neighbouring repeater, ask it something, and hand back the
   answer — as itself, with its own key, without ever ceasing to repeat;
4. an **image that says what it is**, in a release that is signed.

The plan, the decisions and the two protocols are in [`spec/`](spec/README.md)
(start with `spec/SYSTEM.md`). How this firmware is put together is
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). How a release gets onto a board
is [`docs/flashing.md`](docs/flashing.md), and how a board becomes a unit is
[`docs/provisioning.md`](docs/provisioning.md). How it gets built is
[`tasks/`](tasks/README.md); how it gets released is
[`docs/release.md`](docs/release.md); how it keeps up with MeshCore — the weekly
pull request that moves the pin, the one command for a bump that needs a
session, and the eight places a bump re-reads — is
[`docs/upstream.md`](docs/upstream.md).

## State

**There is a release: [`v0.1.1`](../../releases/latest).** The images, the base,
a signed manifest and a body a person can read are a GitHub Release, signed with
the key `73efdd383a957596` on a GitHub runner;
[`docs/flashing.md`](docs/flashing.md) is the one page for getting it onto a
board and [`release-notes/v0.1.1.md`](release-notes/v0.1.1.md) is what it says
about itself. `v0.1.0` of 5 November was the first;
[`v0.1.1`](release-notes/v0.1.1.md) is the one whose `HELLO` says the radio's
band (link 1.1), without which MeshMaster cannot pick a unit's first image over
LTE. Before them, the only way to get this firmware onto a board was to build
it.

The piles through 3.8 have landed. What the firmware does **today**:

- **It is a MeshCore repeater**, upstream's `simple_repeater` from the pin
  `repeater-v1.18.0` (`81dbae59`), compiled unmodified and subclassed — the
  radio, the routing, the ACL, the console and the preferences are upstream's
  and behave exactly as a stock node's (`docs/ARCHITECTURE.md` §2).
- **It speaks the box link** on `Serial1` at 115200, **protocol 1.1**: every
  request and event the protocol defines outside its two reserved ranges,
  `caps 0x7f`, and `HELLO`'s answer and `EV_BOOT` end with the image's `band`.
  A Walter that asks in 1.0 is answered in 1.0. `tools/linkcli.py` from a laptop
  is how a person walks it ([`docs/link.md`](docs/link.md)).
- **It keeps time without an RTC**: the clock is restored across a restart from
  a file, adverts are held back until it is trustworthy, and a backwards set is
  refused once anything of ours has carried a timestamp (`docs/link.md` §2a).
- **It is managed over the link and from the mesh**: upstream's own
  interpreter behind `CLI`, ten settings on `sensor get/set`, and an attention
  machine that can wake a sleeping Walter — which a mesh admin with a phone can
  ask for (`docs/link.md` §2b and §2c).
- **It measures itself**: three current-sense channels with the right shunt
  each, the air inside the box, two battery thresholds that raise attention,
  and the same numbers in a phone's telemetry view (§2d).
- **It is the radio half of a MeshMaster exit node**: on the Walter's behalf it
  begins a job with one peer and makes the five exchanges — a login, a request,
  a CLI line, a path discovery, a trace — **one packet per send**, as itself,
  with the reply delivered as an event (§2e, §2f). At most six sends a minute
  and two seconds apart, whatever the Walter asks for (§2h).
- **It keeps what it hears**: every advert whose signature verified goes into a
  ring of 64 records for the Walter to collect, application data copied and
  never parsed (§2g).
- **It says what it is and is released signed**: an image descriptor, a
  manifest, an Ed25519 signature over it, a release body that begins with words
  a person wrote, and a weekly pull request that moves the MeshCore pin
  ([`docs/release.md`](docs/release.md),
  [`docs/upstream.md`](docs/upstream.md)).
- **It is a family, not a firmware**: a second role, `scanner` — NoiseScope,
  the SX1262 spectrum and noise scanner that is flashed in place of the
  repeater when a site has a noise problem, from the same board target, the
  same power code and the same release
  ([`docs/noisescope.md`](docs/noisescope.md)).
- **It comes up as a member of the Danish mesh with nobody near it, and inside
  its band**: the right channel from the build, the duty cycle the band allows
  (10 %, where upstream starts a repeater at 50 %), a random admin password
  nobody knows, and a first-boot profile of console lines that runs exactly
  once — and one written procedure for making it *this* unit, at the USB port
  or over the link ([`docs/provisioning.md`](docs/provisioning.md)).

Not done, and deliberately: the bridge role and the dual adapter
(`docs/ARCHITECTURE.md` §8 has the seams).

**What a board has shown, and what it has not.** The first unit was powered up
on 5 November 2026 and its repeater came up on the mesh — on a build flashed by
hand. A day later it showed the one thing a release had not: on 6 November a
Walter at `v0.1.0` left the radio's application half-written over SWD, and the
radio came back over its own USB-C from **`v0.1.0`'s published `.uf2` — as a
new node**: the file system holding its key had been torn and the firmware of
the day formatted it without a word (pile 3.9's task 01 found the mechanism and
makes the radio refuse instead; [`docs/flashing.md`](docs/flashing.md) §3a is
what to do and what to expect). No board has run `v0.1.1`. 26 of the 102 lines of
[`docs/bench.md`](docs/bench.md) are ticked; the rest need a radio, a phone, a
scope or the roof, and that page is the walk that would prove them. `tools/verify.ps1` is what has been proved without one.

## Building it

```powershell
pio run -e repeater_rak4630_single   # the repeater for the single-RAK adapter
pio run -e scanner_rak4630_single    # NoiseScope, the scanner role (docs/noisescope.md)
pio test -e native                   # the tests that need no board (in a container on Windows)
.\tools\verify.ps1                   # everything CI runs, in order, with a log per step in out/verify/
```

A clean first build downloads, once, into `~/.platformio`: the `nordicnrf52`
platform, the ARM GCC toolchain, MeshCore's fork of the Adafruit nRF52 Arduino
core at the commit MeshCore pins, `adafruit-nrfutil` for the DFU package; and
into `.pio/libdeps/`: MeshCore itself at the pinned commit (`[meshcore]` in
`platformio.ini`) and every library at the exact version `[libs]` names. It
takes a few minutes; every build after it takes seconds. The images it leaves
in `.pio/build/<env>/` — and `tools/verify.ps1` copies to `out/build/<env>/` —
are `firmware.hex` (the Walter, over SWD), `firmware.zip` (a laptop,
`adafruit-nrfutil dfu serial`), `firmware.uf2` (the bootloader's drive) and
`firmware.elf`.

**To put one on a board** — a build of your own or a release's file —
[`docs/flashing.md`](docs/flashing.md) is the page: the `.uf2` on the
bootloader's drive or the `.zip` over serial DFU on the adapter's USB-C, the
`.bin` through the Walter over SWD, the base `.hex` for a radio fresh from the
factory, the four things to look at afterwards, and the admin password nobody
knows until somebody sets one.

`pio test -e native` is a host build. Linux and CI use the machine's `g++`;
this Windows machine has none, so `tools/verify.ps1` runs that step in a
`python:3.11-slim` container (`tasks/RUNBOOK.md` §2 has the one-line
`docker run`).

`tools/verify.ps1` also rehearses the release on every run: the release
tooling's own tests, then a dry run over the build it has just made, leaving
`out/release/` with the three images, a validated `manifest.json`, its
signature by the specification's public *test* key and `SHA256SUMS`
([`docs/release.md`](docs/release.md)). That step needs the `cryptography` of
`tools/release/requirements.txt`, and on a machine without it the same
container provides it. The task piles that build the rest are run with

```powershell
.\tasks\run-pile.ps1          # phases 0 to 3, one headless session per task
.\tasks\run-all.ps1 -Phase 0  # one phase
```

### Where CI runs

**Everything that checks runs at home; everything that ships stays away.**

| Workflow | Runs on | Why |
|---|---|---|
| [`ci.yml`](.github/workflows/ci.yml) | `[self-hosted, unraid]` | it only checks — `tools/verify.ps1` and nothing else — and GitHub-hosted minutes got out of hand |
| [`upstream.yml`](.github/workflows/upstream.yml) | `[self-hosted, unraid]` | it asks whether MeshCore moved and opens a pull request; it ships nothing |
| [`release.yml`](.github/workflows/release.yml) | `ubuntu-latest` | it builds the images every repeater's update trusts and signs them with `RELEASE_SIGNING_KEY` ([`keys/README.md`](keys/README.md)). It runs at a tag, so its minutes are few |

`unraid` is the label on the organisation's own GitHub Actions runners
(`unraid-1`…, `myoung34/github-runner` on the Unraid box).
`tools/ci/check-runners.ps1` is the rule, in one place, over every job of
`.github/workflows/`, and it runs as `ci.yml`'s first step — so neither half of
it moves by accident. Run it by hand with
`pwsh -File tools/ci/check-runners.ps1`.

What a runner must have for these two: **node** (every action is one),
**pwsh**, **bash**, **git**, **curl**, **tar**, the usual `sed`/`awk`/`date`,
and **`gh`** for `upstream.yml`. `g++` is the one `ci.yml` installs itself when
it is missing, because without a host compiler `tools/verify.ps1` would run the
native tests in a container — and a container started from inside the runner's
own container bind-mounts a path the daemon's host does not have. Python is
`actions/setup-python`'s; PlatformIO's platform, toolchain and libraries live in
the runner's own `~/.platformio`, which is why the cache steps run on GitHub's
runners only. Nothing a job starts outlives it, nothing writes outside its
workspace and that home, and no step opens a port on the host.

## What it will be, in one table

| | |
|---|---|
| MCU / radio | nRF52840 + SX1262 (RAK4630 stamp, u.FL) |
| Built with | PlatformIO; MeshCore as a pinned dependency ([`docs/upstream.md`](docs/upstream.md)); the Adafruit nRF52 core MeshCore pins |
| Builds | `pio run -e <role>_<board>`; `pio test -e native` for everything that needs no board |
| Verified by | `tools/verify.ps1` — locally first; CI runs the same script |
| Flashed | in service: by the Walter over SWD, application region only. At a bench: UF2 or serial DFU over the adapter's USB-C ([`docs/flashing.md`](docs/flashing.md)) |
| Released as | a GitHub Release: the images, the base, `manifest.json`, its signature, `SHA256SUMS`, and a body that begins with `release-notes/vX.Y.Z.md` |
| Channel | 869.618 MHz, 62.5 kHz, SF8, CR8, +22 dBm (MeshCore Denmark, EU Narrow) |

## Provisioning it

A board fresh from the flasher is already on 869.618 MHz, already repeating, and
already carries an admin password drawn from the radio's own noise that nobody
knows — so nobody can administer it over the mesh until a person or the Walter
sets one. Making it *this* unit is six settings and one page:

```bash
# at the field port, 115200: six lines of docs/provisioning.md section 3
# or, over the box link, the same lines in a file:
python3 tools/linkcli.py --port COM7 provision unit-odense.txt
```

[`docs/provisioning.md`](docs/provisioning.md) is that page: what a unit is out
of the box, the settings a unit needs and the command for each, the USB session
line for line, the same lines over the link, and what MeshMaster has to be told
afterwards — including that the admin password set there is the only copy there
is. `sensor get firstboot.state` says what the boot did: `done`,
`partial: <n> refused`, or `never`.

## Rails that hold in every task

Nine rules, and the test that fails when one is broken. `test/test_rails` is
one test per rule, each failing with a sentence that names the rule
(`tasks/phase-2/reports/04.md` has every one of them broken on purpose once and
the sentence it printed); where a rule can only be approximated by a grep, that
test says what it does and does not catch.

| | The rule | The test that holds it |
|---|---|---|
| 1 | **Nothing the mesh says is logged as text.** `MESH_PACKET_LOGGING` and `MESH_DEBUG` stay off in every environment. | `Rails.Rule1_*` — the resolved flags of every board environment, and a walk of `src/` |
| 2 | **No source prints a packet payload, a CLI line or its reply, an advert's application data, or a password.** | `Rails.Rule2_*` — three tests: a payload beside a print, and no print-like call at all under `src/common/link/` or `src/roles/repeater/meshclient/` |
| 3 | **No default password is ever on the air.** A node that boots with no saved settings draws itself a random admin password that nobody knows (`src/roles/repeater/app/FirstBoot.cpp`); the one a person sets at the USB port with `password <new>` is the first anyone can use, and no line of the first-boot profile may carry a password at all. | `Rails.Rule3_*` — no environment defines `ADMIN_PASSWORD` as upstream's word or leaves it out, and the first-boot path exists and is called; `test/test_password` holds what the random one looks like and `test/test_firstboot` holds the profile's table to it |
| 4 | **Path hashes this firmware chooses are two bytes or more.** | `Rails.Rule4_*` — the constant is 2, every `sendFlood` passes it, no literal 1; and the first-boot profile still raises `path.hash.mode`, which is the half upstream decides from a preference |
| 5 | **One packet per send.** No reciprocal path is returned for the client's peer. | `Rails.Rule5_*` — the branch of `onPeerPathRecv` that takes the job peer's path returns false and nothing else |
| 6 | **A password is in RAM for the length of a send** and nowhere else. | `Rails.Rule6_*` — no file on the path of a `SEND_LOGIN` or a sensitive `SEND_CLI` names a file-system API; `test_link_session` searches the session, the client and the job for the bytes afterwards |
| 7 | **Upstream's INA3221 and BME280 support stay out of the build** — one shunt value for three channels, and an I2C conversion on the path of a link request. | `Rails.Rule7_*` |
| 8 | **`spec/` is a copy and MeshCore is not edited.** | `Rails.Rule8_*` — the SHA-256 and byte count of every file against `spec/MANIFEST.txt`, and no script that names the pinned checkout writes in it |
| 9 | **No heap allocation** under `src/common/link/`, `src/roles/repeater/meshclient/` or `src/roles/repeater/clock/`: a node that repeats for months is given its buffers. | `Rails.Rule9_*` |

Three more, which are a shape rather than a grep:

- **It repeats, whatever else happens.** Nothing added here may stall the mesh
  loop: not the link, not a sensor that stopped answering, not the Walter. The
  link gets a bounded slice of `loop()` and never waits
  (`docs/ARCHITECTURE.md` §4); a request never performs I2C and answers out of
  a cache (`Rails.AlsoKept_NoRequestHandlerCanReachAnI2cBus`); and a client
  send can delay a packet being forwarded by its own airtime and no more, at
  most six times a minute (§7, and `src/roles/repeater/meshclient/SendCeiling.h`).
- **Upstream is not edited.** Our code subclasses and composes
  (`docs/ARCHITECTURE.md` §2); every override carries `override`, so a pin bump
  that changes one is a compile error and not a method that silently stops
  being called.
- **Never a chip erase.** The file system holds the node's identity; an image
  lives inside its application region and nowhere else
  (`docs/ARCHITECTURE.md` §9, and the release tooling checks the region against
  the linker script).

## Licence

MIT — [`LICENSE`](LICENSE). MeshCore is MIT too and is a dependency, not a
fork: nothing of its tree is vendored here except what a consumer of the
package cannot reach, and each such file says so.

One file in a release is nobody's here: the base under the application on a
RAK4630 (`boards/base/rak4630/`) — Adafruit's bootloader as RAKwireless builds
it (MIT) over Nordic's SoftDevice (Nordic's own licence, binary redistribution
with a Nordic chip). [`THIRD-PARTY-NOTICES`](THIRD-PARTY-NOTICES) carries their
notices and goes into every release beside the manifest.
