# 01 — Our application

**Depends on:** phase 0.
**Spec:** [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-1, S-2, S-12, S-13; [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §1–§3, all of it.
**Size:** one session.
**Model:** claude-fable-5-1

## Goal

The image stops being upstream's example and becomes ours — **without a line of
upstream's changing**. `main.cpp` is ours; `the_mesh` is a `TwinOakMesh`, a
subclass of upstream's `MyMesh` compiled from the pin; the board target is ours.
It does nothing new yet: every override calls its base. What this task
delivers is the proof that the design of ARCHITECTURE §2 can be built, a place
for everything after it to hang, and a firmware that never has a default
password.

**This is the task that is allowed to say the design is wrong.** If something
ARCHITECTURE §2 relies on is not so at the pin — a function that is private, a
call that is not virtual, a define `build_as_lib.py` does not honour — do not
patch upstream and do not copy `MyMesh.cpp` into `src/`. Stop (RUNBOOK §5) and
write down exactly what does not hold, where in upstream's source, and the
smallest change of design that would.

## Read first, in the pinned checkout

`examples/simple_repeater/main.cpp`, `MyMesh.h`, `MyMesh.cpp` (the constructor,
`begin`, `loop`, `searchPeersByHash`, `getPeerSharedSecret`, `onPeerDataRecv`,
`onPeerPathRecv`, `onAdvertRecv`, `handleCommand`); `src/Mesh.h` and
`Mesh::onRecvPacket` in `src/Mesh.cpp`; `variants/rak4631/target.{h,cpp}` and
`RAK4631Board.{h,cpp}`; `src/helpers/CommonCLI.cpp` (`loadPrefs`, the `sensor`
commands, `password`); `src/helpers/SensorManager.h` and
`sensors/EnvironmentSensorManager.{h,cpp}`; `build_as_lib.py`.

## Deliverables

- **The build takes upstream's example without its `main.cpp`.**
  `-D BUILD_EXAMPLE=simple_repeater` with `-D EXCLUDE_FROM_EXAMPLE=main.cpp`
  (and whatever else of the example is display-only), upstream's `MyMesh.h` on
  the include path, and **no `MC_VARIANT`**: the variant's `target.cpp` is the
  file we replace. The two upstream variant sources still needed
  (`RAK4631Board.cpp`, the Arduino `variant.cpp`) are compiled from the pinned
  checkout by a `tools/pio/` script, not copied.
- **`src/board/target.{h,cpp}`** — ARCHITECTURE §3: `board` (a subclass of
  `RAK4631Board`; `getBattMilliVolts()` stays upstream's until task 06),
  `radio_driver`, `radio_init()`, `radio_new_identity()`, `rtc_clock`
  (auto-discovering over a volatile clock, as upstream), and `sensors`, a
  `TwinOakSensors : EnvironmentSensorManager` with the settings seam overridden
  and one key in it for now: `fw.version` (read-only, our version string). No
  GPS, no display, `Serial1` untouched. I2C at 100 kHz.
- **`src/app/TwinOakMesh.{h,cpp}`** — `class TwinOakMesh : public MyMesh`, with
  every virtual ARCHITECTURE §2 lists overridden **with the `override`
  keyword** and a body that calls the base: `searchPeersByHash`,
  `getPeerSharedSecret`, `onPeerDataRecv`, `onPeerPathRecv`, `onTraceRecv`,
  `onAdvertRecv`, `logRx`, `logTx`. Above each, a comment citing the upstream
  file, function and pin it interposes on. The compiler accepting `override`
  on all of them is the first half of the proof.
- **`src/main.cpp`** — upstream's `setup()` and `loop()` at the pin, minus the
  display, Ethernet and the SenseCAP button, constructing `TwinOakMesh`. The
  USB serial console stays exactly as upstream has it. Cite the upstream file
  and pin at the top, so a bump knows what to re-read.
- **No default password** (S-13). On a boot that finds no saved preferences —
  establish how to tell from `CommonCLI::loadPrefs` at the pin — the firmware
  draws a random 16-character admin password from the radio's RNG, sets it in
  the node's preferences (`getNodePrefs()`, `savePrefs()`) and never prints
  it. The build's `ADMIN_PASSWORD` define, which upstream's constructor needs,
  is a value nobody can use: say in the report what it is and why it never
  survives first boot. The generator (alphabet, length, from an RNG interface)
  is portable and has a native test; that a login with upstream's default
  fails is a bench line.
  A person at the USB port sets a real one with `password <new>`, which the
  console allows; the README's rails say so.
- **`ver` stays upstream's.** MeshMaster reads a repeater's firmware from it
  and has rules that depend on the upstream version. Our version is
  `sensor get fw.version` and, later, the link's `HELLO`.
- **A rails test** (native, like the agent firmware's `test_tls_policy`): it
  reads `platformio.ini` and walks `src/` and fails on `MESH_PACKET_LOGGING`,
  `MESH_DEBUG` set to anything, on any file of `src/` that prints a packet's
  payload, and on a board environment that defines `ADMIN_PASSWORD` as
  `password`.
- **`docs/ARCHITECTURE.md`**: a new section, *Proved by task p1-01*, listing
  each claim of §2 and §3 with the evidence — the upstream line, or the build
  output — and every correction made to the sections above it.

## Acceptance criteria

- [ ] `tools/verify.ps1` green. The report quotes RAM and flash use and compares them with phase 0's stock build.
- [ ] The linker map (or `nm`) shows `MyMesh::onPeerDataRecv`, `MyMesh::handleCommand` and the rest coming from the pinned checkout's `examples/simple_repeater/MyMesh.cpp`, and `setup`/`loop` from our `src/main.cpp`; upstream's `main.cpp` and `target.cpp` are not in the link.
- [ ] `git status` inside `.pio/libdeps/repeater_rak4630_single/MeshCore` is clean after the build.
- [ ] Every override compiles with `override`; the report lists them with the upstream declaration each matches.
- [ ] The native tests cover the password generator (length, alphabet, that two draws differ) and the rails test passes and fails when it should (show one deliberate failure, then remove it).
- [ ] `docs/ARCHITECTURE.md` has its *Proved by* section.

## Not verified here (Thomas, with a board)

Add these to `docs/bench.md`, which this task starts, under "1. The repeater
alone on USB":

- Flash the UF2 (double-tap reset on the adapter's USB-C), open the console at
  115200: it prints its ID; `ver`, `board`, `get radio` (869.618, 62.5, 8, 8),
  `sensor list` shows `fw.version`.
- It hears adverts (`neighbors` after a few minutes on the roof antenna) and
  repeats (a message through it from a phone).
- A phone login with the password `password` is refused; after
  `password <something>` on the console that one is accepted.
- `INFO.VARIANT` of the module, for the Walter's flasher (hardware repository,
  `BENCH-BREADBOARD.md` caveat 6): record it here.

## Out of scope

The link (02, 03), the clock (04), anything in the overrides but a call to the
base (phase 2), the INA3221 (06), the image descriptor (phase 3).
