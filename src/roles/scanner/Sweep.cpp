#include "roles/scanner/Sweep.h"

namespace twinoak {
namespace scanner {

// NoiseScope's FSK_RBW table, which is the SX1262's own set of receiver filter bandwidths
// (its datasheet, "SetModulationParams": RX_BW_4800 to RX_BW_467000).
const double kFskRbwKhz[] = {4.8,   5.8,   7.3,   9.7,   11.7,  14.6,  19.5,
                             23.4,  29.3,  39.0,  46.9,  58.6,  78.2,  93.8,
                             117.3, 156.2, 187.2, 234.3, 312.0, 373.6, 467.0};
const size_t kFskRbwCount = sizeof(kFskRbwKhz) / sizeof(kFskRbwKhz[0]);

double pickRbwKhz(double step_khz) {
  for (size_t i = 0; i < kFskRbwCount; i++) {
    if (kFskRbwKhz[i] >= step_khz * 1.1) return kFskRbwKhz[i];
  }
  return kFskRbwKhz[kFskRbwCount - 1];
}

bool frequencyOk(double mhz) { return mhz >= kRadioLowMhz && mhz <= kRadioHighMhz; }

bool planSweep(double f0, double f1, double step_khz, SweepPlan& out) {
  if (f1 <= f0 || !frequencyOk(f0) || !frequencyOk(f1)) return false;
  if (step_khz < 1.0 || step_khz > 1000.0) return false;
  // The count, as NoiseScope computed it: the ends included, truncated towards zero, so a range
  // that is not a whole number of steps stops at the last point inside it.
  const double span = (f1 - f0) * 1000.0 / step_khz;
  if (span < 1.0 || span > double(kMaxSweepPoints)) return false;  // +1 below
  const uint32_t points = uint32_t(span) + 1;
  if (points < 2 || points > kMaxSweepPoints) return false;
  out.f0 = f0;
  out.f1 = f1;
  out.step_khz = step_khz;
  out.rbw_khz = pickRbwKhz(step_khz);
  out.points = uint16_t(points);
  return true;
}

double pointFrequencyMhz(const SweepPlan& plan, uint16_t index) {
  return plan.f0 + (double(index) * plan.step_khz) / 1000.0;
}

bool sweepDwellOk(uint32_t dwell_ms) {
  return dwell_ms >= kSweepDwellMinMs && dwell_ms <= kSweepDwellMaxMs;
}

uint16_t clampHistSamples(uint32_t samples) {
  if (samples < kHistSamplesMin) return kHistSamplesMin;
  if (samples > 0xFFFFu) return 0xFFFFu;
  return uint16_t(samples);
}

uint32_t clampDwellSeconds(uint32_t secs) {
  if (secs < 1) return 1;
  if (secs > kDwellSecondsMax) return kDwellSecondsMax;
  return secs;
}

uint32_t clampDwellWindowMs(uint32_t win_ms) {
  if (win_ms < kDwellWindowMinMs) return kDwellWindowMinMs;
  if (win_ms > kDwellWindowMaxMs) return kDwellWindowMaxMs;
  return win_ms;
}

uint16_t clampCadCount(uint32_t count) {
  if (count < 1) return 1;
  if (count > kCadCountMax) return kCadCountMax;
  return uint16_t(count);
}

bool homeSettingsOk(double f_mhz, uint8_t sf, uint8_t cr) {
  // The SX1262's LoRa spreading factors are 5 to 12 and its coding rates 5 to 8 (4/5 to 4/8).
  return frequencyOk(f_mhz) && sf >= 5 && sf <= 12 && cr >= 5 && cr <= 8;
}

// ------------------------------------------------------------------ the window

void RssiWindow::add(double dbm) {
  n_++;
  sum_ += dbm;
  if (dbm > max_) max_ = dbm;
  if (dbm < min_) min_ = dbm;
  if (dbm > th_) over_++;
}

bool RssiWindow::stats(WindowStats& out) const {
  if (n_ == 0) return false;
  out.n = n_;
  out.avg = sum_ / double(n_);
  out.max = max_;
  out.min = min_;
  out.dc = double(over_) / double(n_);
  out.th = th_;
  return true;
}

void RssiWindow::reset() {
  // The threshold the next window counts against comes from this one, and from nothing when
  // this one was empty.
  if (n_ > 0) th_ = min_ + kBurstThresholdDeltaDb;
  n_ = 0;
  over_ = 0;
  sum_ = 0.0;
  max_ = kNoReadingDbm;
  min_ = -kNoReadingDbm;
}

void RssiWindow::restart() {
  n_ = 0;
  over_ = 0;
  sum_ = 0.0;
  max_ = kNoReadingDbm;
  min_ = -kNoReadingDbm;
  th_ = kFirstThresholdDbm;
}

}  // namespace scanner
}  // namespace twinoak
