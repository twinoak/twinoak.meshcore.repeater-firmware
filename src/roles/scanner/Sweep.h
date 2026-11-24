// The scanner's arithmetic: what a sweep's points are, which receiver bandwidth a step needs,
// what a command's numbers are clamped to, how one frequency of a `hist` is measured, and the
// window of RSSI samples that `stat.rssi` and `dwell` report.
//
// This is NoiseScope's scanner.cpp with the radio taken out of it
// (software/firmware/noisescope-rak4630/src/scanner.cpp at b798aab0 of the hardware repository:
// sweep_common_setup, pick_rbw, the monitor and dwell accumulators and the four range checks).
// The radio stays in src/roles/scanner/Radio.cpp, which is thin because this file holds the
// decisions; this one is host-portable - no Arduino, no RadioLib, C++11 - and compiled by
// [env:native] beside test/test_scanner.
//
// The monitor and the dwell kept two copies of the same accumulator in NoiseScope. They are one
// class here: a window counts samples above a threshold, and the threshold of the next window is
// the minimum of the one that just ended plus kBurstThresholdDeltaDb - which is what makes
// `dc` a burst duty-cycle number rather than a count of samples above a number somebody chose.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace twinoak {
namespace scanner {

// ------------------------------------------------------------------ the numbers

// The accumulators are fixed arrays of this length: 512 points is 12.8 MHz at the default 25 kHz
// step and the whole of any band this firmware knows.
constexpr uint16_t kMaxSweepPoints = 512;

// Fixed by the SX126x's spectral-scan engine: 33 bins of 4 dB from -11 dBm down to -139 dBm.
constexpr int kHistBins = 33;

// The SX1262 tunes 150-960 MHz (its datasheet, and RadioLib's own check). A sweep may be pointed
// outside the band the image is in - at a suspected blocker, through the cavity filter - which is
// the point of the instrument, so this is the radio's range and not the band's.
constexpr double kRadioLowMhz = 150.0;
constexpr double kRadioHighMhz = 960.0;

// The duty-cycle threshold is the previous window's minimum plus this.
constexpr double kBurstThresholdDeltaDb = 10.0;

// The first window of a session has no previous minimum to work from.
constexpr double kFirstThresholdDbm = -100.0;

// Below any RSSI the receiver reports, so that a window with no sample is recognisable rather
// than plausible.
constexpr double kNoReadingDbm = -200.0;

// The FSK receiver bandwidths the SX1262 supports, in kHz, in order.
extern const double kFskRbwKhz[];
extern const size_t kFskRbwCount;

// ------------------------------------------------------------------ the sweep

// The narrowest supported receiver bandwidth at least `khz` wide, and the widest one when none
// is that wide: the table above is a set of fixed filters and a request lands on one of them.
double narrowestRbwKhz(double khz);

// The smallest supported receiver bandwidth at least 1.1 times the step, so that the steps of a
// sweep overlap slightly rather than leaving gaps; the widest one when no bandwidth is that
// wide.
double pickRbwKhz(double step_khz);

// A sweep, planned: the ends, the step, the receiver bandwidth it needs and how many points it
// has. `points` is 0 for a request this firmware does not run.
struct SweepPlan {
  double f0 = 0.0;
  double f1 = 0.0;
  double step_khz = 0.0;
  double rbw_khz = 0.0;
  uint16_t points = 0;
};

// False - and `out` untouched - when the three numbers are not a sweep: the ends crossed or
// outside the radio's range, a step below 1 kHz or above 1 MHz, fewer than two points or more
// than kMaxSweepPoints.
bool planSweep(double f0, double f1, double step_khz, SweepPlan& out);

// The frequency of point `i`, in MHz: f0 + i * step. Points are indexed from 0 and a reader of
// a `sweep` line computes the same thing (docs/noisescope.md).
double pointFrequencyMhz(const SweepPlan& plan, uint16_t index);

// Whether a single frequency is one the radio tunes - `dwell` and `cad` take one.
bool frequencyOk(double mhz);

// What a sweep's dwell must be: long enough for the receiver's AGC to settle, short enough that
// a full pass is seconds and not minutes.
constexpr uint16_t kSweepDwellMinMs = 5;
constexpr uint16_t kSweepDwellMaxMs = 500;
bool sweepDwellOk(uint32_t dwell_ms);

// The clamps the other three activities apply to their own arguments, each as NoiseScope had
// them. A clamp and not a refusal: a `dwell 869.618 0` is a dwell of one second, which is what
// the operator meant.
uint16_t clampHistSamples(uint32_t samples);
uint32_t clampDwellSeconds(uint32_t secs);
uint32_t clampDwellWindowMs(uint32_t win_ms);
uint16_t clampCadCount(uint32_t count);

constexpr uint16_t kHistSamplesMin = 256;
constexpr uint32_t kDwellSecondsMax = 86400;
constexpr uint32_t kDwellWindowMinMs = 100;
constexpr uint32_t kDwellWindowMaxMs = 60000;
constexpr uint16_t kCadCountMax = 1000;

// Whether the home channel's modem settings are ones the SX1262 has: `home` takes them from the
// operator and a bad spreading factor must not reach the radio.
bool homeSettingsOk(double f_mhz, uint8_t sf, uint8_t cr);

// ------------------------------------------------------------------ the histogram
//
// `hist` measures one frequency after another with the SX126x's own spectral-scan engine: the
// chip takes `n` RSSI samples kHistSampleUs apart and counts each one into the kHistBins bins of
// 4 dB, and the host reads the counters out when the scan is over. **A line's bins therefore add
// up to its `n`** - and on 21 November, on a unit at Bellevue, they did not: one pass of
// `hist 869.418 869.818 25` at 2 048 samples gave seven lines adding up to 2 048, 15, 15, 8,
// 4 140, 32 and 1 807, which is a result read 70 to 360 us after its scan was started
// (tasks/phase-3.14/FOLLOWUPS.md, *The scanner's histogram*; phase 3.15's task 01).
//
// The sequence for one frequency is here rather than in src/roles/scanner/Radio.cpp - host-
// portable, driven by a test through HistRadio, as armScopeSegment() is for the scope - because
// the order of the calls and the two things it does not take the chip's word for are the whole
// of the fix:
//
//   1. **a result belongs to the scan started for it.** A scan of `n` samples cannot have
//      finished before histScanUs() has passed since it was started, so a *completed* status
//      read sooner is not this scan's and is not acted on; the counters are read once, after
//      the completion, and never while the scan runs;
//   2. **the sum is the proof.** The bins are added up before anything is said. They come to
//      `n` or the result is not a histogram: the frequency is measured again, kHistScansMax
//      scans at most, and then the caller says so in one `err` line and ends the pass.

// The engine's sample period, in microseconds: the interval the scan is started with
// (RADIOLIB_SX126X_SCAN_INTERVAL_8_20_US, which src/roles/scanner/Radio.cpp passes by name).
// **This is the one place the number is written.** The earliest a scan can be over, the longest
// it is waited for, and every duration docs/noisescope.md quotes for a `hist` come from it.
constexpr double kHistSampleUs = 8.2;

// How many scans one frequency is given: a result whose bins do not add up to its `n` is not a
// histogram of anything, so it is measured again instead of being sent.
constexpr uint8_t kHistScansMax = 3;

// On top of the scan's own duration, before it is given up on. The wait is histScanUs() doubled
// plus this: the chip is asked for its status over SPI and both serial ports are drained between
// two asks, and the scan's own duration is the only thing that scales with `samples`. (It was
// `samples / 100 + 100` ms, beside a comment that said 8.2 us.)
constexpr uint32_t kHistWaitSlackMs = 50;

// How long the scan itself takes, in microseconds: `samples` x kHistSampleUs, rounded up.
uint32_t histScanUs(uint16_t samples);

// The same in whole milliseconds, rounded up - what docs/noisescope.md section 5 quotes a line
// of a `hist` as costing (17 ms at the default 2 048 samples, 0.54 s at 65 535).
uint32_t histScanMs(uint16_t samples);

// The longest one scan is waited for, in microseconds: histScanUs() doubled plus
// kHistWaitSlackMs. A scan that has not said *completed* by then is aborted and the pass ends
// with the `scan timeout` it has always ended with.
uint32_t histWaitUs(uint16_t samples);

// The four values of the engine's status register, as the chip writes them (RadioLib 7.8.0,
// SX126x_registers.h:128-132: RADIOLIB_SX126X_SPECTRAL_SCAN_NONE, _ONGOING, _ABORTED and
// _COMPLETED, held equal to these four by a static_assert in src/roles/scanner/Radio.cpp).
// RadioLib's own spectralScanGetStatus() collapses them into *completed or not*; this sequence
// reads the register itself, because which of the other three it is matters.
constexpr uint8_t kHistStatusNone = 0x00;
constexpr uint8_t kHistStatusOngoing = 0x0F;
constexpr uint8_t kHistStatusAborted = 0xF0;
constexpr uint8_t kHistStatusCompleted = 0xFF;

// The radio, as measuring one frequency sees it: one member per call of RadioLib's - or of the
// Module's, for the two that are one register - plus the clock and the loop's own poll, handed
// in so that a host test is what moves time. src/roles/scanner/Radio.cpp is the chip half of
// each member and nothing else.
struct HistRadio {
  virtual ~HistRadio() {}

  // SX126x::standby(), then SX1262::setFrequency(float): the frequency this scan is for.
  virtual int standby() = 0;
  virtual int setFrequency(double mhz) = 0;

  // The status register put back to kHistStatusNone before a scan is started. Nothing in
  // RadioLib's spectralScanStart() touches it - an abort, the RSSI window register, SetRx and
  // the scan parameters, and that is all (7.8.0, SX126x.cpp:1191) - and its abort writes the
  // window register alone (SX126x.cpp:1208), so a *completed* left over from the frequency
  // before would otherwise be read as this scan's. Written here, and then **not relied on**:
  // whether the chip's own patch keeps a host's write of that register is not known, and no
  // session has a chip to ask.
  virtual void clearStatus() = 0;

  // SX126x::spectralScanStart(samples) and SX126x::spectralScanAbort().
  virtual int scanStart(uint16_t samples) = 0;
  virtual void scanAbort() = 0;

  // The status register, raw: one of the four values above.
  virtual uint8_t scanStatus() = 0;

  // SX126x::spectralScanGetResult(): kHistBins counters, read once and after the completion.
  virtual int scanResult(uint16_t* bins) = 0;

  // Not RadioLib's. The clock in microseconds (micros() on the board), and one turn of what the
  // loop does during a blocking step: both serial ports drained so that a `stop` is never lost,
  // and yield(). A command that arrives during a scan is still executed between steps, from
  // loop(), exactly as before.
  virtual uint32_t nowUs() = 0;
  virtual void poll() = 0;
};

// What measuring one frequency came to.
enum HistOutcome : uint8_t {
  kHistMeasured = 0,     // the bins are a histogram of this scan and they add up to `n`
  kHistStandbyFailed,    // standby() refused
  kHistFrequencyFailed,  // setFrequency() refused: the scan would have been another frequency's
  kHistStartFailed,      // spectralScanStart() refused
  kHistNeverCompleted,   // no *completed* status inside histWaitUs(); the scan was aborted
  kHistResultFailed,     // spectralScanGetResult() refused
  kHistDoesNotAddUp,     // kHistScansMax scans, none of whose bins added up to `n`
};

struct HistResult {
  HistOutcome outcome = kHistMeasured;
  int code = 0;       // RadioLib's, for the three outcomes that have one
  uint8_t scans = 0;  // scans taken, 1..kHistScansMax
  uint32_t sum = 0;   // what the last result's bins added up to, which an `err` line names
  // A *completed* status read before the scan could have finished - the chip of 21 November,
  // and the reading this sequence was written for. Not acted on, and recorded because it is the
  // one thing a session with no part can still be told by one.
  bool completed_early = false;
};

// Measures `f_mhz`: standby, the frequency, and then up to kHistScansMax scans of `samples`
// samples each - the status register cleared, the scan started, its completion waited for and
// not believed before histScanUs() has passed, the kHistBins counters read once, and the bins
// added up. True with `bins` holding a histogram that adds up to `samples`; false with `out`
// saying what went wrong, and then **no `hist` line is written for this frequency**.
bool measureHistPoint(HistRadio& radio, double f_mhz, uint16_t samples, uint16_t* bins,
                      HistResult& out);

// The `err` line that frequency gets, written into `out` (docs/noisescope.md section 5). The
// words are here and not in src/roles/scanner/Radio.cpp so that they are held by a test:
// `scan start failed`, `scan timeout` and `scan result failed` are what the firmware has always
// said, two name a standby or a retune the driver refused, and the one for a result that does
// not add up names the frequency, the sum and `n` - the three numbers a reader needs to tell a
// chip that will not scan from a pass that was asked for the wrong thing. An outcome of
// kHistMeasured writes the empty string: nothing went wrong.
void histErrorMessage(const HistResult& result, double f_mhz, uint16_t samples, char* out,
                      size_t size);

// The longest of those messages, newline and NUL aside.
constexpr size_t kHistErrorBytes = 160;

// ------------------------------------------------------------------ the scope (the fast sweep)
//
// `scope` is the same measurement as `sweep` with the stepping moved into the radio: a program
// for the SX126x's own core retunes after every RSSI update and leaves one level per point in
// the data buffer, so 250 points cost one SPI command and some tens of milliseconds instead of
// seconds (src/roles/scanner/vendor/sx126x_spectrum/, docs/noisescope.md section 6).
//
// Everything that decides *what to ask the radio for* is here, and host-tested: the span cut
// into segments, the receiver bandwidth, how much of the span a reading actually covers, what
// one frame costs a sink and how long a sweep holds the chip. src/roles/scanner/Radio.cpp does
// the asking and nothing else.

// The frame carries one byte per point, so 2048 is what a line of the protocol can hold and
// still be read by the Walter's verbatim forwarding; 16 is the fewest points a spectrum is.
constexpr uint16_t kScopeMinPoints = 16;
constexpr uint16_t kScopeMaxPoints = 2048;

// What `scope <f0> <f1>` with no point count asks for: one plugin call for any span up to
// kScopeSegmentMhz, and the number the fork's own timings are quoted for (~21 ms at 19.5 kHz) -
// so the default is the live waterfall the whole mode exists for. A `rbw_khz` without a point
// count asks for a *resolution* instead, and then the default is as many points as that
// bandwidth covers without gaps (at most kScopeMaxPoints).
constexpr uint16_t kScopeDefaultPoints = 250;

// The plugin counts points plus skipped RSSI updates in one byte, so one call is at most this
// (SX126xSpectrumScanner::MAX_POINTS, which this is held against by a static_assert in
// src/roles/scanner/Radio.cpp).
constexpr uint16_t kScopePluginMaxPoints = 250;

// A segment's width, and the whole reason a wide span is several of them: `setFrequency()` at a
// span's centre calibrates the image rejection for one band - one of five Semtech presets, or
// +-4 MHz around the frequency - so a point far from the centre is measured through a worse
// calibration (RadioLib 7.8.0, SX126x::calibrateImage). The fork caps a sweep at 32 MHz for
// that reason and this starts there; the bench lowers it if a ghost shows
// (docs/bench.md section 9).
//
// It is both **the default and the widest a segment may be**: `scope`'s sixth number asks for a
// narrower one, because the blocks a wide scope shows repeat about every 5.6 MHz from wherever a
// segment began and the synthesizer dragged that far is the guess at what makes them
// (tasks/phase-3.16/README.md). What a `scope` with no such number takes does not move: that is
// decided after the bench's tables, not here (tasks/phase-3.16/FOLLOWUPS.md).
constexpr double kScopeSegmentMhz = 32.0;

// The longest settle `scope`'s seventh number may ask for: how long the receiver stands on the
// segment's first point, in continuous receive, before the sweep that counts. 20 ms is far more
// than anything the plugin's own two extra updates allow (6.9 us at 467 kHz) and still a fraction
// of the 8 s watchdog, with every segment a separate pass of loop() besides.
constexpr uint32_t kScopeSettleMaxUs = 20000;

// The radio's frequency unit is 32 MHz / 2^25, about 0.95 Hz; the fork asks for at least 100 Hz
// a point and so does this.
constexpr double kScopeMinStepKhz = 0.1;

// The widest of the receiver bandwidths above, which is what a `rbw_khz` asked for is capped at
// and the whole reason a wide span has gaps. Written out because it is in a refusal's message
// and in the page; test/test_scanner holds it against the table.
constexpr double kScopeRbwMaxKhz = 467.0;

// A segment carries at least one point, so a plan never has more segments than points - and
// that is the only bound there is, deliberately: lowering kScopeSegmentMhz at the bench makes
// the segments narrower and more numerous, and a scope of the whole tuning range must not start
// being refused because of it. The worst case at 32 MHz is 26 segments (810 MHz in 51 points),
// which test/test_scanner finds by walking every point count the command takes.
constexpr uint16_t kScopeMaxSegments = kScopeMaxPoints;

// A level the radio never reported: the frame's `255`, and what a point of a segment that did
// not run reads as.
constexpr uint8_t kScopeNoReading = 255;

// An RSSI register update takes about 1.6 ms / RBW in kHz, and a point about one update; a
// segment pays a few more for the retune into its first point (the plugin's own comment in
// spectrum.asm: point 0 gets two extra updates, and the jump from the previous segment settles
// slower than a step). Eight is that, rounded up, and it is what makes the bound below a bound.
constexpr double kScopeUpdateMsKhz = 1.6;
constexpr uint16_t kScopeSegmentUpdates = 8;

// What a sink is allowed to spend on a scope: at most one part in this of its time, so that the
// `stat` heartbeat, an `ack` and the next command still get through and the UART writer never
// blocks the loop that feeds the watchdog.
constexpr uint32_t kScopeSinkDutyDivisor = 2;

// Between frames: 0 is a frame per sweep, and a minute is the longest wait that is still a
// stream.
constexpr uint32_t kScopeIntervalMaxMs = 60000;

// The ROM the plugin is made for, and the only one it is uploaded to: its author verified it on
// an SX1262 whose version string (register 0x0320, 16 characters) reads this, and the fork's own
// scanner class refuses any other. Fifteen characters are compared, as it compares them.
extern const char kScopeRom[];
constexpr size_t kScopeRomChars = 15;

// False for every other ROM - and then `scope` uploads nothing at all and says which ROM it met.
bool scopeRomSupported(const char* rom);

// What a frame holds of the sweeps since the previous one.
enum ScopeAgg : uint8_t {
  kScopeAggMax = 0,   // the strongest level of each point, so a short burst shows
  kScopeAggMean = 1,  // the mean in the power domain
  kScopeAggLast = 2,  // the latest sweep
};

// The word the command takes and the frame carries, and the other way round. False for a word
// that is not one of the three.
const char* scopeAggName(ScopeAgg agg);
bool scopeAggOf(const char* word, ScopeAgg& out);

// One plugin call: a run of consecutive points of the frame, with the first point's centre and
// the range its image rejection is calibrated for (RadioLib's calibrateImageRejection()).
//
// That range is **what the segment actually measures**: from half a receiver bandwidth below its
// first point's centre to half a bandwidth above its last one. Not the nominal bins - a segment
// of one point whose step is 50 MHz wide measures one frequency and is calibrated for that one,
// not for the 50 MHz of span the point stands for.
struct ScopeSegment {
  uint16_t first = 0;       // index of its first point in the frame
  uint16_t points = 0;
  double first_mhz = 0.0;   // the centre of that point
  double cal_lo_mhz = 0.0;  // that centre - rbw/2
  double cal_hi_mhz = 0.0;  // the last point's centre + rbw/2
};

// The four calls that arm a segment before its sweep, in the order src/roles/scanner/Radio.cpp
// makes them: standby, the centre frequency, the image calibration for the segment's range, and
// the plugin's parameters. The `err` line of a segment that would not arm names the one that
// returned the code (docs/noisescope.md section 5), because the frequency alone cannot tell them
// apart and a report from the field needs to (tasks/phase-3.1/FOLLOWUPS.md, the walk of
// 22 October 2026). RadioLib's SX126x parses the status byte the chip clocks out *during* a
// command and asks for no GetStatus after it (RadioLib 7.8.0, SX126x::modSetup sets
// parseStatusCb and no checkStatusCb; Module::SPItransferStream), so the status a call returns
// is the chip's verdict on the command *before* it - the call named is where the code surfaced.
enum ScopeArmCall : uint8_t {
  kScopeArmNone = 0,            // nothing failed, or nothing was called
  kScopeArmStandby = 1,         // SX126x::standby()
  kScopeArmSetFrequency = 2,    // SX1262::setFrequency(centre), which may calibrate by itself
  kScopeArmCalibrateImage = 3,  // SX126x::calibrateImageRejection(cal_lo, cal_hi)
  kScopeArmSetSweep = 4,        // SX126xSpectrumScanner::setSweep(...)
  kScopeArmSweep = 5,           // SX126xSpectrumScanner::sweep(), the sweep itself
  kScopeArmReload = 6,          // the chip reset and the plugin uploaded again (below)
  kScopeArmSettle = 7,          // the retune onto the segment's first point before the settle
};

// The word the `err` line carries for each: "standby", "setFrequency", "calibrateImage",
// "setSweep", "sweep", "reload", "settle"; "" for none.
const char* scopeArmCallName(ScopeArmCall call);

// What `scope <f0> <f1> [points [rbw_khz [interval_ms [agg]]]]` asked for; 0 is "the default"
// for the three numbers that have one. `sink_baud` is the port the command came in on: 0 for a
// sink with no floor worth computing (USB CDC), and then the interval taken is the one asked
// for.
struct ScopeRequest {
  double f0 = 0.0;
  double f1 = 0.0;
  uint32_t points = 0;
  double rbw_khz = 0.0;
  uint32_t interval_ms = 0;
  ScopeAgg agg = kScopeAggMax;
  uint32_t sink_baud = 0;
  // The widest a segment may be, in MHz: 0 is kScopeSegmentMhz, today's. Taken to a tenth of a
  // MHz, and refused below one step or above kScopeSegmentMhz.
  double seg_mhz = 0.0;
  // How long the receiver stands on a segment's first point before the sweep that counts: 0 is
  // today's arm, which stands there for the plugin's own two extra updates and no longer.
  uint32_t settle_us = 0;
};

// A scope, planned. `points` is 0 for a request this firmware does not run.
struct ScopePlan {
  double f0 = 0.0;          // the span as asked, both edges
  double f1 = 0.0;
  double first_mhz = 0.0;   // the first point's centre: f0 + step/2, which the frame's `f0` is
  double step_khz = 0.0;
  double rbw_khz = 0.0;
  double cov = 0.0;         // rbw / step: below 1 where a narrow carrier can fall between points
  uint16_t points = 0;
  uint16_t segments = 0;
  uint32_t interval_ms = 0;  // what was taken, the sink's floor included
  ScopeAgg agg = kScopeAggMax;
  uint32_t segment_ms = 0;   // the longest one plugin call holds the chip
  uint32_t sweep_ms = 0;     // and the whole span, every segment of it, settles included
  double seg_mhz = kScopeSegmentMhz;  // the widest a segment of this plan may be, as taken
  uint32_t settle_us = 0;             // and the settle before each segment's first reading
};

// False - and `out` untouched - when the request is not a scope: the ends crossed or outside the
// radio's range, a point count given and outside kScopeMinPoints..kScopeMaxPoints, a step below
// kScopeMinStepKhz, a negative bandwidth, an interval above kScopeIntervalMaxMs, a segment width
// given and above kScopeSegmentMhz or narrower than one step, or a settle above
// kScopeSettleMaxUs.
bool planScope(const ScopeRequest& request, ScopePlan& out);

// Segment `index` of a plan (0 .. plan.segments - 1). False for an index the plan does not have.
bool scopeSegment(const ScopePlan& plan, uint16_t index, ScopeSegment& out);

// The radio, as arming a segment sees it: one member per call, each exactly one call of
// RadioLib's or the plugin's and nothing else (src/roles/scanner/Radio.cpp is the chip half).
// The order they are made in is armScopeSegment()'s, below, and host-tested - because the order
// is the whole fix of p3.5-02 (tasks/phase-3.5/reports/02.md).
struct ScopeArmRadio {
  virtual ~ScopeArmRadio() {}
  virtual int standby() = 0;
  // SetRfFrequency and nothing else. RadioLib's SX1262::setFrequency() calibrates the image by
  // itself before a move of 20 MHz or more (7.8.0, SX1262.cpp:178-189,
  // RADIOLIB_SX126X_CAL_IMG_FREQ_TRIG_MHZ), +-4 MHz around the *new* centre and *before* the
  // frequency has moved. Made right after a plugin sweep, that calibration is the one the first
  // board refused with IMG_CALIB_ERR at every third segment, and the chip then refuses every
  // calibration after it until a hardware reset. The chip half passes skipCalibration = true.
  virtual int setFrequency(double mhz) = 0;
  virtual int calibrateImage(double lo_mhz, double hi_mhz) = 0;
  // The chip's GetDeviceErrors word, read in standby (where it answers).
  virtual unsigned deviceErrors() = 0;
  virtual int setSweep(double first_mhz, double step_mhz, uint16_t points, double rbw_khz) = 0;
  // The receiver left standing where it is, in continuous receive, for `us` microseconds. The
  // chip half is a delay and nothing else; the default is to stand there for no time at all, so
  // that a radio written before this task - every fake of test/test_scanner - is still one.
  virtual void settle(uint32_t us) { (void)us; }
  // A hardware reset of the chip and the plugin uploaded again, as the scope's start does it:
  // back in standby with its device errors cleared and nothing of the last sweep left. The one
  // thing that was found to clear a refused calibration (tasks/phase-3.5/reports/02.md).
  virtual int reload() = 0;
};

// GetDeviceErrors bit 4, IMG_CALIB_ERR: the image calibration failed (SX1261/2 datasheet
// 13.5.5; RadioLib's RADIOLIB_SX126X_IMG_CALIB_ERR). The chip says this in its *status*, but
// the driver reads the status byte clocked out during the command before it, and the next
// command after our calibration begins with a NOP that masks it - so the arm asks the chip
// outright, and a calibration that failed is an `err` and not a sweep through a bad image.
constexpr unsigned kScopeImageCalibrationError = 0x0010;

// The code the `err` line carries for that case: RadioLib's RADIOLIB_ERR_SPI_CMD_FAILED
// (-707, TypeDef.h:440 at 7.8.0), the code the driver returns when it does see the chip's
// failure to execute. A static_assert in src/roles/scanner/Radio.cpp holds the two equal.
constexpr int kScopeArmCalibrationFailed = -707;

struct ScopeArmResult {
  ScopeArmCall failed = kScopeArmNone;  // kScopeArmNone when the segment is armed
  int code = 0;
  unsigned device_errors = 0;  // what the chip said after the last calibration (0 before it)
  bool reloaded = false;       // the chip was reset and the plugin uploaded again on the way
};

// The segment's centre as the plugin's setSweep() computes it - first + step * points / 2, in
// float, the fork's own formula - which is what the frequency is set to before the calibration,
// so that setSweep()'s own setFrequency() of the same value is a move of zero and calibrates
// nothing.
float scopeSegmentCentreMhz(const ScopePlan& plan, const ScopeSegment& seg);

// The range the chip's image calibration was last made for, carried from one arm of a sweep to
// the next so that a segment already inside it is armed without a second one. Invalid - and so
// never inside anything - after a reset of the chip, which is what clears a calibration.
struct ScopeCalibration {
  bool valid = false;
  double lo_mhz = 0.0;
  double hi_mhz = 0.0;
};

// How far outside a calibrated range a segment may still be called inside it: a kilohertz, which
// is the rounding of the arithmetic above and nothing a calibration can tell apart (RadioLib
// asks the chip for the band in units of 4 MHz, SX126x::calibrateImageRejection).
constexpr double kScopeCalibrationSlackMhz = 0.001;

// What one arm's calibration covers. **Today's rule is one calibration a segment, for the
// segment's own range**, and a plan whose segments are as wide as kScopeSegmentMhz allows gets
// exactly that - which is every plan a `scope` with no width asked for makes. A plan cut
// narrower would otherwise pay a calibration for every one of thirty-three segments, so its
// calibration is made for **kScopeSegmentMhz at a time** - the width a calibration was always
// made for - from its own first point upwards, and never past the plan's last point.
void scopeCalibrationRange(const ScopePlan& plan, const ScopeSegment& seg, double& lo_mhz,
                           double& hi_mhz);

// The receiver put **on the segment's first point** and left standing there, in continuous
// receive, for the plan's settle - the last thing an arm does, and the only thing done between
// two sweeps of a segment that is already armed (src/roles/scanner/Radio.cpp, serviceScope).
//
// Why a retune and not a sweep of one point: the plugin's setSweep() is what loads the sweep that
// counts, and it standbys the chip and puts the receiver at the **segment's centre** before the
// plugin's own jump to point 0 (vendor/sx126x_spectrum/SX126xSpectrumScanner.h, setSweep ~L130).
// So nothing done before setSweep() can leave the receiver on the first point, and the only move
// left afterwards is the one the plugin itself makes all sweep long: the frequency written with
// the receiver in continuous receive and no calibration (spectrum.asm, `retune`: REG_FREQ
// [0x6B..0x6E], "checked on hardware against SetRfFrequency"). ScopeArmRadio::setFrequency() is
// that write, so the plugin's own retune into point 0 is then a move of zero and its two extra
// updates are spent on a synthesizer that has already settled.
//
// Nothing at all when the plan asked for no settle: a `scope` without the two numbers makes
// exactly the calls it made before this task.
bool settleScopeSegment(ScopeArmRadio& radio, const ScopePlan& plan, const ScopeSegment& seg,
                        ScopeArmResult& out);

// Arms `seg`: standby, the centre frequency, the image calibration for the segment's own range,
// the chip's device errors read, the plugin's parameters, the settle - in that order and no
// other. True when armed; false with `out` naming the call that returned the code.
//
// `cal` is what the chip was last calibrated for, read and written: a segment that lies inside
// it is armed with neither a calibration nor the question to the chip that goes with one
// (scopeCalibrationRange above), and a reload invalidates it because the reset did. The overload
// without it calibrates every segment, which is what this function did before phase 3.16's task
// 02 and what test/test_scanner holds it to.
//
// A calibration the chip refuses is retried **once, after a reload**. On the first RAK4630 the
// SX1262 refuses an image calibration that reaches more than about 20 MHz above where a plugin
// sweep left it, whenever that sweep ended below about 205 MHz - IMG_CALIB_ERR, and every
// calibration after it refused too - and nothing short of a hardware reset was found to clear
// it: not a standby in either mode, not an FS or an RX lock at the new frequency, not a
// frequency move across the whole range, not the PLL's own Calibrate (the probe table in
// tasks/phase-3.5/reports/02.md). After the reset and the upload the same arm is accepted, and
// the next ones with it: one reload per pass over 150-960 MHz, at the second segment. The
// refusal is the chip's, so the answer is the chip's own verdict and not a table of
// frequencies: refused once, reload and ask again; refused twice, an `err` that says so.
bool armScopeSegment(ScopeArmRadio& radio, const ScopePlan& plan, const ScopeSegment& seg,
                     ScopeCalibration& cal, ScopeArmResult& out);
bool armScopeSegment(ScopeArmRadio& radio, const ScopePlan& plan, const ScopeSegment& seg,
                     ScopeArmResult& out);

// How many bytes the NDJSON line of a frame of `points` points is at most, newline included:
// the base64 of one byte per point plus the longest the envelope can be. The emitted line is
// held to this by test/test_scanner.
uint32_t scopeFrameBytes(uint16_t points);
uint32_t scopeBase64Length(uint16_t bytes);

// The shortest interval a sink of `baud` bits a second can carry that frame at: 8N1, so ten bits
// a byte, times kScopeSinkDutyDivisor. 0 for `baud` 0.
uint32_t scopeIntervalFloorMs(uint16_t points, uint32_t baud);

// How long one plugin call of `points` points holds the chip, and how long a whole sweep of the
// plan does: `points` RSSI updates plus kScopeSegmentUpdates a segment, at 1.6 ms / RBW each.
// The first is what must stay inside RadioLib's 1000 ms wait for BUSY (Module.h:224) and the
// second inside the 8 s watchdog, which test/test_scanner holds them to.
uint32_t scopeSegmentMs(uint16_t points, double rbw_khz);
uint32_t scopeSweepMs(uint16_t points, uint16_t segments, double rbw_khz,
                      uint32_t settle_us = 0);

// What the `ack` of a `scope` that started carries: what was *taken*, which is not always what
// was asked (docs/noisescope.md section 5). Here and not where the ack is sent, so that a test
// can hold it to the byte - and the bytes of a `scope` asked for with neither a segment width nor
// a settle are the ones this line had before phase 3.16's task 02: the two numbers are named only
// when one of them is not the default.
void scopeAckMessage(const ScopePlan& plan, char* out, size_t bytes);

// And the sentence a request this firmware does not run is refused with, which names every bound
// planScope() has.
extern const char kScopeRefusal[];

// Sweeps per second times ten, as the frame's `sps` carries it: 0 when no time has passed.
uint16_t scopeSweepsPerSecondX10(uint32_t sweeps, uint32_t elapsed_ms);

// The sweeps since the last frame, added up point by point. One of these is a frame: `add()` for
// each segment the radio answered, `sweepDone()` when a whole sweep has gone in, `take()` to
// collapse what has accumulated into the bytes a frame carries.
//
// A point nothing was read for stays kScopeNoReading, and a mean is the mean of the readings a
// point actually got and not of the sweeps that happened - a segment that failed must not pull
// a neighbouring point down.
class ScopeAccumulator {
 public:
  // Begins a frame of `points` points: every point no reading, no sweep counted.
  void reset(uint16_t points);

  // One segment's levels, as the plugin left them: `count` bytes for the points from `first`.
  void add(uint16_t first, const uint8_t* levels, uint16_t count);

  // One whole sweep of every segment has gone in.
  void sweepDone() { sweeps_++; }
  uint16_t sweeps() const { return sweeps_; }
  uint16_t points() const { return points_; }

  // Collapses the frame into `out`, which must hold `points()` bytes.
  void take(ScopeAgg agg, uint8_t* out) const;

 private:
  uint16_t points_ = 0;
  uint16_t sweeps_ = 0;
  uint8_t max_[kScopeMaxPoints];
  uint8_t last_[kScopeMaxPoints];
  uint16_t count_[kScopeMaxPoints];
  float power_[kScopeMaxPoints];
};

// ------------------------------------------------------------------ the window

// What one window of RSSI samples came to: the fields of `stat.rssi` and of a `dwell` line
// (docs/noisescope.md).
struct WindowStats {
  uint32_t n = 0;
  double avg = kNoReadingDbm;
  double max = kNoReadingDbm;
  double min = kNoReadingDbm;
  double dc = 0.0;  // the fraction of samples above `th`
  double th = kFirstThresholdDbm;
};

// A window of instantaneous-RSSI samples. `add()` for every sample, `stats()` for what they came
// to, `reset()` to begin the next window - which carries the threshold over from the one that
// ended.
class RssiWindow {
 public:
  void add(double dbm);

  uint32_t count() const { return n_; }
  double threshold() const { return th_; }

  // False when nothing has been sampled, and `out` is untouched then: an empty window is not an
  // average of zero.
  bool stats(WindowStats& out) const;

  // Clears the counters and takes the next window's threshold from this one's minimum. Nothing
  // is carried over from a window that had no sample.
  void reset();

  // Back to the state of a fresh session: no samples and kFirstThresholdDbm.
  void restart();

 private:
  uint32_t n_ = 0;
  uint32_t over_ = 0;
  double sum_ = 0.0;
  double max_ = kNoReadingDbm;
  double min_ = -kNoReadingDbm;
  double th_ = kFirstThresholdDbm;
};

}  // namespace scanner
}  // namespace twinoak
