// The scanner's arithmetic: what a sweep's points are, which receiver bandwidth a step needs,
// what a command's numbers are clamped to, and the window of RSSI samples that `stat.rssi` and
// `dwell` report.
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
constexpr double kScopeSegmentMhz = 32.0;

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
  uint32_t sweep_ms = 0;     // and the whole span, every segment of it
};

// False - and `out` untouched - when the request is not a scope: the ends crossed or outside the
// radio's range, a point count given and outside kScopeMinPoints..kScopeMaxPoints, a step below
// kScopeMinStepKhz, a negative bandwidth, or an interval above kScopeIntervalMaxMs.
bool planScope(const ScopeRequest& request, ScopePlan& out);

// Segment `index` of a plan (0 .. plan.segments - 1). False for an index the plan does not have.
bool scopeSegment(const ScopePlan& plan, uint16_t index, ScopeSegment& out);

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
uint32_t scopeSweepMs(uint16_t points, uint16_t segments, double rbw_khz);

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
