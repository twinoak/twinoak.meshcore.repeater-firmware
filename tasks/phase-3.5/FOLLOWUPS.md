# Phase 3.5 — follow-ups

Items for a later task of this phase are tagged with it. 05 leaves its `[06]` items here, for the bump
to settle at the new pin. The review of 01–06 goes below them when it is written.

## From 02 (22 October)

- [ ] **[reviewer] The SX1262's refusal is worth telling the plugin's author** (MeshCore
  [#3578](https://github.com/meshcore-dev/MeshCore/issues/3578), sleeply4cat). On one RAK4630
  (`rom` `SX1261 V2D 2D02`), after a `0x9B` sweep that ended below about 205 MHz the chip refuses an
  image calibration reaching more than about 20 MHz above the sweep's end — `IMG_CALIB_ERR` in
  `GetDeviceErrors`, *failure to execute* in the status, and every calibration after it refused too
  until a hardware reset. The fork's own scanner never meets it: one segment, calibrated in
  `setSweep()` before any sweep. Ours reloads the plugin once per pass over 150–960 MHz
  (`armScopeSegment`). Whether the plugin's retune leaves the synthesizer's band selection behind
  is a question for whoever has the assembler (`spectrum.asm`); the probe table is in
  `reports/02.md`, and it is one board's.
- [ ] **[06] The scope's arm at the new pin.** `armScopeSegment` leans on RadioLib 7.8.0's
  `SX1262::setFrequency(float, bool skipCalibration)` and on its `calibrateImageRejection()` (the
  two `static_assert`s in `src/roles/scanner/Radio.cpp` hold the codes); a RadioLib bump under the
  MeshCore bump is where either could move. `bench walk 9` at the new pin is the check, 9.9 in
  particular.

## From 04 (22 October)

- [ ] **[reviewer] A release's bytes are not reproducible from its source.** Two builds of the
  **same** commit differ: a clean rebuild of `out/build/repeater_rak4630_single/firmware.bin`
  differed from the build before it in **3 bytes at 0x64D3C**, inside
  `"Oct 23 2026" "00:05:09"` → `"…" "00:08:32"` — RadioLib 7.8.0's own info string,
  `RADIOLIB_VALUE(__DATE__) " " RADIOLIB_VALUE(__TIME__)` (`src/BuildOpt.h:656`); the string is
  in the linked image, and whoever fixes this finds out why.
  `out/bench/flash.log` has four distinct
  repeater `application_sha256` for the evening of 22 October, in which no input to the repeater
  image changed. So a manifest's SHA-256 is per *build* and not per *tag*
  (`spec/FIRMWARE-MANIFEST.md`), and nobody can rebuild a release and compare it to the
  signed one. Found by p3.5-04 while answering "is the board running this commit's image?" -
  nothing of that task depends on it. What it would cost to fix is a build flag or a
  `--defsym`/`-ffile-prefix-map`-style stamp over RadioLib's two macros, which is a decision
  about a pinned library's source and therefore the reviewer's.
