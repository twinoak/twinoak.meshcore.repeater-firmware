# Phase 3.17 — follow-ups

Items for a later task of this phase are tagged with it. Only the reviewer ticks an item.

## Sessions 10 and 11 (25 November)

- [ ] **[after 02's session] The wide picture.** If `stepped` is clean on the unit, the
  wide span is swept that way and stays at 467 kHz; if not, the bandwidth is the question
  again (at 117 kHz and below all but 912.0 is gone, `tasks/phase-3.16/FOLLOWUPS.md`).
  meshmaster.dk's proposal follows either.
- [x] **[answered by session 10, 25 November] The far echoes are the sweep plugin's, not the
  chip's ordinary reception.** In one session: the LoRa receiver parked on 912.0 MHz read
  -118 dBm in all 320 windows of 250 ms and the same on 908.0, while 869.618 held ten
  packets at -32 dBm in 37 of 319 windows; the host-stepped `sweep` at 467 kHz, tuned with
  RadioLib's own `setFrequency`, never read a point 15 dB over its median in 38 passes over
  905 to 919 MHz and 52 over 811 to 821; and the plugin's `scope` of the same place showed
  911.1 to 915.3 at -61.5 dBm in its five lit frames, as in every session.
- [x] **[confirmed by session 11; task 02] An echo is gone when a segment begins at its
  place and stands the settle.** Step 0.5 MHz, first point at x.25, settle 500 µs, five
  lines, 2 to 11 lit frames each. All four mid-segment (`scope 800 960 320 467 1000 max 32
  500`): 815.75 to 818.75 at -64.5 dBm, 845.25 to 848.25 at -77.5, 887.25 to 889.25 at
  -81.5, 911.25 to 916.25 at -61. Starts every 16 MHz from 784.25: 816 reads -104 and 912
  -106 (the floor), 845 and 888 as before. Starts from 792.25: 816 and 912 back; 888.25 and
  on clean, but 887.25 and 887.75, the two points before the start, still lit (-87). Starts
  from 829.25: 845 at the floor (-104.5). A start every 8 MHz: 816 and 912 gone, 845 there
  at -66. Sweeps a second: 11.0 with five segments, 9.9 with eight, 7.9 with ten, 6.9 with
  eleven, 4.4 with twenty-two. `dev` `0000` throughout.
- [ ] **[none] So the plugin reads through a moment the synthesizer needs,** about 30 to
  40 µs, when the frequency crosses certain places: 816.0, 864.0 and 912.0 MHz (25.5, 27
  and 28.5 times 32 MHz; smaller at 832, 880 and 904), and two that fit no rule, about
  845.1 and 887.6. The block at a segment's first point is the same thing after the jump.
  `spectrum.asm`'s `retune` writes the frequency registers and nothing else of what
  `SetRfFrequency` does. Why those places is not known.
- [ ] **[later] What the far echoes are.** 816.0 and 912.0 MHz are 25.5 and 28.5 times the
  32 MHz crystal; the crystal's own 32.0 MHz either side of the signal shows at -97 to -105.
- [ ] **[later] `us` steps by 977 µs** (the board's `micros()`): nothing at 467 kHz.
- [ ] **[later] A sweep takes up to twice as long when readings stand still** (89 to 174 ms
  for the same 250 points at 4.8 kHz): the latency is counted in updates the plugin saw.

## The review of 01 and 02, and the unit's word: session 12 (25 November, evening)

Read: both reports' deviations, decisions and *Not verified here*; the diffs under
`src/roles/scanner/` of both (`Commands`, `Lines`, `Sweep`, `Radio`, `main`); `garden-d.txt`.
Not read: the tests, `echoes.py`, the words. Nothing that holds a release back. v0.1.9
(04290ff) carries both; its scanner ran session 12 on the unit (Walter 0.1.12, restored, six
steps of 85 s to the clock; the scanner flash's first try failed at `0x036000` after 32 s, as
it has since Walter 0.1.11).

- [x] **[proven on the unit] 01: a `scope` that names no settle stands 500 µs.**
  `scope 800 960 320 467 1000` (`ack`: `settle=500us`; 4 lit frames of 85) against the same
  with `max 32 0` (`settle=0us`; 11 lit). With no settle the first points of the segments
  at 832.25, 896.25 and 928.25 MHz read -77.5, -85 and -71 dBm in the lit frames, falling
  over four points; with the default they read the floor (-103.5 to -107.5). 11.0 against
  11.4 sweeps a second for five segments.
- [x] **[proven on the unit] 02: a stepped scope has no far echoes.** The same span with
  `stepped`, twice (`ack`: `settle=500us mode=stepped`; 3 and 5 lit frames, the signal at
  -34.5 and -35.5 dBm): 815.75 to 818.75, 845.25 to 848.25, 887.25 to 889.25 and 911.25 to
  916.25 MHz read -101.5 to -106 dBm in the lit frames, which is the floor. The plugin's two
  steps of the same session read -62.5, -65.5, -74.5 and -62.5 there (default settle) and
  -68, -86.5, -80 and -61.5 (no settle). `dev` `0000` throughout.
- [x] **[measured] The raised floor around a strong signal shrinks too, at 467 kHz.** The
  plugin: -50 to -95 dBm from 858 to 880 MHz in the lit frames. Stepped: the floor (-104)
  up to 864.5 MHz and from 873.75 MHz on, raised only from about 5 MHz under the signal to
  3.6 MHz over it. At 58.6 kHz (13 MHz, one segment; 11 lit frames the plugin's, 2 stepped)
  the plateau at about -65 dBm, 35 dB under the signal and out to 4.4 MHz either side, is
  there both ways: that part is the receiver's own.
- [ ] **[none] What stepped costs.** 320 points at 467 kHz: 223.6 ms a sweep, which is
  0.70 ms a point (the 500 µs stand, and about 0.2 ms for the chip's command and the read):
  3.2 sweeps a second where the plugin makes 11.0. 222 points at 58.6 kHz: 154.3 ms, 6.1 a
  second against 54.6. A one-second `max` frame is three sweeps and not eleven.
- [ ] **[later] A stepped scope still loads the plugin** (02's deviation 5): it uses the
  receiver the arm sets up and nothing of the plugin's sweep, and it is refused on a chip
  whose ROM the plugin does not know.
- [ ] **[none] Point 0 of a stepped segment stands twice** (02's deviation 1): one settle
  a segment.

## Session 13 (25 November, evening): what the stand is worth

The first session on Walter 0.1.14; restored, five steps to the clock. A second station is
on the channel at about -67 dBm (frames at -66 to -72 in every step), so a *lit* frame is
counted here as one holding -50 dBm or more on 869.618 MHz, which is the neighbour alone.

- [x] **[measured] A stepped scope needs its stand.** `scope 800 960 0 467 900 max 32 0
  stepped`, twice (`ack`: `settle=0us mode=stepped`, so the stand is the latency's 21 µs;
  6 and 5 lit frames, the neighbour at -34.5 dBm): 816 and 912 MHz read the floor, **but
  845 MHz is back in some frames (to -65.5 and to -70.5 dBm) and 888 MHz to -86, and the
  raised floor around the signal is as wide as the plugin's** (854 to 877 and to 880 MHz).
  The plugin's control step of the same session: 816 at -59, 912 at -64, 888 at -76.5,
  raised from 856.7 to 889.3 MHz.
- [x] **[measured] With the default 500 µs it is clean, as the proposal types it.**
  `scope 779.618 959.618 0 467 1000 stepped` (386 points, six segments, 5 lit frames at
  -38 dBm): all four places at the floor (-103 to -106), raised only from 865.7 to 873.1
  MHz. And at 234 kHz, `scope 780 960 0 234 2000 stepped` (769 points, 4 lit frames): the
  same, raised from 865.1 to 874.4 MHz, the floor 3 dB lower. `dev` `0000` throughout.
- [ ] **[none] What each costs.** No stand: 76.2 ms a sweep of 343 points (0.22 ms a
  point), 6.0 sweeps a second with six segments, where the plugin makes 9.3. The default:
  270.5 ms for 386 points and 538.1 ms for 769 (0.70 ms a point), 2.6 and 1.5 sweeps a
  second.
- [ ] **[later] A stand between 21 and 500 µs has not been tried.** A session's forty
  characters do not hold `max 32 100 stepped` behind a span and an interval of four digits;
  with the interval left out a step fills the Walter's room in a quarter of a minute.
- [ ] **[later] `echoes.py --lit`** takes a second station on the channel for the signal
  when the threshold is under its level: say so where the threshold is described.

## Session 14 (25 November, 20:21 CET): the two narrow pictures, both ways

Asked for by Thomas, to see them side by side. Restored; four steps of 110 s to the clock;
read on 26 November. The proposal's two narrow lines, each the plugin's way and `stepped`.

- [x] **[measured] 13 MHz at 58.6 kHz (`scope 863.118 876.118 250 0 1000`): stepped is a
  little cleaner and shows the signal as well.** Nine lit frames each, the neighbour at
  -34 and -35 dBm. The plugin: raised from 865.02 to 875.05 MHz without a break, and a lobe
  from 863.92 to 864.34 MHz at -59 dBm (864.0 is 27 times 32 MHz). Stepped: raised from
  866.06 to 874.22 MHz, weak patches beside it (-75 to -93), **no lobe at 864**; the quiet
  floor -115 against -112.5. 48.9 sweeps a second against 5.3 (174.8 ms a sweep).
- [x] **[measured] 2 MHz at 9.7 kHz (`scope 868.618 870.618 250 0 1000`): stepped does not
  show a LoRa signal properly.** The plugin, 9 frames with the neighbour: its channel filled
  at -33 dBm (the median of the seven points around 869.618), and the rest of the span
  lifted to -70 to -73 from a quiet floor of -121. Stepped, 3 such frames of 110: the
  strongest point -32 to -49 but the channel's median -68 to -74, the rest of the span at
  -87 to -101. A chirp of 62.5 kHz is inside a 9.7 kHz filter a sixth of the time, and a
  point is read four times a second where the plugin reads it sixteen times: 15.8 sweeps a
  second against 4.3 (214.8 ms a sweep, the stand there is the latency's 660 µs).
- [ ] **[later] The lift of the whole 2 MHz span while the neighbour sends** (-70 dBm, 50
  dB over the quiet floor) is 15 to 30 dB lower stepped, in three frames. Whether that is
  the stand or only fewer readings while the packet is on is not told apart.
- [ ] **[next: meshmaster.dk] So, for the proposal:** the 180 MHz step `stepped`; the 13
  MHz step either way (stepped loses the lobe and 43 of 49 sweeps a second); the 2 MHz
  step the plugin's. Thomas's to say.

## Session 15 (26 November): the proposal with the new settings

Asked for by Thomas as a real try: the 2 MHz picture the plugin's way, the 13 MHz and the
180 MHz pictures `stepped`, then the dwell and the preamble check. The scanner flash's
first try went through (Walter 0.1.14, three sessions of three).

- [x] **[measured] The wide picture is clean.** `scope 779.618 959.618 0 467 1000 stepped`,
  60 frames, 10 holding the neighbour: 816, 845, 888 and 912 MHz at the floor (-103.5 to
  -106 dBm). What stands in the quiet median: 865.2 to 867.5 MHz (-91 to -100) and 943.1
  MHz (-100), over a floor of -108.5. Thomas: *very happy with step 3*.
- [ ] **[phase 3.18] The neighbour's packets fill the two narrow pictures.** In the frames
  holding the neighbour, 250 of 250 points of the 2 MHz picture and 195 of 250 of the 13 MHz
  picture stand 10 dB or more over their quiet medians (17 of 386 in the wide one). The
  second station, at -65 dBm, raises 10 of 250 points of the 13 MHz picture and the floor
  stands beside it. Read as the chip's gain control; `tasks/phase-3.18/` asks the unit.
- [ ] **[none] The dwell and the preamble check:** 60 windows of a second on 869.618 MHz,
  13 with something over -90 dBm and 8 with the neighbour; 75 of 100 tries heard a LoRa
  preamble.
