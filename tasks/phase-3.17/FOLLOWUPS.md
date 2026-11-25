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
