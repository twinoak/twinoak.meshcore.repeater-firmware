#include "roles/scanner/Sweep.h"

#include <math.h>
#include <string.h>

namespace twinoak {
namespace scanner {

// NoiseScope's FSK_RBW table, which is the SX1262's own set of receiver filter bandwidths
// (its datasheet, "SetModulationParams": RX_BW_4800 to RX_BW_467000).
const double kFskRbwKhz[] = {4.8,   5.8,   7.3,   9.7,   11.7,  14.6,  19.5,
                             23.4,  29.3,  39.0,  46.9,  58.6,  78.2,  93.8,
                             117.3, 156.2, 187.2, 234.3, 312.0, 373.6, 467.0};
const size_t kFskRbwCount = sizeof(kFskRbwKhz) / sizeof(kFskRbwKhz[0]);

double narrowestRbwKhz(double khz) {
  for (size_t i = 0; i < kFskRbwCount; i++) {
    if (kFskRbwKhz[i] >= khz) return kFskRbwKhz[i];
  }
  return kFskRbwKhz[kFskRbwCount - 1];
}

double pickRbwKhz(double step_khz) { return narrowestRbwKhz(step_khz * 1.1); }

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

// ------------------------------------------------------------------ the scope

const char kScopeRom[] = "SX1261 V2D 2D02";

bool scopeRomSupported(const char* rom) {
  if (rom == nullptr) return false;
  for (size_t i = 0; i < kScopeRomChars; i++) {
    if (rom[i] != kScopeRom[i]) return false;
  }
  return true;
}

const char* scopeAggName(ScopeAgg agg) {
  switch (agg) {
    case kScopeAggMean: return "mean";
    case kScopeAggLast: return "last";
    default: return "max";
  }
}

bool scopeAggOf(const char* word, ScopeAgg& out) {
  if (word == nullptr) return false;
  if (strcmp(word, "max") == 0) {
    out = kScopeAggMax;
    return true;
  }
  if (strcmp(word, "mean") == 0) {
    out = kScopeAggMean;
    return true;
  }
  if (strcmp(word, "last") == 0) {
    out = kScopeAggLast;
    return true;
  }
  return false;
}

uint32_t scopeBase64Length(uint16_t bytes) { return 4u * ((uint32_t(bytes) + 2u) / 3u); }

uint32_t scopeFrameBytes(uint16_t points) {
  // The longest the envelope of docs/noisescope.md's `scope` line can be - every number at its
  // widest, the quotes and the newline - and the base64 of one byte a point. Not a guess: the
  // emitted line is held against this by test/test_scanner, for a frame of one point and for one
  // of kScopeMaxPoints.
  constexpr uint32_t kEnvelope = 200;
  return kEnvelope + scopeBase64Length(points);
}

uint32_t scopeIntervalFloorMs(uint16_t points, uint32_t baud) {
  if (baud == 0) return 0;
  // 8N1: a byte is ten bits on the wire. Rounded up, and then the frame may have only one part
  // in kScopeSinkDutyDivisor of the port's time.
  const uint32_t bits = scopeFrameBytes(points) * 10u;
  const uint32_t ms = (bits * 1000u + baud - 1u) / baud;
  return ms * kScopeSinkDutyDivisor;
}

uint32_t scopeSegmentMs(uint16_t points, double rbw_khz) {
  if (rbw_khz <= 0.0) return 0;
  const double updates = double(points) + double(kScopeSegmentUpdates);
  return uint32_t(ceil(updates * kScopeUpdateMsKhz / rbw_khz));
}

uint32_t scopeSweepMs(uint16_t points, uint16_t segments, double rbw_khz) {
  if (rbw_khz <= 0.0) return 0;
  const double updates = double(points) + double(segments) * double(kScopeSegmentUpdates);
  return uint32_t(ceil(updates * kScopeUpdateMsKhz / rbw_khz));
}

uint16_t scopeSweepsPerSecondX10(uint32_t sweeps, uint32_t elapsed_ms) {
  if (elapsed_ms == 0) return 0;
  const uint64_t x10 = (uint64_t(sweeps) * 10000u) / elapsed_ms;
  return x10 > 0xFFFFu ? 0xFFFFu : uint16_t(x10);
}

bool planScope(const ScopeRequest& request, ScopePlan& out) {
  if (request.f1 <= request.f0 || !frequencyOk(request.f0) || !frequencyOk(request.f1)) {
    return false;
  }
  if (request.rbw_khz < 0.0) return false;
  if (request.interval_ms > kScopeIntervalMaxMs) return false;
  if (request.points != 0 &&
      (request.points < kScopeMinPoints || request.points > kScopeMaxPoints)) {
    return false;
  }

  const double span_khz = (request.f1 - request.f0) * 1000.0;

  // The two defaults, and the one place their order is decided: a bandwidth asked for is a
  // resolution asked for, so the point count follows it - as many points as that bandwidth
  // covers without gaps. With neither given the default is kScopeDefaultPoints, one plugin call,
  // and the bandwidth follows the step instead (tasks/phase-3.1/reports/01.md).
  uint32_t points = request.points;
  double rbw_khz = request.rbw_khz > 0.0 ? narrowestRbwKhz(request.rbw_khz) : 0.0;
  if (points == 0) {
    if (rbw_khz > 0.0) {
      const double needed = ceil(span_khz / rbw_khz);
      points = needed < double(kScopeMinPoints) ? kScopeMinPoints
               : (needed > double(kScopeMaxPoints) ? kScopeMaxPoints : uint32_t(needed));
    } else {
      points = kScopeDefaultPoints;
    }
  }

  const double step_khz = span_khz / double(points);
  if (step_khz < kScopeMinStepKhz) return false;
  if (rbw_khz <= 0.0) rbw_khz = pickRbwKhz(step_khz);

  // A segment is at most kScopePluginMaxPoints points, and what it measures is at most
  // kScopeSegmentMhz wide: its first point's centre to its last one's, plus half a receiver
  // bandwidth at each end. One point always fits, however wide a step is - it is one frequency
  // through one filter - which is what keeps this from ever asking for a segment of none.
  const double step_mhz = step_khz / 1000.0;
  const double rbw_mhz = rbw_khz / 1000.0;
  double per_segment = floor((kScopeSegmentMhz - rbw_mhz) / step_mhz) + 1.0;
  if (per_segment > double(kScopePluginMaxPoints)) per_segment = double(kScopePluginMaxPoints);
  if (per_segment < 1.0) per_segment = 1.0;
  const uint32_t max_points = uint32_t(per_segment);
  const uint32_t segments = (points + max_points - 1u) / max_points;

  out.f0 = request.f0;
  out.f1 = request.f1;
  out.step_khz = step_khz;
  out.first_mhz = request.f0 + step_mhz / 2.0;
  out.rbw_khz = rbw_khz;
  out.cov = rbw_khz / step_khz;
  out.points = uint16_t(points);
  out.segments = uint16_t(segments);
  out.agg = request.agg;
  const uint32_t floor_ms = scopeIntervalFloorMs(out.points, request.sink_baud);
  out.interval_ms = request.interval_ms > floor_ms ? request.interval_ms : floor_ms;
  // The longest segment, which is the first ones when the points do not divide evenly.
  out.segment_ms = scopeSegmentMs(uint16_t((points + segments - 1u) / segments), rbw_khz);
  out.sweep_ms = scopeSweepMs(out.points, out.segments, rbw_khz);
  return true;
}

bool scopeSegment(const ScopePlan& plan, uint16_t index, ScopeSegment& out) {
  if (plan.points == 0 || plan.segments == 0 || index >= plan.segments) return false;
  // The points spread as evenly as whole points can: the first `remainder` segments carry one
  // more than the rest, so no segment is longer than the bound planScope held them to.
  const uint16_t base = uint16_t(plan.points / plan.segments);
  const uint16_t extra = uint16_t(plan.points % plan.segments);
  const uint16_t count = uint16_t(base + (index < extra ? 1u : 0u));
  const uint16_t first = uint16_t(uint32_t(index) * base + (index < extra ? index : extra));
  const double step_mhz = plan.step_khz / 1000.0;
  const double half_rbw_mhz = plan.rbw_khz / 2000.0;
  out.first = first;
  out.points = count;
  out.first_mhz = plan.f0 + (double(first) + 0.5) * step_mhz;
  // Every frequency this segment is tuned to and no more: its first point's centre to its
  // last's, each seen through a filter half a bandwidth wide on either side.
  out.cal_lo_mhz = out.first_mhz - half_rbw_mhz;
  out.cal_hi_mhz = out.first_mhz + double(count - 1) * step_mhz + half_rbw_mhz;
  return true;
}

// ------------------------------------------------------------------ the frame

void ScopeAccumulator::reset(uint16_t points) {
  points_ = points > kScopeMaxPoints ? kScopeMaxPoints : points;
  sweeps_ = 0;
  memset(max_, kScopeNoReading, sizeof(max_));
  memset(last_, kScopeNoReading, sizeof(last_));
  memset(count_, 0, sizeof(count_));
  for (uint16_t i = 0; i < points_; i++) power_[i] = 0.0f;
}

void ScopeAccumulator::add(uint16_t first, const uint8_t* levels, uint16_t count) {
  if (levels == nullptr) return;
  for (uint16_t i = 0; i < count; i++) {
    const uint16_t at = uint16_t(first + i);
    if (at >= points_) break;
    const uint8_t level = levels[i];
    last_[at] = level;
    if (level == kScopeNoReading) continue;  // the plugin read nothing for this point
    // The strongest level is the *lowest* number: a level is -2 x dBm.
    if (level < max_[at]) max_[at] = level;
    // 10^(dBm/10) with dBm = -level/2, which is 10^(-level/20).
    power_[at] += powf(10.0f, -float(level) / 20.0f);
    count_[at]++;
  }
}

void ScopeAccumulator::take(ScopeAgg agg, uint8_t* out) const {
  if (out == nullptr) return;
  for (uint16_t i = 0; i < points_; i++) {
    if (agg == kScopeAggLast) {
      out[i] = last_[i];
      continue;
    }
    if (agg == kScopeAggMax) {
      out[i] = max_[i];
      continue;
    }
    if (count_[i] == 0) {
      out[i] = kScopeNoReading;  // nothing to take a mean of
      continue;
    }
    const float mean = power_[i] / float(count_[i]);
    const float level = -20.0f * log10f(mean) + 0.5f;
    out[i] = level < 0.0f ? 0u
             : (level > float(kScopeNoReading - 1) ? uint8_t(kScopeNoReading - 1)
                                                   : uint8_t(level));
  }
}

}  // namespace scanner
}  // namespace twinoak
