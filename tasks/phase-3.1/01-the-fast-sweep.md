# 01 — The fast sweep

**Depends on:** phase 3 (task 02: NoiseScope is the `scanner` role).
**Spec:** [`docs/noisescope.md`](../../docs/noisescope.md) (all of it; §8 is this task's reference); `src/roles/scanner/` (all of it - `Sweep.*` holds the arithmetic, `Radio.cpp` the radio, `Lines.*` the output, `main.cpp` the two sinks); [`tools/noisescope/waterfall_plot.py`](../../tools/noisescope/waterfall_plot.py); MeshCore issue [#3578](https://github.com/meshcore-dev/MeshCore/issues/3578) *Radio spectrum visualization* (sleeply4cat, 12 October 2026) and the two branches of its fork it links: `sleeply4cat/MeshCore` **`spectrum-stream` at `41472f9734cecdfa5c795eddedd6b15e2553b6b3`** (`src/helpers/radiolib/SX126xSpectrumScanner.h`, `SX126xSpectrumPlugin.h`, `examples/companion_radio/SpectrumSession.{h,cpp}`) and **`mesh-pocket-spectrum` at `f40f469d1e00eb8ae1b8ca07390dfc96a33ab707`** (`src/helpers/radiolib/sx126x_plugins/spectrum.asm` and its `README.md`, `tools/sxplug/`); RadioLib 7.8.0 as this repository builds it (`SX126x::uploadPatch`, `calibrateImage`, `calibrateImageRejection`, `getMod`).
**Size:** one session, long.
**Model:** claude-opus-5

## Goal

Thomas, 13 October: *"This illustrates more or less what I'd like
NoiseScope to be able to do. The illustration shows a rather short span
around 868 MHz which is nice, but I'd like it to go as wide as the radio
allows it, but having the span configurable. The waterfall below is
awesome."*

Today a `sweep` is stepped by the MCU: retune, sample `getRSSI` for the
dwell, next point - 281 points of 863–870 MHz at the default 20 ms take
seconds, so a "waterfall" is one row every few seconds. Issue #3578 does the
sweep **inside the radio**: a 167-word program for the SX126x's own core,
uploaded into its program RAM like the Semtech spectral-scan patch `hist`
already uses, adds a command that retunes after every RSSI update and leaves
one level per point in the data buffer. Its author measured 250 points in
about 56 ms at an RBW of 7.3 kHz, 21 ms at 19.5 kHz and 5 ms at 78 kHz - a
hundred times faster, fast enough for a live waterfall.

Bring that into the scanner as a new streaming mode, **`scope`**, with the
span configurable **up to everything the SX1262 tunes (150–960 MHz)**, and
give the plotter a live spectrum-over-waterfall view. The existing `sweep`,
`hist`, `dwell` and `cad` stay exactly as they are.

## What the review established (13 October), so you need not

- **The plugin binary is its source.** `spectrum.asm` at `f40f469d`,
  assembled with the sxplug assembler vendored on that branch
  (`tools/sxplug`, Python, no third-party packages), gives exactly the 167
  words of `SX126X_SPECTRUM_PLUGIN` in `SX126xSpectrumPlugin.h` at
  `41472f97`, and its linter reports nothing. Both branches are the
  MeshCore fork, MIT (`license.txt`); the scanner class differs between the
  branches and `spectrum-stream` (12 October) is the newer.
- **The plugin is ROM-specific**: it is made for, and verified by its
  author on, an SX1262 whose ROM string (register `0x0320`, 16 chars) reads
  `SX1261 V2D 2D02`; the scanner class refuses any other. Once loaded it
  sets the string's last character (`0x032E`) to `0x50` ('P'), and a reset
  of the chip removes it.
- **The fork caps a sweep at 32 MHz** and the whole span inside 150–960
  MHz. The cap is the image calibration: `setFrequency()` at the span's
  centre calibrates the image rejection for that frequency
  (`SX126x::calibrateImage`: one of five predefined bands, otherwise ±4 MHz
  around it), so points far from the centre are measured with a worse
  calibration. A wider span is therefore **several segments**, each
  calibrated for its own range (`calibrateImageRejection(f_lo, f_hi)`).
- **RBW bounds the useful step.** The receiver bandwidths are the GFSK ones,
  4.8 to 467 kHz. A step wider than the RBW leaves gaps a narrow carrier can
  fall in: 150–960 MHz without gaps needs at least 1735 points at 467 kHz.
- **Two sinks, one of them slow.** Every line the scanner writes today goes
  to the Walter UART **and** to USB (`main.cpp`'s `LineSink`). 115200 baud is
  about 11.5 kB/s; a 250-point frame is a few hundred bytes. USB CDC is
  orders of magnitude faster, and is where a laptop watches a waterfall.

## Deliverables

- **The plugin, vendored** under `src/roles/scanner/vendor/sx126x_spectrum/`:
  the scanner class and the plugin header from `41472f97`, `spectrum.asm`
  and its `README.md` from `f40f469d`, each with a header comment naming the
  repository, the branch, the commit and the licence. Change the vendored
  class only where RadioLib 7.8.0 differs from the commit the fork builds
  against, and say where in the report. Not the sxplug assembler: the
  binary is checked in, and a native test pins it (a hash of the 167 words,
  with the commit that produced it in the test's comment).
- **The ROM gate**: `info` gains `rom` (the 16-character string, read once
  at boot in FSK standby). `scope` on any other ROM answers `err` naming it
  and uploads nothing; `sweep` is the fallback and says so in the `err`.
- **`scope <f0> <f1> [points [rbw_khz [interval_ms [agg]]]]`** and `scope
  off` (and `stop`, which ends anything): `150 ≤ f0 < f1 ≤ 960` MHz;
  `points` 16…2048 (default: enough for no gaps at the RBW, at most 2048);
  `rbw_khz` 0 = the narrowest GFSK bandwidth ≥ 1.1 × step (the rule
  `pickRbwKhz` already applies), capped at 467; `interval_ms` 0…60000
  between frames, 0 = a frame per sweep; `agg` `max` (default: the strongest
  level, so short bursts show), `mean` (in the power domain) or `last`. The
  ack carries what was taken: points, step, RBW, segments, coverage.
- **The plan, host-portable**, in `Sweep.*` beside today's arithmetic and
  under `test/test_scanner`: the span cut into segments of at most
  `kScopeSegmentMhz` (start at the fork's 32, a constant the bench may
  lower) and at most 250 points per plugin call, each segment with its own
  calibration range; `cov` = RBW / step, below 1 when the span has gaps;
  the interval floor on a sink (below). The radio half in `Radio.cpp` stays
  thin: calibrate, set the sweep, run it, read the buffer.
- **Frames**: one NDJSON line each,
  `{"t":"scope","seq":…,"ts":…,"ms":…,"f0":…,"step":…,"rbw":…,"n":…,"cov":…,"agg":"max","sweeps":k,"sps":…,"lv":"<base64>"}`
  - `f0` the first point's centre in MHz, `step` kHz, `sweeps` how many
  sweeps went into this frame, `sps` sweeps per second ×10 since the last
  frame, and `lv` the base64 of `n` bytes, one per point, **−2 × dBm** as
  the SX126x reports it (0.5 dB steps), `255` = no reading. Base64 keeps the
  line JSON, so the Walter's verbatim forwarding (§5) and every NDJSON
  reader still work, at a third of the size a number array would be. A
  2048-point frame must fit `Lines`' buffer; grow it if it does not, and
  test it.
- **The sink rule**: a `scope` stream goes to the port its command came in
  on, and only there - the other port keeps getting `stat` and everything
  else as today. On the Walter UART the interval is raised to what 115200
  carries for that frame size (the plan computes it; the ack says the
  interval it took), so a scope can never block the UART writer or starve
  the watchdog. USB gets every sweep if asked.
- **Safety, as `hist` does it**: the plugin is uploaded when a `scope`
  starts and the chip is reset when it ends (`stop`, `scope off`, another
  command, an error), so it never outlives the mode; before every sweep the
  tag at `0x032E` is checked, and a missing tag (a brownout reset the chip)
  ends the scope with an `err` instead of sending a command the ROM does
  not have. A sweep holds BUSY for its duration; the worst case (250 points
  at 4.8 kHz) is well inside RadioLib's SPI timeout and the 8 s watchdog -
  compute it in the plan and test the bound. RX boosted gain follows the
  scanner's `boost` setting, not the fork's hard-coded `true`. Never
  transmits.
- **The plotter**: `waterfall_plot.py --port COMx --scope 863 870 [--points
  … --rbw … --agg …]` sends the command (and `stop` on exit), and draws the
  **spectrum on top** - the current frame, a max hold and a running average
  - and the **waterfall below** on the same frequency axis, newest row at
  the top, a time axis, the colour scale fixed with `--range -130 -60` or
  automatic (5th to 99.5th percentile of the last rows), at its own frame
  rate whatever the arrival rate. Markers: the home channel from `info`,
  `--mark 869.4,869.65` for band edges, and a grey band where `cov < 1`.
  `--file` replays a log and `--save` writes a PNG, as today.
  `--baseline dummyload.ndjson` subtracts the per-frequency median of a
  dummy-load capture, so the receiver's own spurs and the front end's shape
  cancel and what is left is the site (§7: deltas are the signal). Existing
  `sweep`/`hist`/`dwell` plotting is untouched.
- **Docs and bench**: `docs/noisescope.md` §1, §4, §5 (the command, the
  message, the sink rule) and §6 (what the plugin is, where it is from, the
  ROM gate, the reset), §8 updated from "planned" to what was built;
  `docs/bench.md` gets the bench walk below.

## Acceptance criteria

- [ ] `scope` is planned, validated and framed by host-portable code with native tests: segments and calibration ranges for 863–870, 860–880 and 150–960 MHz; `cov` below 1 and reported for a 150–960 at 1024 points; the UART interval floor; base64 of a 2048-point frame inside the line buffer; `max`/`mean`/`last` with `255` for no reading; the plugin's 167 words pinned by hash.
- [ ] On a ROM other than `SX1261 V2D 2D02` nothing is uploaded and the `err` names the ROM; `info` carries `rom`.
- [ ] A `scope` from USB never writes a frame to the Walter UART, and one from the UART never exceeds what 115200 carries.
- [ ] `sweep`, `hist`, `dwell`, `cad`, `stat`, `info` and `boot` lines are unchanged except `info`'s new `rom` (the existing tests pass with only that).
- [ ] `tools/verify.ps1` green, both images build, `pio test -e native` green, CI green.

## Not verified here

The board. The session has no radio; the bench walk is Thomas's, in
`docs/bench.md`:

1. `info` on a RAK4630: the `rom` string. If it is not `SX1261 V2D 2D02`,
   stop there - the plugin is not for this chip.
2. `scope 863 870` over USB with the plotter: frames arrive at tens of
   sweeps a second, the home channel's traffic shows as bursts in the
   waterfall, and `sps` agrees with the fork's numbers (~21 ms for 250
   points at 19.5 kHz).
3. A **50 Ω dummy load** in place of the antenna, `scope 150 960 2048`,
   saved as the baseline: the radio's own spurs and the matching network's
   shape across the whole tuning range.
4. **The image check**: the tinySA Ultra as a generator at a known
   frequency, through an attenuator into the RAK, and `scope` a wide span
   around it - one peak where it is, and nothing (or what the baseline
   already has) at the image. If a ghost shows, lower `kScopeSegmentMhz`
   until it does not, and write the number down.
5. The site through the cavity filter, `scope 791 960` with the baseline
   subtracted: LTE800 and GSM900 as they reach the radio.
6. The same `scope` from the Walter UART: the ack's interval, and no
   watchdog reset over ten minutes.

## Out of scope

The Walter's forwarding of `scope` frames to MQTT and anything on an LTE
budget (the Walter firmware is its own repository); a web or MeshMaster
view of the waterfall (later, once frames reach MQTT); the issue's BLE
companion protocol (this firmware has no BLE); any change to the repeater
role.
