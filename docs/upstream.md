# Keeping up with MeshCore

This firmware is built **against** MeshCore, not forked from it (`spec/SYSTEM.md`
S-2). The pin is two lines of `platformio.ini`:

```ini
[meshcore]
pin = 81dbae592ffad40b4212bc0b37e82b38da08f4bc
tag = repeater-v1.18.0
```

Those two lines are the only statement of which MeshCore this is, and a bump
moves them and nothing else (*The pin's history* at the bottom of this page
says when each was adopted and why). MeshCore moves — a release per example
family every few weeks, and ours is the `repeater-v*` one — and this page is
how this repository moves with it. Three tiers, and the first is meant to be
the common case:

| | What happens | Who does it |
|---|---|---|
| **one** | a weekly workflow sees a newer `repeater-v*` tag and opens the bump as a pull request; CI on it is the test | nobody, until it is merged |
| **two** | that pull request is red: one command turns it into a task, and a session does it on the branch the workflow pushed | `tools/new-sync-task.ps1`, then the driver |
| **three** | upstream changed the **protocol** — a packet layout, the login, a CLI reply | a person reading, in this repository *and* in MeshMaster, before any code changes |

## Why a dependency and not a fork

Because of this page. A fork has to be *merged*, and a merge of an upstream
release touches files that are not ours — and every one of those conflicts is a
question nobody here can answer. A pin is a line, and the only thing that can
go wrong is that our own code no longer compiles or no longer agrees with
theirs, which is exactly what CI can tell us by itself
(`docs/ARCHITECTURE.md` §2, "What breaks this, and how it is noticed").

The price is that a consumer of the PlatformIO package does not get everything
a checkout of MeshCore's own tree gets, and some things had to be arranged by
hand. `tasks/phase-0/reports/01.md` has the whole list of ten with what each
cost; what follows is the shorter list that matters here.

## The eight places a bump re-reads

Each of these depends on upstream in a way a compiler may not catch, so each is
read again at the new tag — by the session of a red bump
(`tasks/sync/TEMPLATE.md` is the same list as a checklist), and by whoever
merges a green one: that reading is what the review of a green bump *is*
(*What a bump costs today* below).

| | Where | What it depends on upstream for |
|---|---|---|
| 1 | **the copied flag blocks** — `[nrf52_base]`, `[radiolib]` and `[env:repeater_rak4630_single]` of our `platformio.ini` | MeshCore's `[arduino_base]`, `[nrf52_base]` and `[sensor_base]` (its root `platformio.ini`) and `[rak4631]` and `[env:RAK_4631_repeater]` (`variants/rak4631/platformio.ini`). They use paths relative to MeshCore's own root (`-I variants/rak4631`, `extra_scripts = create-uf2.py`, `post:variants/rak4631/fix_bsec_lib.py`), so they cannot be included from here and are copied. A define added, removed or renamed upstream is a silent difference here: the build stays green and the firmware is not the one upstream ships. Our copy names the section it came from and says what was deliberately left out. Five of upstream's flags are not in our copy of `[nrf52_base]` at all — `LORA_FREQ`, `LORA_BW`, `LORA_SF`, `LORA_CR` and `LORA_TX_POWER` — because they differ by band here and each environment takes them from a `[band_*]` block (`docs/ARCHITECTURE.md` §8); a bump that changes what upstream defaults them to is re-read against those blocks and not against `[nrf52_base]` — and one define in it is **not** upstream's at all, `SERIAL_BUFFER_SIZE=256`, which overrides the Adafruit core's `#ifndef`-guarded 64 so that a 246-byte box-link frame fits (`src/common/link/SerialTransport.h`): a bump of the core that changes that default, or that stops reading the define, is a silent difference too. Upstream's `RADIOLIB_EXCLUDE_*` lines are not in that copy either: they are `[radiolib]`, a section of their own, because **both** images of a board take RadioLib's configuration from there and the two firmwares a repeater swaps between must contain the same driver built the same way (`docs/noisescope.md` §2). A bump re-reads them there, and `[env:scanner_rak4630_single]` beside the repeater's — whose own copied flags are a short list, because it compiles no MeshCore. **Two of upstream's lines are deliberately not in our copy since `repeater-v1.18.0`** (p3.5-06): `platformio/toolchain-gccarmnoneeabi@^1.140201.0` in `[nrf52_base]`'s `platform_packages` — upstream builds nRF52 with GCC 14 now, and its `src/armstubs.cpp` exists for it — because this firmware pins the platform (`nordicnrf52@11.0.0`, GCC 7.2.1) and a compiler change touches every byte of both images, so it is a bump of its own (`tasks/phase-3.5/FOLLOWUPS.md`); and the *removal* of `-D NRF52_POWER_MANAGEMENT` from `[rak4631]`, which this firmware keeps because the link's `reset_reason` depends on the early capture that define enables and the undervoltage boot lock it also enables never fires on this board (`src/roles/repeater/link/BoardLink.cpp`, `tasks/phase-1/reports/06.md`). Both are said beside the lines in `platformio.ini` |
| 2 | **`[libs]`**, every version exact | MeshCore asks with ranges (`library.json`: `jgromes/RadioLib ^7.6.0`, `rweather/Crypto ^0.4.0`, `adafruit/RTClib ^2.1.3`, `melopero/Melopero RV3028 ^1.1.0`; its `platformio.ini` adds the sensor libraries and a git commit for RadioLib). A reproducible build needs exact ones, so a bump re-resolves them and brings across what moved — and keeps our pins **first** in `lib_deps`, which is what makes MeshCore's own range resolve to the RadioLib we chose. The RadioLib line is `[libs].radiolib`, read by `${libs.core}` and by `${libs.scanner}`: the scanner's list is SPI, Wire and that one line, and PlatformIO installs `lib_deps` per environment, so neither list can reach the other's build |
| 3 | **`boards/rak4631.json`**, vendored byte for byte | PlatformIO reads `boards_dir` before it clones a dependency, so `board = rak4631` cannot name a file inside MeshCore (`boards/README.md`). A bump re-copies upstream's and `diff`s it — the `ldscript` line in it is what decides place 7 |
| 4 | **`tools/pio/`** — what each script reaches for inside the pinned tree | `meshcore_private_libs.py` compiles `lib/ed25519` out of the checkout (a *private* library of upstream's project, which a package consumer does not get); `meshcore_variant.py` compiles `variants/rak4631/variant.cpp` out of it for every role of this board — the Adafruit core at this pin has no `WisCore_RAK4631_Board` variant of its own, so the Arduino pin map is upstream's — and `RAK4631Board.cpp` as well for a role that compiles MeshCore, which it reads off `lib_ignore`; it refuses to run if `MC_VARIANT` is set, because that would compile upstream's `target.cpp` beside ours; `meshcore_uf2.py` runs `bin/uf2conv/uf2conv.py` out of it with the nRF52840 family id `0xADA52840`. Those last two are why **the scanner environment has MeshCore in `lib_deps` and ignores it**: it compiles not one line of upstream's, but it needs the checkout on disk for the pin map and the converter, so a bump that moves either breaks both images and not one. All three raise with this page's name in the message if the pin moves what they reach for, so they fail loudly rather than quietly. `buildinfo.py` and `linkmap.py` reach for nothing of upstream's |
| 5 | **every place that cites a line of upstream's at the old pin** | a comment in `src/` that names a file and a function of MeshCore's, as the convention asks (`tasks/README.md`), and anything else that spells the pin out. `grep -rn "<the old pin's first eight characters>"` finds them: `src/roles/repeater/app/TwinOakMesh.h` (the eleven overrides, each with the upstream declaration and definition it matches - the eleventh is `savePrefs()`, which exists so that a first boot writes `/prefs.json` once: a bump that changes `MyMesh::savePrefs()`, or `CommonCLI::savePrefs`'s remove-then-write of that file, or its zeroing of `advert_interval` below sixty minutes, is re-read against `docs/ARCHITECTURE.md` §4a; `Mesh::onRecvPacket`'s peer loop by line; and, for the clock, the four places upstream puts our timestamp into a packet and the advert timers that say when upstream's own periodic advert can first fire), `src/roles/repeater/main.cpp` (upstream's `main.cpp`, which ours is a copy of minus the display, Ethernet and the other platforms), `src/roles/repeater/app/FirstBoot.h` (`CommonCLI::loadPrefs` and the two files it reads), **`src/roles/repeater/firstboot/Profile.{h,cpp}`** (the first-boot profile: the rule that decides whether a reply means "the line was taken", which is read off **every** acceptance and refusal `CommonCLI.cpp`'s `set`, `sensor set`, `password` and `region` branches and `MyMesh.cpp` can produce - a bump that reworded one of them is a profile line judged refused, which shows up as `sensor get firstboot.state` saying `partial` and not as a corrupted preference; the key `path.hash.mode`, its range of 0 to 2 and `MyMesh::sendSelfAdvertisement`'s `path_hash_mode + 1`, which is what makes mode 1 two bytes; the key `dutycycle`, its range of 1 to 100, its `airtime_factor = (100 / dc) - 1` and the `OK - <n>.<n>%` it answers with - the profile's second line is that key, the reply rule has to accept that reply, and `CommonCLI::loadPrefs`'s clamp of `airtime_factor` to 9.0 is what makes 10 % the lowest a unit can be set to (`docs/ARCHITECTURE.md` section 7; the band's own ceiling is in `src/common/bands/Bands.h`); **the Danish standard's four keys** (p3.5-05), each with the branch that takes it, its range and upstream's own default - `agc.reset.interval`, whose preference is **seconds / 4** in a `uint8_t` (`CommonCLI.h:51`) and whose reply therefore says `OK - interval rounded to <4n>`, which the reply rule has to accept; `loop.detect`, whose four words are the only values taken and whose `moderate` is the `max_loop_moderate` row of `MyMesh.cpp` (a bump that re-tunes that row changes what `moderate` means without changing the key); `flood.max.unscoped`, its ceiling of 64 and `mesh::isFloodHopLimitExceeded`'s test of `ROUTE_TYPE_FLOOD` in `src/helpers/RoutingPolicy.h`; and `flood.advert.interval`, whose range of 0 or 3-168 hours has to contain 60-85 and whose `_callbacks->updateFloodAdvertTimer()` is what puts the derived period in force on the first boot itself. **At `repeater-v1.18.0` every radio key moved** (p3.5-06): `get radio`, `dutycycle`, `af`, `path.hash.mode`, `agc.reset.interval`, `tx`, `rxdelay`, `txdelay`, `direct.txdelay`, `int.thresh`, `cad`, `radio.rxgain` and `multi.acks` are `CommonRadioPrefs::handleCommand` in `src/helpers/CommonRadioPrefs.cpp`, reached from `CommonCLI::handleCommand` before anything else and matching the whole line; the text of every reply is unchanged, `set af` newly refuses a factor outside 0-9 (`ERROR: af must be 0-9`, where the pin before took it and `loadPrefs` clamped it on the next boot) and `agc.reset.interval` constrains the seconds to 0-1020 where the pin before wrapped them - so the profile's two lines there are the same lines with the same replies, and every citation of ours points at the new file; and `MyMesh::handleCommand`'s writing into the line in place, as `BoardCli` has it), `src/common/board/target.h` (upstream's `target.{h,cpp}`, and `MainBoard::reboot()`/`powerOff()`, which the clock keeper's file hangs on), `src/roles/repeater/link/BoardLink.cpp` (`NRF52Board`'s early capture of `POWER->RESETREAS` and `getResetReason()`, which reach the board object only because `RAK4631Board::begin()` calls `checkBootVoltage()`; `NRF52Board::reboot()`; and the Adafruit core's three DFU magic words at the *core's* own pinned commit `d541301`, `cores/nRF5/wiring.c`), `src/common/link/SerialTransport.h` (the core's `SERIAL_BUFFER_SIZE`, and the blocking rule of `Uart::write` / `availableForWrite` that is what "write what fits without blocking" means on this core), **`src/roles/repeater/link/BoardLink.cpp`'s `BoardCli`** (`MyMesh::handleCommand`'s signature and the fact that it writes into the line in place; the 160-byte reply buffer upstream's own console gives it, which three of its branches assume as a literal; and the four commands that never return), **`src/roles/repeater/app/Settings.{h,cpp}`** (`MyMesh::onPeerDataRecv`'s `client->isAdmin()` gate, which is the whole permission check for `sensor set` over the mesh; `sensor list`'s 134-byte cut-off, which is why our keys are in the order they are; and `EnvironmentSensorManager`'s own settings, which are none on this board but are still listed first), **`src/roles/repeater/clock/`** (`VolatileRTCClock`'s built-in date and its `tick`, `AutoDiscoverRTCClock`'s fallback and the four `*_success` flags it keeps private — which is why the clock keeper decides "an RTC answered" from the answer and not from a flag; upstream's `time <epoch>`, `clock sync` and `clkreboot` in `CommonCLI.cpp`, which the keeper polls for rather than patching; the core's `InternalFileSystem.cpp` geometry and `FILE_O_WRITE`, which decide the flash-wear arithmetic in `ClockKeeper.h`; and the core's startup loops and linker script, which are the whole of why there is no surviving-RAM copy), **`src/common/power/`** (upstream's own INA3221 and BME280 support in `EnvironmentSensorManager.cpp`, one of which applies a single shunt value to three channels and the other of which samples its part inside `querySensors` — both left out of the build, and `test/test_rails` fails a board environment that turns either on; `MyMesh::handleRequest`'s telemetry branch, whose two channel-1 fields `LPP_GET` has to reproduce around ours and whose `CayenneLPP` is sized `MAX_PACKET_PAYLOAD - 4`; `SensorManager`'s `TELEM_PERM_*` and `TELEM_CHANNEL_SELF`; `NRF52Board::getMCUTemperature`; `NRF52Board::checkBootVoltage` and `PWRMGT_VOLTAGE_BOOTLOCK`, which decide whether a low battery can stop this node booting at all — `tasks/phase-1/reports/06.md` answers that question and a bump re-reads it; **CayenneLPP 1.6.1's `addField` and its `isType` gate**, which `src/common/power/Telemetry.cpp` mirrors byte for byte and which silently refuses three of the types its own header names; and the Adafruit nRF52 core's `Wire_nRF52.cpp`, whose TWIM loops have no deadline), **`src/roles/repeater/app/TwinOakMesh.cpp`** (the mesh client's extra peer in front of upstream's: `Mesh::onRecvPacket`'s peer loop by line, the bound on the number of matches — `matching_peer_indexes[MAX_CLIENTS]` filled only by `MyMesh::searchPeersByHash` and read only with an index shifted back into its own range, and `MAX_CLIENTS` 20 via `ClientACL.h` winning over `MyMesh.h`'s own define — `mesh::LocalIdentity::calcSharedSecret` for the job's ECDH secret, **and the five sends** (`Mesh::createAnonDatagram`, `createDatagram`, `createTrace`, `sendDirect`, `sendFlood` by line, with `sendFlood`'s two-byte path-hash argument; the receiving repeater's `MyMesh::handleLoginReq`, `handleRequest` and `onPeerDataRecv` for what each reply's bytes are and that a login reply carries the repeater's timestamp; `RadioLibWrapper`'s `getPacketsRecv`/`getPacketsSent`/`getNoiseFloor`/`getLastRSSI`/`getLastSNR` for `MESH_COUNTERS`; and the `static_assert`s on `MAX_PACKET_PAYLOAD`, `CIPHER_MAC_SIZE`, `CIPHER_BLOCK_SIZE`, `PAYLOAD_TYPE_*`, `TXT_TYPE_*` and `TELEM_PERM_BASE`), **and the observation ring** (`MyMesh::onAdvertRecv`, which ours calls *first* so that upstream's neighbour table sees every advert as before; `Mesh::onRecvPacket`'s advert branch for the self-advert drop and the clamp of `app_data` to `MAX_ADVERT_DATA_SIZE`; `Dispatcher::checkRecv` by line for *why* the RSSI is noted in `logRx` and nowhere else — the `logRx` call, the score delay, `queueInbound` and `MAX_RX_DELAY_MILLIS`; `Packet::path_len`/`getPathHashSize`/`getPathHashCount`/`getPathByteLen` and `tryParsePacket`'s own refusal of a path over `MAX_PATH_SIZE`; and the size of the packet pool, `new StaticPoolPacketManager(32)` in `MyMesh`'s constructor, which is **not a macro** and is therefore held by a `static_assert` against a literal — a bump that resizes the pool makes the RSSI note table the wrong length and must fail there)), **`src/roles/repeater/meshclient/`** (`Packet.h`'s path-length encoding `(hash_size - 1) << 6 | hop_count` and `isValidPathLen`, `MeshCore.h`'s `PUB_KEY_SIZE`, `MAX_PATH_SIZE`, `MAX_PACKET_PAYLOAD` and `MAX_ADVERT_DATA_SIZE`, `Identity.h`'s `isHashMatch`, and — for the reply — `TxtDataHelpers.h`'s `TXT_TYPE_CLI_DATA`/`TXT_TYPE_PLAIN` and the trace's hash-size-as-exponent of `Mesh::onRecvPacket`, each `static_assert`ed or cited against the pin), **`src/roles/repeater/meshclient/SendCeiling.h` and `docs/ARCHITECTURE.md` §7's two answers about upstream's transmit path** (`Dispatcher::sendPacket` and `checkSend` by line — one outbound queue and one `tx_budget_ms` for our packets and every forwarded one alike; `getAirtimeBudgetFactor`, which is what makes the duty cycle a half; the budget decremented by measured airtime on a completed send, and the `MIN_TX_BUDGET_RESERVE_MS` push-out; `PacketQueue::get`'s priority-then-insertion order; the retransmit priorities of `Mesh::onRecvPacket` and `routeRecvPacket` against those of `sendDirect` and `sendFlood`, which together are the reason a client send cannot delay a forward by more than its own airtime; and the packet pool's size again. A bump that re-prioritises the queue or changes the budget is re-read here, because the answer "repeating comes first" is read out of those lines and nowhere else), `docs/ARCHITECTURE.md` §11 (every claim with its line) — and the comments under `test/` that cite the pin the same way (`test_rails`, `test_settings`, `test_firstboot`). **No test and no tool of ours asserts the tag or the pin as a literal**, which is what makes a harmless bump green: *What a bump costs today* below |
| 6 | **`build_as_lib.py`'s defines** | it is upstream's own script, run by its `library.json`, and it decides which of MeshCore's sources are compiled. We set `BUILD_EXAMPLE=simple_repeater` (which adds `examples/simple_repeater/*.cpp`) and, after it — the script reads the defines in order — `EXCLUDE_FROM_EXAMPLE=main.cpp`. We deliberately set neither `MC_VARIANT` (it would compile upstream's `target.cpp`, the file `src/common/board/target.cpp` replaces; the two variant files we do need are place 4's) nor `DISPLAY_CLASS` nor `MC_UI_FLAVOR`. A rename there, or an example renamed, is a build that compiles the wrong set of files or none |
| 7 | **the application region in the linker script** | `boards/rak4631.json` names `nrf52840_s140_v6.ld`, which the Adafruit core fork provides, and its `FLASH` region (`ORIGIN = 0x26000, LENGTH = 0xED000 - 0x26000`) is where an image may live and nothing else. `tools/pio/buildinfo.py` copies the script the link used and the release tooling reads `app_region` out of that copy, with tests that refuse a byte outside it — so a move is caught. But the manifest's `app_region` then **changes**, and the Walter writes images at that address: a bump that moves the region is a bump whose report says so, loudly, because the other box has to be told |
| 8 | **what our sources take from upstream's headers** | `src/common/board/CoreHeaders.h` (the Arduino core's own two, which `UpstreamHeaders.h` includes and a file that needs no MeshCore stops at), `src/common/board/UpstreamHeaders.h`, `src/roles/repeater/app/UpstreamMyMesh.h` and `src/roles/scanner/ScannerHeaders.h` (RadioLib, SPI, the watchdog registers and Semtech's spectral-scan patch, for the role that compiles no MeshCore) are the only files of ours that include an upstream header - with one more that is not upstream's at all and is read on a **RadioLib** bump rather than a MeshCore one: `src/roles/scanner/vendor/sx126x_spectrum/SX126xSpectrumScanner.h`, the vendored host class of the `scope` sweep plugin, which reaches four members of `SX126x` that are public only under `RADIOLIB_GODMODE` and are protected in 7.8.0 (`mod`, `readRegister`, `setRx`, `setBufferBaseAddress`; the subclass at the top of that file is the whole of the adaptation, and its own header comment says so), and calls `uploadPatch`, `calibrateImageRejection` and `setRxBandwidth` by their 7.8.0 signatures, and each does it as system headers, so that upstream's warnings do not fail our `-Werror` (`docs/ARCHITECTURE.md` §11). A bump that adds a header we need is a line in one of them — `<Adafruit_BME280.h>` is there because this firmware drives that part itself, and `<CayenneLPP.h>` arrives through upstream's own `SensorManager.h`. Two more are named there because `src/common/` may not include a role's header and so no longer reaches them through `MyMesh.h`: `<helpers/IdentityStore.h>` for the `FILESYSTEM` macro and the core's `<InternalFileSystem.h>` for `InternalFS` — a bump that moves either is a compile error in `src/common/power/` and `src/common/board/target.cpp`. And upstream's `MyMesh.h` includes `<target.h>` *unqualified*, which is what lets ours shadow theirs by include order — a bump that qualifies that include, or adds a `target.h` of its own earlier on the path, is a build that constructs two boards |

And the rails that must survive a bump, because a bump is exactly when one gets
quietly lost: nothing the mesh says logged as text (`MESH_PACKET_LOGGING` and
`MESH_DEBUG` stay off whatever upstream's examples do), not one file of
upstream's tree edited, no default password on the air, and the native tests
and the `spec/` fixtures green.

## What of MeshCore this firmware stands on

Since phase 1's first task, our application is `TwinOakMesh : public MyMesh`
(`src/roles/repeater/app/`), our `main.cpp` constructs it, and `src/common/board/target.cpp` is the
board. What it needs of upstream is a short list of **public and protected
virtual** members —
`handleCommand`, `getNodePrefs`/`savePrefs`, the `sensors` seams,
`onAdvertRecv`, `logRx`, `logTx`, `searchPeersByHash`, `getPeerSharedSecret`,
`onPeerDataRecv`, `onPeerPathRecv`, `onTraceRecv`, `onAnonDataRecv`,
`sendSelfAdvertisement`, `MainBoard::reboot`/`powerOff`, and `Mesh`'s own
`createAnonDatagram`, `createDatagram`, `createTrace`, `sendDirect`,
`sendFlood` - and one call rather than an override, `MainBoard::loop()`, which
upstream's `main.cpp` makes once a pass since `repeater-v1.18.0` and ours
makes too, because ours is a copy of theirs (place 5). Three of those entries are the clock keeper's (§5 of that file):
`onAnonDataRecv` and `sendSelfAdvertisement`, which join `onPeerDataRecv` and
`onPeerPathRecv` as the places a packet of ours can carry our clock, and
`reboot`/`powerOff`, which its file is written in front of.
`docs/ARCHITECTURE.md` §2 has the table and the one thing in it
that cannot fail loudly: the **order** `Mesh::onRecvPacket` tries matched peers
in, which our extra peer sits in front of. A comment above that override cites
the function and the pin, which is what place 5 of the table is for.

## What a bump costs today

**Nothing, when nothing of upstream's that we rely on moved.** Outside
`[meshcore]` and the pin's history at the bottom of this page — and the
workflow's own commit writes both — no file of this repository states which
MeshCore release this is, so a bump that compiles for every board and passes
the native tests arrives **green**, and a person merges it. No session, no
task, no branch of ours. That is tier one, and it is meant to be the common
case.

The two files that used to name the release say what they mean instead
(`tasks/phase-3.2/reports/01.md`):

- `test/test_version/test_version.cpp` —
  `Version.ThisBuildSaysThePinOfPlatformioIni` and
  `Upstream.ThisBuildSaysThePinOfPlatformioIni` build what they expect out of
  `TWINOAK_MESHCORE_TAG` and `TWINOAK_MESHCORE_PIN`, the same two defines the
  code reads, and assert that those defines *arrived*: an empty tag, or a pin
  that is not forty hex digits, fails — which was the tests' whole point. The
  shape itself, `twinoak-<role> <version> on MeshCore <tag> (<8 hex>)` and
  `<tag>+<8 hex>`, is held by `Version.TheShapeDoesNotDependOnTheRelease` with
  a made-up tag and pin (`repeater-vX.Y.Z`, `0123abcd…`) through
  `formatVersion`/`formatUpstream` — the seam `versionString()` and
  `upstreamString()` take their inputs through.
- `tools/release/test_release.py` — the synthetic `buildinfo.json` its
  fixtures are built from says `repeater-vX.Y.Z` too, and
  `test_the_upstream_pin_is_the_builds` holds that the manifest's `upstream` is
  whatever the build directory it was assembled from said.

What is left of the old pin in this repository is **comments** that cite a line
of upstream's (place 5 above) and `spec/`, which is a copy and is never edited
here. A comment is re-read at the new tag by the session of a red bump; it
fails nothing by itself.

**What a person reads before merging a green bump.** Green says the bump
compiled for every board, the native tests still pass, `spec/` still matches
its manifest and the release still assembles. It does not say that a define
upstream renamed, a library range they moved or a flag they added has been
brought across — the eight places of the table above are exactly the things a
compiler may not catch, and `tasks/sync/TEMPLATE.md` is that list as a
checklist. So the review of a green bump is: read the pull request body (it is
upstream's own commit list between the two pins) against those eight places,
and merge. Anything it turns up that matters is a follow-up of the phase that
owns it — or, if upstream moved the wire, tier three, which is a reading task
before it is a code change.

**A red bump is tier two**, one command, and a session: below.

## Tier one — the weekly pull request (automatic)

[`.github/workflows/upstream.yml`](../.github/workflows/upstream.yml), Monday
mornings at 06:29 UTC and on demand:

1. `tools/upstream-check.ps1` reads `git ls-remote --tags` of the URL
   `[meshcore].url` names and compares the newest `repeater-vX.Y.Z` with the
   pin. Nothing newer: it says so in the run summary and stops.
2. Something newer: a branch `upstream/<tag>` whose one commit moves `pin` and
   `tag` and adds the row to *The pin's history* at the bottom of this page,
   pushed, and a pull request **MeshCore \<tag\>** whose body is
   `git log --oneline <old>..<new>` of upstream's tree — what changed, in their
   words.
3. CI runs on it like any pull request: `tools/verify.ps1` and nothing else, so
   **green means the bump built for every board, the native tests still pass,
   `spec/` still matches its manifest and the release still assembles.** A
   person merges it. Red is tier two.

Only one bump is open at a time: if `upstream/<tag>` is already on `origin`,
the workflow leaves it alone and says which pull request the open bump is.

**One click before CI runs.** The pull request is opened by
`github-actions[bot]` with the workflow's own `GITHUB_TOKEN`, and GitHub holds
workflows on such a pull request rather than starting them — its guard against
a workflow that sets itself off. So the run waits for **Approve and run** on
the Actions tab, or

```bash
gh api -X POST repos/twinoak/twinoak.meshcore.repeater-firmware/actions/runs/<id>/approve
```

and that click is the same click as reading the bump. What would remove it is a
personal access token in a repository secret for `gh pr create` to use instead
of the built-in one; that is Thomas's to decide, and one button a month is
cheaper than a credential to rotate.

You can ask the same question yourself, at any time, without a runner:

```powershell
.\tools\upstream-check.ps1                        # against the pin
.\tools\upstream-check.ps1 -Tag repeater-v1.16.0  # as if the pin were two tags back
```

And the whole workflow can be rehearsed without waiting for upstream: *Run
workflow* with `pin_tag = repeater-v1.17.0` pushes a throwaway base branch
pinned at that tag and opens the real pull request on top of it, so the diff and
the commit list are the ones a real bump would show. Close the pull request and
delete both branches afterwards — a rehearsal is a test, not a bump.

## Tier two — a red bump is one command

```powershell
.\tools\new-sync-task.ps1 -Tag repeater-v1.18.0 -Pr 12 -LogUrl <the failing job>
.\tasks\run-all.ps1 -Phase sync
```

It writes `tasks/sync/NN-meshcore-<tag>.md` from
[`tasks/sync/TEMPLATE.md`](../tasks/sync/TEMPLATE.md) — the tag, the pull
request, the failing log, and the standing instructions: read what upstream
changed, re-read the eight places, bring the code to the new API, keep the
rails, verify locally, and say in the report what upstream changed that
mattered. It adds the row the driver's table needs and changes nothing else:
the pin was already moved by the branch the workflow pushed, and the session
works **on that branch** (`upstream/<tag>`) rather than branching from `main`.

## Tier three — a change to the wire protocol

Rare, and not a firmware change first. If MeshCore changes a **packet layout**,
the login exchange, or what the CLI replies, then it is not only this firmware
that has to be brought across:

1. the **mesh client** of phase 2 is a port of the agent firmware's
   `src/mesh/AgentMesh.cpp` (`spec/SYSTEM.md` S-4), and its reading of those
   bytes is held against MeshMaster's own fixtures. The dialect is the .NET
   agent's, and a firmware that invents a second one is a bug however well it
   works;
2. so the order is **meshmaster.dk first** — where the new reading is
   established and tested — then `twinoak/meshmaster-agent-firmware`, then
   here. `spec/proposals/03` is the shared half of it.

Which means tier three is a **reading task before it is a code change**: what
moved, who else reads it, and what the server already says about it. The
specification changes where it lives (`spec/README.md`) and is copied here
afterwards; nothing under `spec/` is edited in this repository.

## Reading the firmware you build against

After a build, the pinned MeshCore is on disk:

```
.pio/libdeps/repeater_rak4630_single/MeshCore/
```

That checkout — not a memory of it, not a transcription in a task file, not
this page — is what a claim about MeshCore is checked against (`tasks/README.md`,
`CLAUDE.md`). `git log`, `git show` and `grep` all work in it: it is a real
clone at the pinned commit, and `git status` in it is empty after a build
because nothing here ever writes to it.

## The pin's history

Newest first. A bump's own commit adds its row, so this table is written by
`.github/workflows/upstream.yml` and not by hand.

| Tag | Commit | Moved on | How |
|---|---|---|---|
<!-- upstream.yml inserts the newest row directly under this line -->
| `repeater-v1.18.0` | `81dbae59` | 23 October 2026 | the first real bump; read and walked by a session (p3.5-06). The weekly workflow opened it on 19 October as pull request #26, whose CI waited for *Approve and run* and was never approved; p3.5-06 merged the workflow's commit into its own branch, re-read the eight places, walked the board and merged (`tasks/phase-3.5/reports/06.md`) |
| `repeater-v1.17.1` | `d9296435` | 11 October 2026 | the first pin, chosen because the field repeaters and the agent firmware run it (`tasks/phase-0/reports/01.md`) |
