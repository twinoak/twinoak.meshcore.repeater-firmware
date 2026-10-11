# 01 — The toolchain

**Depends on:** nothing.
**Spec:** [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-2, S-10, S-11; [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §1–§3; [`tasks/README.md`](../README.md).
**Size:** one session.
**Model:** claude-fable-5-1

## Goal

`pio run` builds MeshCore's **stock `simple_repeater` example** for the RAK4630
from MeshCore as a pinned dependency — not one file of upstream's edited, none
of ours on the board yet — and `pio test -e native` runs a test. One script,
`tools/verify.ps1`, does everything CI does, and CI does nothing but run it.

It is Fable's because every later task inherits three decisions made here: how
the package is consumed on an nRF52, how the board is named, and what "verified"
means in this repository.

## Read first

- The agent firmware's `platformio.ini`, `tools/pio/meshcore_private_libs.py`,
  `boards/README.md` and `.github/workflows/ci.yml` at
  `51faafb427cba41b0de9983736b6621bc155f829` (RUNBOOK §0 says how to read a
  sibling repository). They solved this for ESP32; the shape is the same.
- MeshCore at the pin, once the first build has cloned it: `platformio.ini`
  (`[arduino_base]`, `[nrf52_base]`, `[sensor_base]`), `variants/rak4631/`,
  `build_as_lib.py`, `library.json`, `boards/rak4631.json`.
- NoiseScope's `platformio.ini` in the hardware repository
  (`software/firmware/noisescope-rak4630/` at
  `b798aab043281fa4c9b17802a164fa94be12e206`): a build for this exact stamp
  that already works, pinned to the same Adafruit core fork.

## Deliverables

- **`platformio.ini`.**
  - `[meshcore]`: `pin = d92964352441e53b93e8667b802e04f6e072b39e`,
    `tag = repeater-v1.17.1`, the URL, and where PlatformIO puts it. This is
    the commit the agent firmware pins and the field repeaters run.
  - `[libs]`: every library at an **exact** version. Start from the agent
    firmware's resolved list (same pin) and add what an nRF52 build needs
    (upstream's `[nrf52_base]`: the Adafruit nRF52 core fork at the commit
    upstream pins, `CustomLFS`). Where a library cannot be taken at the commit
    upstream names, say why, as the agent firmware does for RadioLib.
  - One board environment, **`repeater_rak4630_single`** (`<role>_<board>`,
    ARCHITECTURE §8): upstream's flag blocks for the RAK4631 copied in with a
    comment naming their source — they use paths relative to MeshCore's root
    and cannot be included — `-D MC_VARIANT=rak4631`,
    `-D BUILD_EXAMPLE=simple_repeater`, and the Danish channel as build
    defaults: **869.618 MHz, 62.5 kHz, SF8, CR8, +22 dBm**.
  - Left out, each with a comment: `-w` (GCC's `-w` silences `-Werror`
    whatever the order — the agent firmware's report found that),
    `MESH_PACKET_LOGGING`, `MESH_DEBUG`, a display class, and the GPS
    (`Serial1` is the link to the Walter on our adapter; upstream's own
    RS232-bridge environments show how the GPS is left out).
  - `build_src_flags = -Wall -Wextra -Werror` for `src/` only.
  - `[env:native]`: `platform = native`, googletest, `-std=c++17 -Wall -Wextra
    -Werror`, `test_build_src = yes` with a `build_src_filter` that starts
    empty and lists portable files one by one.
- **`boards/rak4631.json`**, vendored unedited from the pin, and
  `boards/README.md` saying why it is the one vendored file of upstream's.
- **`tools/pio/`**: whatever script the package consumer needs to reach
  MeshCore's private `lib/ed25519` (the agent firmware's
  `meshcore_private_libs.py` is the model) and the UF2 / DFU-package step that
  upstream attaches to its own project with `create-uf2.py`. Nothing copied
  from the pinned tree except where PlatformIO leaves no other way, and each
  such file says so and raises loudly if a bump moves what it reaches for.
- **`src/version.{h,cpp}`** — the one source file of ours in this task: the
  string `twinoak-repeater <version> on MeshCore <tag> (<pin8>)`, with
  `<version>` from `-D TWINOAK_FW_VERSION` (default `0.0.0-dev`). Host-portable,
  with a native test. Nothing calls it on the board yet.
- **`tools/verify.ps1`** — runs on Windows PowerShell and on `pwsh` under
  Linux, and is the definition of *verified* from here on:
  1. `python spec/tools/specsync.py verify spec`
  2. `python spec/tools/linkref.py check spec/fixtures/link/link-v1.json`
  3. `pio run -e <env>` for **every** board environment (read them from
     `platformio.ini`; no second list to forget)
  4. `pio test -e native` — with the machine's own `g++` when there is one,
     otherwise in the container of RUNBOOK §2
  5. every step's output to `out/verify/<step>.log`, a one-line summary per
     step at the end, exit code non-zero when anything failed.
  It finds a `pio` that is not on PATH the way the driver's preflight does, and
  a Python the same way: the one PlatformIO itself runs on is always there. It
  takes `-Only <step>` for a quick re-run of one step and nothing else clever.
- **`.github/workflows/ci.yml`** — on `pull_request` and on `push` to `main`
  (not on branch pushes: one run per commit), `concurrency` per ref, PlatformIO
  Core pinned to the version this machine runs, the PlatformIO and pip caches
  keyed on `platformio.ini`, and **one step that matters: `pwsh
  tools/verify.ps1`**. The images (`.hex`, the DFU `.zip`, `.elf`) kept as an
  artifact for 14 days.
- **Branch protection on `main`**, as the agent firmware has it: a pull request
  required with 0 approvals, admins not enforced (the driver's notes commit
  pushes to `main`), no force pushes, no deletions, the CI job required. If
  `gh` has no right to set it, say so under *Not verified here*.
- **`README.md`**: "State" replaced by "Building it" — the three commands, and
  what a clean first build downloads.

## Acceptance criteria

- [ ] `tools/verify.ps1` is green on this machine from a clean clone: the build log shows RAM and flash use for `repeater_rak4630_single` (quote them in the report), and the native test ran.
- [ ] The build output includes an image the Walter can flash over SWD (`firmware.hex`) and a DFU package for a laptop; `out/` keeps them.
- [ ] Not one file under `.pio/libdeps/` is modified by the build or by a script (`git -C` it, or hash before and after).
- [ ] A deliberate warning in `src/version.cpp` fails the build, and is removed again; the report quotes the compiler's line.
- [ ] Changing one byte of a file under `spec/` makes `tools/verify.ps1` fail at step 1 with the file's name, and is reverted; the report says so.
- [ ] CI is green on the pull request, ran once for the commit, and ran `tools/verify.ps1` and nothing parallel to it.
- [ ] The report lists, as the agent firmware's bootstrap report did, **what a package consumer does not get on an nRF52** and how each was arranged.

## Not verified here (Thomas, with a board)

Nothing is flashed: the image is upstream's stock repeater built with our
flags. The first thing worth flashing is phase 1's.

## Out of scope

Our application (`main.cpp`, the subclass, the board target) — phase 1, task
01. The release — task 02. The upstream watch — task 03. Any environment but
the one named.
