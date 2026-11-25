# Phase 3.16 — follow-ups

Items for a later task of this phase are tagged with it. Only the reviewer ticks an item.

- [ ] **[next, offered: 500 µs, see *What the garden said*] The default.** Which width and settle `scope` takes when none is
  given, and meshmaster.dk's proposal for the wide span.
- [ ] **[later] Two layouts and the lower reading.** A sweep made twice with its segments
  begun in different places, each point keeping the lower of the two: an echo that follows
  the segments falls on different frequencies in the two and is dropped, whatever makes it.
  It costs half the sweeps, and a real signal shorter than two sweeps is dropped with it.
- [ ] **[later] The latency is counted in updates the plugin saw.** `wait_update` takes a
  reading that did not change after about three update times, so on a steady level a point
  costs three updates and the reading filed under it may belong to a later one. Not measured.
- [ ] **[Thomas] The image check of `docs/bench.md` §9 has never been walked** (22 October):
  one carrier, a wide `scope`, one peak. This pile's bench is that check and more.
- [ ] **[later] A sweep from high to low** would say timing from frequency at a glance. The
  plugin's step has no sign; it would be a change to words that are not ours.

## The review of 01 and 02 (24 November)

Both merged 24 November (#61, #62, main 9b0de3b). Read: both reports whole but for the tests'
names. Not read: the code. Eight of eight and seven of eight criteria; the open one is the
bench's.

- [ ] **[Thomas] No board was on the bench for either task.** `bench status` said *board NOT
  PRESENT* and `bench.local.json` still read `dummy-load`, so nothing of *Before the run* had
  been made, and the capture of 23 November was not in the checkout. Every table is empty.
  With the board, the generator and `"rf": "antenna"` in place it is two commands:
  `python tools/noisescope/hunt.py --label p3.16-01 --carrier 869.618` and
  `python tools/noisescope/hunt.py --list tools/noisescope/lists/p3.16-02.txt --label p3.16-02 --carrier 869.618`.
  They need this pile's scanner on the board (`bench flash` of its build), since the second
  list types the two numbers.
- [x] **[none] 01 built the two tools and read the plugin.** `echoes.py`, `hunt.py`,
  `scope_plans.json` (the plans both the C++ and the Python tests read), 46 tests. Inside one
  `scope` the three units are one number, so the tool says *cannot tell* for one scope and
  names the unit only from scopes of different step and bandwidth; the floor is each
  segment's tenth percentile. On the dummy-load baseline of 22 October it finds nine spurs
  whose distances do not repeat.
- [ ] **[the bench] What the plugin's source says** (its report, *The plugin, read*): a point
  costs the plugin about forty instructions, about 3.4 µs, which at 467 kHz is a whole RSSI
  update, so the sweep there cannot retune *right after every update*; `wait_update` takes a
  reading that has not changed after 17 reads and counts it as an update, so the latency and
  the pacing depend on the signal; a retune is four register writes and nothing waits for a
  lock; the receiver stands at a segment's centre and point 0 is half a segment away with two
  updates for the jump. **Nothing in the program has a period**, of points, MHz or time: the
  comb every 5.6 MHz is the chip's, if it is there at all.
- [ ] **[next, mine] The SX1261/2 datasheet was read by neither session** (no copy, no web).
  Every sentence about the synthesizer is a guess until somebody reads it.
- [x] **[none] 02 built the two numbers**, `seg_mhz` and `settle_us` after `agg`, and the
  default has not moved (a `scope` of six numbers or fewer is `main`'s, held by tests). The
  settle is the frequency written to the first point with the receiver already in receive,
  then a wait; a one-point sweep could not be used, because `setSweep` itself puts the
  receiver back at the centre.
- [x] **[answered by session 9: it does] Not known: whether the chip takes a frequency while it is in continuous
  receive.** The plugin does the same write all sweep long. If it does not, the settle
  changes nothing and the table shows that.
- [ ] **[later] Nothing bounds segments times settle:** a legal line can plan a sweep of 41 s,
  which its `ack` says. And a calibration's own milliseconds are not in the planned time.
- [x] **[none] A plan no narrower than 32 MHz calibrates every segment as `main` does;** only a
  narrower one skips a calibration inside a range already made.
- [ ] **[Thomas] `VERSION` is still 0.1.7 with three notes waiting** (p3.14-01, p3.15-01,
  p3.16-02): the notes pull request of v0.1.7 is not merged, and a release refuses until it
  is.

## What the datasheet says, as far as it could be read (24 November)

The SX1261/2 data sheet, Rev 1.1, 1.2 and 2.2, read through a web fetch that returns the first
forty-odd pages of each and nothing after (sections 1 to 6.1). Sections 8, 9, 13 and 15 are
not read. Page numbers are Rev 2.2's.

- [ ] **[the bench] A hop of 10 MHz takes the synthesizer 30 µs** (`TS_HOP`, table 3-7, p.19;
  40 µs from standby to a running synthesizer). A segment's first point is up to 16 MHz from
  where the receiver stands, and the plugin gives it two RSSI updates: 6.9 µs at 467 kHz,
  55 µs at 58.6 kHz. So at the wide filters the first readings of a segment are taken while
  the synthesizer is still on its way, by the maker's own number. A settle of 50 to 100 µs is
  the first thing for the bench to try; 02's list starts at 100.
- [x] **[answered by session 7: the LDO changes nothing] In receive the chip's DC-DC converter is clocked from the synthesizer, at
  about 5 MHz, and the divider is worked out again only when `SetRfFrequency` is called**
  (section 5.1, p.34). The plugin retunes by writing the frequency registers, so never. A
  switching regulator at about 5 MHz is a second thing that could make a comb, and it would
  sit at fixed distances from the strong signal, not from a segment's first point. The two
  captures of 23 November fit the segment's first point exactly and the signal poorly; the
  span begun 7 MHz lower says which.
- [ ] **[later] The receiver has an automatic gain control that can be switched off for a
  manual gain** (section 6.1.6, p.41: how the RSSI is calibrated). The registers are in a
  table that was not read. A gain held still for a sweep would take the gain control out of
  the question.
- [ ] **[none] The synthesizer** is a fractional-N third-order sigma-delta loop on the 32 MHz
  reference, *capable of auto-calibration* (section 4.2, p.25). Nothing in the pages read
  says how its oscillator is banded or when it calibrates, so the band changing every 5.6 MHz
  is still a guess.
- [ ] **[none] The receiver is a low-IF one** (167, 200 or 250 kHz by bandwidth, table 4-2);
  image rejection 54 dB calibrated and 35 dB not; blocking 68, 70 and 80 dB at 1, 2 and
  10 MHz for a 20 kHz filter (table 3-8). A neighbour at -32 dBm leaves an image some 54 dB
  down within half a MHz of itself, inside its own skirt.
- [ ] **[next, mine, with the file] Rev 2.2 added a table *RSSI settling time per bandwidth***
  to `GetRssiInst` (13.5.4, p.102) and pseudo-code for the image calibration (9.2.1). Neither
  was readable. With the PDF itself: sections 8, 9.2, 9.6, 13.4.1, 13.5.4 and 15.

## The data sheet itself (Rev 1.2, the file Thomas gave; 24 November)

Sections 8, 9, 13 and 15 read from the PDF. Page numbers are Rev 1.2's.

- [x] **[answered by session 7: not the DC-DC] The radio runs on its DC-DC converter here, and that converter
  switches at about 5 MHz off the synthesizer** (5.1, p.32). `SX126X_USE_REGULATOR_LDO` is
  defined nowhere in this repository, so `beginPlugin` asks for the DC-DC. Looked at again
  with that in mind, the echoes of 23 November lie near multiples of 5 MHz from the
  neighbour's signal in both captures: in the 13 MHz one at +4.8 to +5.4 MHz (874.4 to
  875.0) with the level at -5.4 MHz well over its mirror; in the 180 MHz one near -65, -60,
  -55, -50, -30 to -25, +20, +30, +45, +60, +65 and +85 MHz, each within a MHz or two. That
  reading explains three places the segment's-first-point reading did not (805.5, 820.0,
  955.7 MHz) and misses the exactness of the other. **The captures cannot say which.** Two
  measurements can: the span begun 7 MHz lower (01's list has it), and the same sweep with
  the radio on its LDO. The second needs a word the scanner has not got: the regulator,
  switched as `boost` is.
- [x] **[done in 03; `0000` in sessions 7 to 9] The chip says when its synthesizer lost lock:** `GetDeviceErrors`
  has `PLL_LOCK_ERR` (bit 6) and `PLL_CALIB_ERR` (bit 2) beside the `IMG_CALIB_ERR` this
  firmware already reads (13.6.1, table 13-85). Read after a sweep, they would say whether
  the plugin's retunes ever leave the loop unlocked.
- [ ] **[none] What the calibration of the synthesizer is:** *to select the proper VCO
  frequency and division ratio for any RF frequency* (9.2, p.56), run at power-on and by
  `Calibrate` from standby (13.1.12; 3.5 ms for everything). And a note under the
  synthesizer's table: its phase noise *is not constant over frequency, due to the topology
  of the PLL*, and may differ much between two frequencies close together (table 3-7,
  note 2). So the loop does have settings that go with the frequency. Whether a frequency
  written to the registers, as the plugin writes it, picks them again is not said.
- [ ] **[none] `SetRfFrequency` sets the frequency in FS, TX and RX, and in RX the chip adds
  the IF offset itself** (13.4.1, p.83). Nothing forbids it in receive. That is what 02's
  settle rests on.
- [x] **[none] An image calibration holds for every frequency between its two ends** (9.2.1),
  so a calibration a 32 MHz range is by the book.
- [ ] **[none] Times** (table 8-2, p.53): FS to RX 41 µs, RX to FS 15 µs, standby with the
  crystal running to RX 62 µs; a hop of 10 MHz 30 µs (table 3-7).
- [ ] **[none] Not in Rev 1.2:** anything on a gain control, and any settling time for the
  RSSI. `GetRssiInst` is one sentence (13.5.4). Rev 2.2 has both (its 6.1.6 and its table of
  RSSI settling time by bandwidth); it is read in the section below. Section 15
  has four items and none is about the synthesizer.

## Rev 2.2 of the data sheet, read against Rev 1.2 (24 November)

The file Thomas gave (Rev 2.2, December 2024), all of it. Every section cited above was
compared word for word with Rev 1.2's. Page numbers here are Rev 2.2's.

- [x] **[none] What was cited from Rev 1.2 stands.** 5.1 (p.34), table 8-2 (p.56), 9.2
  (p.60), 9.2.1 (p.61), 9.5, 9.6 (p.62), 13.1.12 and 13.1.13 (p.80-81), 13.4.1 (p.88), 13.6.1
  (p.104; the error bits are table 13-86 here), table 3-7 with its note 2 (p.18-19), table
  3-8, 4.3.1 and section 15 (p.109-111) differ in spelling and layout, and in what is listed
  below. The DC-DC converter's clock, the calibration's sentence on the synthesizer, the hop
  time, the error bits and the four known limitations are as they were. Rev 2.2 speaks for
  neither reading of the echoes more than Rev 1.2 did.
- [x] **[answered by session 7: 1.6 ms / RBW holds] The RSSI's own times, by bandwidth** (13.5.4, table 13-82, p.101-102;
  new). Two columns for GFSK: from receive begun (BUSY falling) to the first value, and from
  one value to the next. At 467 kHz 30 µs and 2 µs; at 58.6 kHz 197 and 16; at 9.7 kHz 1 152
  and 96; at 4.8 kHz 2 295 and 192. So a value every 0.94 ms / RBW in kHz, and the first one
  twelve to fifteen of those after receive begins.
  - The plugin's timing, and `kScopeUpdateMsKhz` here, take an update to be 1.6 ms / RBW:
    1.7 times Semtech's. By Semtech's figure the plugin polls four or five times a value
    (three at 467 kHz), not eight, and its `wait` is five values, not three: the *[later]*
    item on the latency at the top of this file, made worse. Which figure this board keeps
    is one measurement: a sweep timed, points against microseconds.
  - 02's settles (100 µs to 5 ms) lie on both sides of the first-value time for both spans
    of its list (30 µs at 467 kHz, after a hop of 15 MHz; 197 µs at 58.6 kHz). The list
    stands as it is.
  - At a narrow bandwidth the first value is late: 2.3 ms at 4.8 kHz, where the plugin gives
    point 0 two updates more than any other point. Whether a narrow scope's first point is
    read too early has not been looked for in the 4.8 kHz scope of 24 November
    (meshmaster.dk's session 4).
- [ ] **[none] The RSSI is corrected for the gain, and there is a gain control** (6.1.6,
  p.41-42; new). An offset (0x089C, 0x089D) and a tune for each of thirteen gains (0x08F5 to
  0x08FB), with defaults measured on Semtech's own 868-915 MHz board. The section names a
  manual gain mode, the gain control off, for calibrating; it gives neither the register
  that turns the control off nor the gains, and sends the reader to Semtech's driver. So a
  sweep that passes a strong signal passes through gain changes, and the data sheet offers
  no way to hold the gain still.
- [x] **[none] An image calibration for any band is by the book now** (9.2.1, p.61): the two
  bytes are MHz in steps of 4, the lower rounded down and the upper up, where Rev 1.2 said
  to ask Semtech.
- [ ] **[none] A receive bandwidth of 1 MHz** (table 6-5, p.44: `BW1000`, 0x08, and six
  register writes with it; blocking of 45 to 60 dB where the narrow filters have 68 to 80,
  table 3-8). Half the points of a wide scope. Nothing to do with the echoes; not offered.

## The garden (24 November, evening)

Thomas asked for the rest of the hunt to be made on the unit at Bellevue and not on a bench.
Tasks 03 and 04 and the README's *The garden instead of the bench* are that.

- [x] **[run as session 6] Session A** (the README's six lines; the scanner of 0.1.7 that is on the
  unit): whether the echoes move when the span is begun 7 and 2.8 MHz lower. It needs no
  firmware and decides between the two readings.
- [ ] **[later] A step's line is 40 characters and a program six steps**, in meshmaster.dk's
  contract and the Walter's. The lists fit, with whole MHz and nothing to spare in D (384 of
  384 bytes). If the hunt goes on, the two numbers are what to raise.
- [ ] **[none] A session's lines can be had as a file already:** *Download the lines* on the
  session's page, every line as the scanner sent it. `echoes.py` reads it as it is.

## What the garden said: sessions 6 to 9 (24 and 25 November)

Four sessions on the unit at Bellevue, each `restored`, every step run to the clock:
`/noisescope/6` (the README's session A, scanner 0.1.7), then with the scanner of 0.1.8:
`/noisescope/7` (`garden-b`), `/noisescope/8` (zooms, in place of `garden-c`) and
`/noisescope/9` (settle and bandwidths, in place of `garden-d`). Read from each session's own
lines: the frames in which 869.618 MHz reads -70 dBm or more (the neighbour, -32 to -38 dBm)
against the frames in which it is under -85 dBm; a place is an echo where the lit frames'
median stands 6 dB or more over the quiet frames'. Lit frames a step: 9 to 24 of 90 in
session 6, 1 to 14 of 85 in session 7, 3 to 8 of 27 to 40 in session 8, 3 to 20 in session 9.
Where a level rests on one or three frames it is said.

**03 and 04 on the unit** (reviewed by their reports and by what the unit did; neither read
line by line): the four words are taken and named in the `ack`; `us`, `usx` and `dev` are in
the frames; `hunt.py --sessions` took every program before it was typed. Nothing found.

- [x] **[none] There are three artefacts, not one, and the 5.6 MHz comb was none of them.**
  The README's reading of 24 November (*about every 5.6 MHz from a segment's first point*)
  was the layout of one span. Begun 7 and 2.8 MHz lower (session 6), the block at a segment's
  first point moved with it and everything else stayed where it was.
- [ ] **[next, offered] One: a block at a segment's first point, cured by a settle of
  500 µs.** It follows the segments at every bandwidth (467 down to 39 kHz), 13 to 36 dB
  over the quiet floor, and the same points read 4 to 11 dB high even in quiet frames.
  Session 9, `scope 840 900 0 467 1000`: first six points in lit frames -64.5, -74.5, -74.5,
  -78.5, -88.5, -91.5 dBm with no settle; -104, the floor, with `max 32 500` and with
  `max 32 2000`; in quiet frames -98.5 without and -104.5 with. 500 is enough. It costs
  sweeps: 29.1 a second without, 28.0 with 500 µs, 25.8 with 2 000, for two segments. **The
  default is Thomas's to move; the number the garden speaks for is 500.** (This also answers
  whether the chip takes a frequency in continuous receive: it does.)
- [ ] **[next, offered] Two: far echoes at fixed frequencies, and they belong to the wide
  bandwidths.** For a signal on 869.618 MHz: 816.0, 845.0, 887.7 and 912.0 MHz, wherever the
  segments lie. At 467 and 373.6 kHz they are 3 to 4 MHz wide and -55 to -66 dBm (815.7 to
  818.3, 887.4 to 891.1, 911.6 to 915.0). At 234.3 kHz narrower and weaker: 815.9 to 816.4 at
  -75.5, 911.7 to 912.8 at -70.5, 887.7 at -91.5, 845.0 at -94. At 58.6, 46.9 and 39 kHz:
  nothing at 845.0 or 887.7, 816.0 one point at -101.5, **912.0 still there at -81.5 to
  -83.5** (911.92 to 912.15 at all three, so it does not move with the IF of 250, 200 and
  167 kHz), and the crystal's 32.0 MHz either side of the signal at -97 to -105 (837.6 and
  901.6). 816.0 and 912.0 are 25.5 and 28.5 times 32 MHz. **Not the DC-DC converter** (on the
  LDO every one is where it was and as strong), **not the gain** (`noboost`: the same, the
  quiet floor 1 dB up), **not a lost lock** (`dev` `0000` in every frame that carries it, in
  three sessions). What they are is not known.
- [ ] **[none] Three: a raised floor around a strong signal.** About 35 dB under it: out to
  some 10 MHz either side at 467 and 373.6 kHz, -6 to +7 MHz at 234.3, and about 4.4 MHz
  either side at 117 kHz and below, with a lobe at -5.8 to -5.2 and one at +4.8 to +5.1 MHz
  (the *echo at 874.4* of 24 November). The same on the LDO. It is the receiver's own
  selectivity as the RSSI sees it; nothing typed changes it.
- [ ] **[next, offered] The wide picture at 117 kHz is the open question.** One step of
  80 s (`scope 780 960 0 117 4000`, 1 535 points, seven segments) had three lit frames:
  912.0 at -78 dBm, nothing at 816.0, 845.0 or 887.7, and blocks at 796.1, 824.7, 889.8,
  893.1, 921.9 and 939.2 MHz that three frames cannot tell from real signals. A longer
  capture at 117 and at 58.6 kHz decides whether the wide picture moves there.
- [x] **[none] An update is 1.6 ms / RBW on this chip**, as the plugin has it and not as
  Rev 2.2's table reads: 250 points at 4.8 kHz in one segment, shortest sweep 88.9 ms
  (83 by 1.6, 49 by 0.94); 990 points at 58.6 kHz, 34.2 ms.
- [ ] **[later] And a sweep takes up to twice that when readings stand still:** the same
  250 points, median 147 ms, longest 174 ms. That is the *[later]* item at the top of this
  file, measured: the pacing follows the signal.
- [ ] **[later] `us` steps by 977 µs.** The board's `micros()` moves in 1/1024 s, so six
  plugin calls at 467 kHz read 5 858 µs every time and say nothing. Good enough at 58.6 kHz
  and below. A finer clock (the nRF52's timer) if the number is ever wanted at 467 kHz.
- [ ] **[later] `garden-c` was not run** (a segment's width): session 6 took its question
  away. `garden-d` was run in part (the three 60 MHz lines; not the 13 MHz ones).
