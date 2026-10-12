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
