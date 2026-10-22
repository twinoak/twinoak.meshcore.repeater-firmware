# NoiseScope — the SX1262 spectrum and noise scanner

The second role of this family (`docs/ARCHITECTURE.md` §8): diagnostic firmware
flashed **in place of the repeater** when a site has a noise problem. It streams
spectrum sweeps, RSSI histograms and burst statistics over the Walter
management UART, and then the repeater is flashed back. The measurement happens
through the repeater's own RX chain — antenna, coax, cavity filter, matching
network, the SX1262 itself — so unlike an external tinySA measurement it shows
exactly what the radio sees.

**Never transmits. RX only. Never writes flash.** Those three are what make a
scanner-and-back round trip safe for a provisioned node, and each is a line of
`test/test_rails` rather than a promise on this page.

This page is the hardware repository's `software/firmware/noisescope-rak4630/`
`README.md` and `PROTOCOL.md`, merged and corrected for where the firmware now
lives (`tasks/phase-3/reports/02.md` lists every correction). The sources moved
at commit `b798aab043281fa4c9b17802a164fa94be12e206` of that repository.

| | |
|---|---|
| role | `scanner` (descriptor role byte **3**, `spec/FIRMWARE-MANIFEST.md` §4) |
| image | `scanner.rak4630-single.868`, beside `repeater.rak4630-single.868` in every release |
| build | `pio run -e scanner_rak4630_single` |
| sources | `src/roles/scanner/`, with `src/common/` for the descriptor, the INA3221 and the band table |
| link | NDJSON lines and text commands, **not frames** — `spec/LINK-PROTOCOL.md` §9 |
| protocol version | 0.1, unchanged |

## 1. What it measures

* **Monitor (default on boot):** continuous ~1 kHz instantaneous-RSSI sampling
  of the home channel (869.618 MHz, 62.5 kHz, SF8 by default — the band's own
  channel, `platformio.ini`'s `[band_868]`) through the LoRa channel filter.
  Reports avg/max/min and a burst duty-cycle number every 10 s — this is
  MeshCore's "noise floor" number, but with the transients that MeshCore's
  averaging deliberately filters out.
* **Sweep:** stepped-RSSI spectrum scan (the band's range, 863–870 MHz, in
  25 kHz steps by default), avg + max-hold per point. The SX1262 tunes
  150–960 MHz, so you can also point it at a suspected blocker (GSM/LTE
  downlink) — through the cavity filter, which is the honest view of what
  reaches the radio.
* **Scope (the fast sweep):** the same spectrum as *sweep*, with the stepping
  moved **inside the radio** — a 167-word program for the SX1262's own core
  retunes after every RSSI update and leaves one level per point in the data
  buffer, so 250 points cost tens of milliseconds where a stepped sweep of the
  band costs seconds. Fast enough for a live waterfall, and the span is the
  operator's: a channel, a band, or the whole 150–960 MHz the radio tunes, in
  segments each calibrated for its own range. §6 is what the program is and
  where it comes from.
* **Hist:** Semtech's spectral-scan engine (the official RAM patch, the same
  mechanism SX1261-equipped LoRaWAN gateways use). Per frequency: a histogram
  of 2048 RSSI samples taken 8.2 µs apart, 33 bins × 4 dB from −11 down to
  −139 dBm. Shows floor, burst level and duty cycle in one line — the tool for
  "idles at −115 but jumps to −100" mysteries.
* **Dwell:** park on one frequency and stream windowed statistics — burst
  periodicity and duty cycle over time.
* **CAD:** LoRa preamble-detection ratio, to tell "another LoRa network" apart
  from broadband/other interference.

## 2. Relationship to the repeater

Same board target, same pins, same band, same release. What the two images
share is `src/common/`:

| | |
|---|---|
| the image descriptor | the 96 bytes of `spec/FIRMWARE-MANIFEST.md` §4, and the `role`, `board` and `ver` of the `boot` and `info` lines are read **out of those bytes** |
| the INA3221 | `src/common/power/` — the address, this board's three shunt values (RS2 100 mΩ, RS3 20 mΩ, RS4 50 mΩ), the register arithmetic and the reader's schedule. A `pwr` object of the scanner and a `POWER_GET` of the repeater are now the same numbers from the same code |
| the band | `src/common/bands/Bands.h` — the home channel and the default sweep are the band's, and a build whose frequency is outside its band does not compile |
| the Arduino pin map | MeshCore's `variants/rak4631/`, out of the pinned checkout, for both roles |

What it does **not** share is the mesh: the scanner compiles no MeshCore at
all (`platformio.ini`: `lib_ignore`). It drives the SX1262 through RadioLib
directly, because it retunes between LoRa and FSK, uploads a RAM patch and
reads instantaneous RSSI a thousand times a second — none of which upstream's
radio wrapper exposes. The RX path is the repeater's, setting for setting:

* **DIO2 as the RF switch** and **DIO3 driving the TCXO at 1.8 V** (the
  variant's own values, `variant.h`);
* **140 mA current limit** and **RX boosted gain on** (the repeater
  environment's two flags, passed to the scanner with the same values; `boost
  off` to compare);
* **RadioLib at the same exact version** as the repeater's image
  (`platformio.ini`'s `[libs].radiolib`), configured with the same
  `RADIOLIB_EXCLUDE_*` list (`[radiolib]`);
* **the SX1262's power-enable pin driven high** before anything else, which
  the repeater's board does in `RAK4631Board::begin()` and NoiseScope did not
  — the one change this port made to its bring-up
  (`tasks/phase-3/reports/02.md`).

**The node's settings are not touched.** The identity keys and preferences in
the internal file system are never opened: this image compiles no file system,
so a scanner → repeater round trip keeps the node's ID, its name and its
neighbours. When flashing over SWD, use **sector/range erase of the
application area only — never chip erase** (`docs/ARCHITECTURE.md` §9), or the
bootloader, the SoftDevice and that file system go with it.

## 3. Building and flashing

```
pio run -e scanner_rak4630_single
tools/verify.ps1                      # builds both images, with the tests
```

Artifacts in `.pio/build/scanner_rak4630_single/` (and in
`out/build/scanner_rak4630_single/` after `tools/verify.ps1`), the same three
files every image of a release carries (`docs/release.md`):

* `firmware.bin` — the raw application, for SWD (the Walter, a J-Link, a probe)
* `firmware.zip` — the DFU package for `adafruit-nrfutil dfu serial`
* `firmware.uf2` — for the bootloader's drag-and-drop drive

A release names them `twinoak-scanner-rak4630-single-868-vX.Y.Z.*`.

**Remote (Walter, SWD):** flash over the SWD lines (D-sub pins 3/4/5),
application range only. Same procedure back to the repeater. The nRF52
watchdog (8 s) plus the bootloader living in protected flash means a bad day
ends in a reboot loop you can still flash out of — not a brick.

**Field (USB-C on the adapter):** double-tap reset or send `dfu` over any
serial console → UF2 bootloader (drag and drop) — or
`adafruit-nrfutil dfu serial -pkg firmware.zip -p COMx -b 115200`.

## 4. Using it

Connect at 115200 (the Walter UART, or USB). It boots into monitor mode and
starts reporting on its own; with `auto 10` (the default) it also runs a full
sweep of the band every 10 minutes. A typical session:

```
time 1754820000        # sync the clock (writes the RV-3028)
info
sweep                  # one full-band pass, ~7 s
hist 869.4 869.8 25    # histogram zoom on the 500 mW ERP subband
dwell 869.618 300      # 5 minutes of burst statistics on-channel
cad                    # is it LoRa?
```

A live waterfall on a laptop, and a saved log plotted later:

```
pip install -r tools/noisescope/requirements.txt
python tools/noisescope/waterfall_plot.py --port COM7 --metric max
python tools/noisescope/waterfall_plot.py --file site.ndjson --save site.png
```

And the fast sweep, which is the one to reach for when something is *moving*.
The plotter sends the command itself and `stop` when it leaves, and draws the
spectrum over a waterfall on one frequency axis:

```
python tools/noisescope/waterfall_plot.py --port COM7 --scope 863 870
python tools/noisescope/waterfall_plot.py --port COM7 --scope 150 960 --points 2048
python tools/noisescope/waterfall_plot.py --port COM7 --scope 791 960 \
    --baseline dummyload.ndjson --mark 869.4,869.65 --range -130 -60
```

`--baseline` is the one worth knowing: capture a `scope` of the same span with
a **50 Ω dummy load** in place of the antenna, and the plotter subtracts that
capture's per-frequency median from what it draws. The receiver's own spurs
and the shape of the front end cancel, and what is left is the site (§7:
deltas are the signal).

By hand, over any console:

```
scope 863 870                  # the band, 250 points, a frame per sweep
scope 150 960 2048             # everything the radio tunes, in 26 segments
scope 869.4 869.8 512 19.5 200 mean
stop                           # or `scope off`
```

## 5. The protocol

Line-oriented, both directions, **115200 8N1** on the Walter management UART
(`Serial1`, D-sub pins 8/9). The same protocol is served on USB CDC when a
host is attached.

### Framing

* **Device → Walter:** NDJSON. One JSON object per line, `\n`-terminated. Any
  line **not** starting with `{` is human-readable chatter (help text,
  prefixed `# `) and must be ignored by machine parsers.
* **Walter → device:** plain-text commands, one per line, space-separated
  arguments, `\n` or `\r` terminated. The command word is case-insensitive; an
  argument is not. At most eight words are read.
* **Every line a scan produces** carries `seq` (monotonic per boot — detect
  gaps and reboots), `ts` (unix epoch, `0` = the clock was never set; re-stamp
  on receipt if 0) and `ms` (millis since boot). `boot` carries `ts`, and
  `stat` carries `ts` and `ms`: a heartbeat is not a measurement and has no
  `seq`.
* **Every number is finite.** A reading that is not a number — a window with no
  sample, a radio that answered nothing — is written as `null` and not as
  `nan`, because `nan` is not JSON and a line a parser drops carries nothing at
  all. `test/test_scanner` holds every message to that.
* No checksum in v0.1: the link is short, push-pull and double-filtered
  (2×1000 pF). A malformed line fails JSON parsing — drop it.

### Commands

| Command | Effect |
|---|---|
| `help` | human-readable command list (`# ` lines) |
| `info` | one `info` line (fw/version/role/board/devid/reset reason/peripherals/radio ROM/home cfg) |
| `stat` | one `stat` line immediately (the heartbeat also fires every 10 s) |
| `sweep [f0 f1 [step_khz [dwell_ms [passes]]]]` | RSSI sweep. Defaults: the band's range, 25 kHz, 20 ms, 1 pass. `passes 0` = repeat until `stop` |
| `scope <f0> <f1> [points [rbw_khz [interval_ms [agg]]]]` | the fast sweep, streamed to the port the command came in on until `stop`. `150 ≤ f0 < f1 ≤ 960` MHz, **both ends required**; `points` 16…2048 (`0` = 250, or as many as the given `rbw` covers without gaps); `rbw_khz` `0` = the narrowest bandwidth at least 1.1 × the step, capped at 467; `interval_ms` 0…60000, `0` = a frame per sweep; `agg` `max` (default), `mean` or `last`. `scope off` ends it |
| `hist [f0 f1 [step_khz [samples [passes]]]]` | spectral-scan histogram sweep (Semtech RAM patch). Defaults as above with 2048 samples |
| `dwell <f_mhz> [secs [win_ms]]` | time-domain statistics on one frequency (defaults 60 s, 1000 ms windows). Uses the home LoRa RX filter |
| `cad [f_mhz [count]]` | LoRa channel-activity detections out of `count` tries (default the home frequency, 100) |
| `stop` | abort any activity, return to monitor |
| `home <f_mhz> [bw_khz sf cr]` | set the monitor channel (the band's by default). A frequency alone retunes and leaves the modem settings alone; all three change together |
| `boost on\|off` | SX1262 RX boosted gain (default on, matching the repeater) |
| `auto <minutes>` | periodic unattended sweep with the default parameters (default 10, `0` = off) |
| `time <unix_epoch>` | set the clock; also written to the RV-3028 when present. An epoch before November 2023 is refused |
| `reboot` | software reset |
| `dfu [serial]` | reboot into the UF2 bootloader (or serial-only DFU with `serial`) |

**Both ends of a range are read together or not at all**: `sweep 865` is the
default sweep, not a sweep from 865 MHz. A sweep is refused when the ends are
crossed, either end is outside 150–960 MHz, the step is below 1 kHz or above
1 MHz, or the range comes to fewer than 2 or more than 512 points. `scope`
goes further and **requires** both ends — it has no default span, because the
span is the whole point of it — and is refused for a step below 100 Hz (finer
than the radio tunes) as well.

A `scope` ack carries what was *taken*, which is not always what was asked:

```
{"t":"ack","cmd":"scope","ok":true,
 "msg":"n=250 step=28.0000kHz rbw=39.0kHz seg=1 cov=1.393 interval=0ms sweep=86ms"}
```

`seg` is how many plugin calls one sweep of the span is (§6), `cov` is the
receiver bandwidth over the step — below 1 and a narrow carrier can fall
between two points — and `interval` is what the port allowed (below).

Every command is answered with an `ack` line:
`{"t":"ack","cmd":"sweep","ok":true,"msg":"started"}`. A command arriving
mid-scan is executed between scan steps — nothing is lost, and a long `hist`
step delays it by tens of milliseconds. `stop` always works.

### Messages (device → Walter)

`boot` — once after every reset. `reset` is one of `por|pin|wdt|soft|lockup`.
`role`, `board` and `ver` are read out of the image's own descriptor, so what
the unit says and what the file said before it was flashed cannot differ.

```json
{"t":"boot","fw":"noisescope","ver":"1.2.3","role":"scanner","board":"rak4630-single",
 "hw":"rak4630","radio":"sx1262","reset":"por","radio_ok":true,"rtc":1,"ina":1,
 "ts":1754820000}
```

`info` — the same six identity fields, plus the build date, the nRF52's device
id, which peripherals answered, the boosted-gain and auto-sweep settings, the
radio's own ROM string and the home channel. `rom` is read once at boot and is
what decides whether `scope` can run on this chip at all (§6); it is empty when
the radio did not answer.

```json
{"t":"info","fw":"noisescope","ver":"1.2.3","role":"scanner","board":"rak4630-single",
 "hw":"rak4630","radio":"sx1262","build":"Oct 12 2026","devid":"deadbeef01020304",
 "reset":"por","rtc":1,"ina":1,"radio_ok":1,"boost":1,"auto_min":10,
 "rom":"SX1261 V2D 2D02","home":{"f":869.618,"bw":62.5,"sf":8,"cr":8}}
```

`stat` — the heartbeat, every 10 s and on request. `rssi` is present in
monitor mode only and aggregates ~1 kHz instantaneous-RSSI sampling of the
**home channel through the LoRa channel filter** — what the repeater's
receiver lives in. `dc` is the fraction of samples above `th` (the previous
window's minimum + 10 dB): a direct burst-duty-cycle number. `pwr` is the
B-board INA3221's three channels — panel / battery / load — with this board's
own shunt values applied (`src/common/power/Ina3221.h`): `ipanel` positive =
panel into the charger, `ibat` positive = charging and negative = discharging,
`iload` positive = draw. Voltages are the bus readings at VIN, VBAT and
VLOAD+. The three channels are reported together or not at all.

```json
{"t":"stat","ts":1754820010,"ms":12345,"up":12,"mode":"monitor","home":869.618,
 "rssi":{"n":9900,"avg":-114.2,"max":-99.5,"min":-116.0,"dc":0.0312,"th":-106.0},
 "pwr":{"vpanel":18.10,"ipanel":120,"vbat":4.012,"ibat":-52,"vload":3.980,"iload":85}}
```

The power numbers come from the reader's cache and not from the bus: a `stat`
never waits on I2C, and during a long sweep or histogram pass the values are
the last ones the reader took (it runs between scan steps, like everything
else).

`sweep` — one line per completed pass. `avg` and `max` are dBm arrays of
length `n`; point *i* is at `f0 + i*step/1000` MHz. `rbw` is the FSK receiver
filter bandwidth used (kHz), auto-picked as the smallest one at least
1.1 × the step.

```json
{"t":"sweep","seq":17,"ts":1754820100,"ms":112000,"f0":863.000,"f1":870.000,
 "step":25.0,"rbw":29.3,"dwell":20,"n":281,"unit":"dBm","avg":[-115.2,...],"max":[-108.9,...]}
```

`scope` — one line per frame of the fast sweep, for as long as the mode runs.
`f0` is the **first point's centre** in MHz and `step` its spacing in kHz, so
point *i* is at `f0 + i*step/1000` MHz, as for a `sweep`. `lv` is the base64 of
`n` bytes, one per point, each **−2 × dBm** as the SX126x reports it (0.5 dB
steps; `255` = no reading). Base64 and not an array of numbers: a 2048-point
frame is then 2.7 kB instead of 9, the line is still JSON, and the Walter's
verbatim forwarding and every NDJSON reader go on working untouched.

`sweeps` is how many sweeps went into this frame and `sps` is sweeps per
second **× 10** since the previous one (476 = 47.6 a second). `agg` is what the
frame holds of those sweeps: `max` (the strongest level of each point, so a
short burst shows), `mean` (in the power domain) or `last`. `cov` is the
receiver bandwidth over the step.

```json
{"t":"scope","seq":312,"ts":1754820500,"ms":512000,"f0":863.014000,"step":28.0000,
 "rbw":39.0,"n":250,"cov":1.393,"agg":"max","sweeps":6,"sps":476,"lv":"7Ozs7e3t7u7u…"}
```

**Where a frame goes.** Every other line of this firmware goes to the Walter's
UART *and* to USB. A `scope` frame goes to **the port its command came in on,
and only there** — the other port keeps getting `stat`, `boot` and everything
else as before. The two ports are not the same pipe: USB CDC takes every sweep
there is, while the Walter's UART is 115200 baud, so a `scope` started on the
UART has its interval raised to what that port can carry for that frame size
(a frame may take at most half the port's time). The `ack` says the interval it
took. A scope can therefore never block the UART writer or starve the watchdog,
whatever it was asked for.

`hist` — one line per frequency per pass. `bins[i]` counts RSSI samples in the
4 dB slot starting at `b0 - 4*i` dBm (bin 0 = −11 dBm, bin 32 = −139 dBm); `n`
samples were taken ~8.2 µs apart (≈17 ms per line at n=2048). The histogram
preserves transients that averaging destroys: a 1 % duty-cycle burster shows
up as a distinct population in a high bin.

```json
{"t":"hist","seq":44,"ts":1754820200,"ms":212000,"f":869.618,"rbw":29.3,
 "n":2048,"b0":-11,"bw_db":4,"bins":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,19,0,0,1,210,1790,25,0,0,0,0]}
```

`dwell` — one line per window. The same statistics as `stat.rssi` but at the
commanded frequency and window length.

```json
{"t":"dwell","seq":81,"ts":1754820300,"ms":312000,"f":869.618,"win":1000,
 "n":990,"avg":-113.8,"max":-100.1,"min":-115.9,"dc":0.0404,"th":-105.9}
```

`cad` — the LoRa preamble-detection ratio at the home modem settings. A high
`det` with bursty RSSI means the interferer is another LoRa network and not
broadband noise.

```json
{"t":"cad","seq":90,"ts":1754820400,"ms":412000,"f":869.618,"sf":8,"bw":62.5,"n":100,"det":17}
```

`done` — when a finite activity completes:
`{"t":"done","cmd":"sweep","seq":18,"ts":...,"ms":...}`.
`ack` / `err` — command responses and asynchronous errors (`err` carries
`msg`). An `err` from a `scope` whose segment would not arm, or whose sweep
failed, also carries **`call`** — which of the radio calls returned the code:
`standby`, `setFrequency`, `calibrateImage`, `setSweep`, `sweep` or `reload`
— and **`code`**, RadioLib's status (`-707` is `RADIOLIB_ERR_SPI_CMD_FAILED`,
the chip's *failure to execute*). Both are additive: a reader that knows only
`msg` reads the same line as before. The `msg` ends with the SX1262's own
`GetDeviceErrors` word (`0x0010` is an image calibration that failed,
`0x0040` a PLL that did not lock) and, when the chip was reset and the plugin
uploaded again on the way, *after a reload*. Two things to know when reading
`call`. RadioLib's SX126x driver takes the status byte the chip clocks out
*during* a command and asks for no `GetStatus` after it, so a code a call
returns is the chip's verdict on the command **before** it — `call` is where
the code surfaced. And a calibration the chip refused without the driver
seeing it is caught through `GetDeviceErrors` and reported as
`calibrateImage` with the same `-707`, which is the code the driver would
have returned had it asked (§8, *The scope across the band*).

```json
{"t":"err","msg":"scope setup failed (-707) in calibrateImage at 181.4429; device errors 0x0010; after a reload",
 "call":"calibrateImage","code":-707}
```

### Forwarding it, without understanding it

The format is chosen so that the Walter needs no knowledge of the payload:

1. Read lines; drop anything not starting with `{`.
2. Publish verbatim to `<site>/scanner/<t>` (parse only the `t` field), QoS 0 —
   e.g. `grenaa-chimney/scanner/sweep`. Retain only `boot` and the latest
   `stat`.
3. If `ts` is `0`, either send `time <epoch>` to the scanner (preferred — it
   has an RTC) or add an `rx_ts` field before publishing.
4. Watch `seq` for gaps (lost lines) and for restarts (a reboot restarts it,
   and a fresh `boot` line comes with it).
5. On session start: send `time`, optionally `auto 10`. Data flows without
   further commands.

A `sweep` line at the default settings is about 2.5 kB, so ~0.25 s of UART
time at 115200; at `auto 10` that is about 15 kB/h of LTE traffic including
the heartbeats.

A `scope` frame is 0.5 kB at 250 points and 2.9 kB at 2048, and it arrives as
often as the interval says — which on this UART the device has already raised
to what the port can carry (§5). It is a **stream and not a measurement a unit
leaves running**: start one when somebody is watching, and `stop` it. Forwarding
it needs nothing new, since it is one NDJSON line with a `t` like any other, but
the LTE budget is the Walter's problem and not this firmware's.

### Telling this firmware from the repeater

`spec/LINK-PROTOCOL.md` §9: a Walter that gets no `HELLO` response but sees
lines beginning with `{` — or gets one in answer to `info\n` — is talking to a
scanner, and treats the UART as text until the next reset of the RAK. Text is
never mixed with frames in one session. The image itself says the same thing
before it is flashed: its descriptor's role byte is 3.

## 6. Design notes and safety

* **Watchdog:** an 8 s hardware WDT, fed from the main loop. Any hang → reset →
  boots into passive monitor mode. A scan step feeds nothing: a step that
  outlasts the timeout is a step that should not have started.
* **Serial always live:** scans are chunked, and command input is drained
  during blocking steps and executed between them.
* **Spectral-scan patch:** a ~2 kB Semtech blob (from `Lora-net/sx1302_hal`,
  shipped with RadioLib), uploaded into SX1262 RAM over SPI at the start of
  each `hist` session and purged by a hardware reset at the end. Volatile by
  design: a PWR_KILL cycle, an NRST pulse or a reboot always returns the radio
  to stock.
* **The sweep plugin (`scope`):** a second RAM patch, and not Semtech's. It is
  **167 words of code for the SX1262's own core** (a CoolRISC 816), uploaded
  into the chip's program RAM with the same `SX126x::uploadPatch` the histogram
  uses; it adds the command `0x9B`, which retunes the receiver after every RSSI
  update and leaves one level per point in the data buffer. One SPI command is
  then a whole sweep: 250 points in about 86 ms at the narrowest bandwidth and
  about 5 ms at 78 kHz, where the MCU-stepped `sweep` takes seconds.

  It comes from MeshCore issue
  [#3578](https://github.com/meshcore-dev/MeshCore/issues/3578) and is vendored
  under `src/roles/scanner/vendor/sx126x_spectrum/` — the host class and the
  binary from `sleeply4cat/MeshCore` branch `spectrum-stream` at `41472f97`,
  its assembler source `spectrum.asm` and `README.md` from branch
  `mesh-pocket-spectrum` at `f40f469d`, MIT, each file naming where it came
  from. The assembler is **not** vendored and nothing rebuilds the binary here:
  `test/test_scanner` pins the 167 words by hash instead, so a word cannot be
  changed by hand unnoticed. The `.asm` is checked in beside it to be read.

  **The ROM gate.** The plugin is made for, and was verified by its author on,
  an SX1262 whose ROM version string (register `0x0320`, 16 characters) reads
  `SX1261 V2D 2D02`. This firmware reads that string once at boot and `info`
  carries it as `rom`; a `scope` on any other ROM uploads **nothing**, answers
  an `err` naming the ROM it met, and says that `sweep` is the fallback.

  **It never outlives the mode.** The plugin is uploaded when a `scope` starts
  and the chip is hardware-reset when it ends — `stop`, `scope off`, another
  command, or any error — exactly as the histogram's patch is. Once loaded it
  sets the last character of the ROM string (`0x032E`) to `P`, and that tag is
  checked before *every* sweep: a chip that was reset under us (a brownout) has
  no `0x9B` command, so the mode ends with an `err` rather than running
  whatever is left in the chip's RAM.

  **Why a wide span is several sweeps.** `setFrequency()` calibrates the image
  rejection for one band — one of five Semtech presets, or ±4 MHz around the
  frequency (RadioLib 7.8.0, `SX126x::calibrateImage`) — so a point far from
  the centre is measured through a worse calibration. A `scope` therefore cuts
  its span into **segments**, each at most 32 MHz wide and at most 250 points,
  each calibrated for its own range with `calibrateImageRejection()`, and one
  segment runs per pass of the main loop. `150 960 2048` is 26 of them; the
  `ack` says how many. The 32 MHz is `kScopeSegmentMhz` in
  `src/roles/scanner/Sweep.h`, and the bench lowers it if an image ever shows
  (`docs/bench.md` §9). A segment is armed in one order — standby, its centre
  frequency with the driver's own calibration skipped, the calibration for
  its range, the chip asked whether it took — and a calibration the chip
  refuses is asked for once more after the chip is reset and the plugin
  uploaded again (`armScopeSegment`, host-tested; §8 says what the first
  board taught about it).
* **Power telemetry:** the B-board INA3221 in every heartbeat, so a scanning
  session doubles as a power-budget check. Missing peripherals (a bench setup)
  are detected and skipped.
* **The nRF52840's own radio is untouched:** no BLE, and the SoftDevice is not
  enabled.
* **No DC/DC.** The repeater's board brings the MCU's DC/DC converter up
  (MeshCore's `NRF52BoardDCDC`); the scanner does not, because that helper is
  MeshCore's and this image has none of it. It is the MCU's regulator and not
  the radio's, but it is a difference in the supply the radio sits on — the
  bench is where it is worth a look if two readings disagree
  (`docs/bench.md` §9).

## 7. Interpreting results (quick reference)

* tinySA-style absolute accuracy is not the goal (±a few dB); *deltas* are the
  signal: antenna vs. dummy load, filter in vs. out, quiet hour vs. busy hour.
* `stat.rssi.dc` ≫ 0 with a calm `avg` = a pulsed interferer, the case a single
  noise-floor number hides. Chase it with `dwell` (periodicity), then `hist`
  (level and true duty cycle), then `sweep` with the max-hold to find its
  frequency.
* A high `cad.det` on a clean-looking channel = other LoRa traffic.
* A **`scope` with `--baseline`** is that comparison made automatic: capture
  the same span on a 50 Ω dummy load, and the plotter subtracts its
  per-frequency median from everything it draws. What is left is the site and
  not the receiver.
* A raised floor across the whole sweep that disappears on a dummy load =
  in-band interference or front-end compression from a strong out-of-band
  carrier. Step the sweep across the likely blockers (LTE800 DL 791–821,
  GSM-R/GSM900 DL 921–960) and remember that you are looking through the
  cavity filter — that attenuation is part of the honest answer.

## 8. The fast sweep, as built

*Phase 3.1's task [`01`](../tasks/phase-3.1/01-the-fast-sweep.md); the page for
it is §1, §5 and §6 above, and this is what was decided and where it lives.*

> **The scope across the band** (p3.5-02, 22 October 2026;
> [`tasks/phase-3.5/reports/02.md`](../tasks/phase-3.5/reports/02.md) has the
> probe table). The first RAK4630 (`rom` `SX1261 V2D 2D02`) streamed one- and
> two-segment spans and refused the third segment of anything wider with
> `-707` (p3.4-02). The call was the **image calibration**, and the chip's
> own `GetDeviceErrors` said so (`IMG_CALIB_ERR`): after a plugin sweep that
> ended below about 205 MHz, this SX1262 refuses an image calibration that
> reaches more than about 20 MHz above where the sweep left it — and every
> calibration after that one, until the chip is hardware-reset; a standby in
> either mode, an FS or RX lock at the new frequency, a frequency move across
> the whole range and the PLL's own `Calibrate` were each tried and cleared
> nothing. RadioLib's `SX1262::setFrequency` makes exactly such a calibration
> by itself before any move of 20 MHz or more (7.8.0, `SX1262.cpp:178`), which
> every segment after the first is. Two things, both in `armScopeSegment`
> (`src/roles/scanner/Sweep.cpp`, host-tested): the frequency is set with
> that calibration skipped and the one calibration made is the segment's own;
> and a calibration the chip still refuses — on this board once per pass over
> 150–960 MHz, at the second segment — is answered by a reset, the plugin
> uploaded again and the same arm once more. With that, every span tried
> streams: the whole range at 2048 points at about two sweeps a second, the
> six segments of 791–960 at about ten, 863–870 unchanged at about forty. The
> "silent" spans of p3.4-02's table were the console's: the `err` had been
> written before a second open of the port, and `bench console --scanner
> "<line>" --collect N` now keeps one open across both. The thresholds above
> are this one board's, measured, not the datasheet's.

MeshCore issue [#3578](https://github.com/meshcore-dev/MeshCore/issues/3578),
*Radio spectrum visualization* (sleeply4cat, 12 October 2026), is where the
trick comes from: run the sweep **inside the SX1262** rather than stepping it
from the MCU. What this firmware does with it differs from the issue in four
ways, each of them a line of `src/roles/scanner/Sweep.h` with a native test:

| | the issue | here |
|---|---|---|
| span | one sweep, at most 32 MHz | **150–960 MHz**, cut into segments of at most 32 MHz and 250 points, each calibrated for its own range |
| where frames go | a Bluetooth companion client | the port the command came in on, sized for it: USB CDC gets every sweep, the Walter's 115200 UART gets the interval that port can carry |
| the frame | a binary push of a companion protocol | one NDJSON line like every other, with the levels base64-encoded so that the Walter's verbatim forwarding and every reader go on working |
| the picture | a screen on the device | `tools/noisescope/waterfall_plot.py --scope`, and later the Walter's MQTT |

Where it lives:

| | |
|---|---|
| the plan — segments, bandwidth, coverage, the sink's interval, the duration bounds | `src/roles/scanner/Sweep.h`, host-portable, `test/test_scanner` |
| the frame and its base64 | `src/roles/scanner/Lines.{h,cpp}` |
| the command | `src/roles/scanner/Commands.{h,cpp}` |
| the radio half — calibrate, set the sweep, run it, read the buffer | `src/roles/scanner/Radio.cpp`, which is thin |
| the plugin and its host class, vendored | `src/roles/scanner/vendor/sx126x_spectrum/` |
| the picture | `tools/noisescope/waterfall_plot.py` |

**Not done here, on purpose:** the Walter's forwarding of `scope` frames to
MQTT and the LTE budget that goes with it (the Walter firmware is its own
repository), a web or MeshMaster view of the waterfall (later, once frames
reach MQTT), and the issue's BLE companion protocol — this firmware has no
Bluetooth at all.
