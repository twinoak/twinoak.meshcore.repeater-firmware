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

## From 05 (23 October)

- [ ] **[06] `agc.reset.interval`'s handler has moved, and gained a constraint.** At the pin
  `d9296435` the `set`/`get` pair is in `CommonCLI::handleSetCmd`/`handleGetCmd`
  (`src/helpers/CommonCLI.cpp:474-477`, `:820-821`); at `repeater-v1.18.0` it is
  `CommonRadioPrefs::handleCommand` (`src/helpers/CommonRadioPrefs.cpp:163-173`), reached from
  `CommonCLI::handleCommand` *before* `handleSetCmd` and matching the whole line
  (`memcmp(command, "set agc.reset.interval ", 23)`). **The line this firmware sends and the
  reply it gets are the same** - the preference is still seconds/4 in a `uint8_t` and the reply is
  still `OK - interval rounded to 4` - so nothing of ours had to move; what 1.18 adds is
  `secs = constrain(secs, 0, 255 * 4)`, which at the old pin wrapped instead (`atoi(x) / 4` into a
  `uint8_t`, so `set agc.reset.interval 1024` stored 0). 4 is unaffected either way. What the bump
  owes this: re-point the citation in `src/roles/repeater/firstboot/Profile.cpp` at the new file
  and line, and the same citation in `docs/upstream.md` place 5, which already says both.
- [ ] **[06] A transport code of 0 is un-scoped at 1.18, and `flood.max.unscoped 15` does not
  hold it.** `01f9293b` (*treat transport code 0 the same as un-scoped*) adds
  `if (packet->transport_codes[0] == 0) return &wildcard;` to `RegionMap::findMatch`
  (`src/helpers/RegionMap.cpp:191`). At the pin such a packet matches no region, so
  `recv_pkt_region == NULL` and `MyMesh::allowPacketForward` drops it
  (`examples/simple_repeater/MyMesh.cpp:437-443`); at 1.18 it matches the wildcard and is
  forwarded. Its route type is still `ROUTE_TYPE_TRANSPORT_FLOOD`, and
  `mesh::isFloodHopLimitExceeded` applies `flood_max_unscoped` only to `ROUTE_TYPE_FLOOD`
  (`src/helpers/RoutingPolicy.h:19`, byte for byte the same at both pins) - so the Danish
  standard's 15-hop ceiling does **not** cover the packets 1.18 newly calls un-scoped: they are
  held to `flood_max`, which is 64. Nothing of this task is wrong at either pin; what the bump
  owes is the decision whether the profile should also lower `flood.max` (upstream's own 64), and
  Thomas's, since it is the Danish standard and not a reading of upstream.
