#include "roles/scanner/Sweep.h"

#include <math.h>
#include <stdio.h>
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

// ------------------------------------------------------------------ the histogram

uint32_t histScanUs(uint16_t samples) {
  return uint32_t(ceil(double(samples) * kHistSampleUs));
}

uint32_t histScanMs(uint16_t samples) { return (histScanUs(samples) + 999u) / 1000u; }

uint32_t histWaitUs(uint16_t samples) {
  return 2u * histScanUs(samples) + kHistWaitSlackMs * 1000u;
}

namespace {

// The bins of one result, added up: the proof that it is a histogram of the scan that was
// started for it. kHistBins counters of sixteen bits cannot overflow a 32-bit sum.
uint32_t histSum(const uint16_t* bins) {
  uint32_t sum = 0;
  for (int i = 0; i < kHistBins; i++) sum += uint32_t(bins[i]);
  return sum;
}

// One scan of `samples` samples at the frequency the radio is already on: the status register
// cleared, the scan started, its completion waited for, and the counters read once. False with
// `out` naming what went wrong; `out.sum` is the caller's to compute.
bool oneHistScan(HistRadio& radio, uint16_t samples, uint16_t* bins, HistResult& out) {
  // Whatever the register reads now is the frequency before's, and nothing in RadioLib's start
  // writes it (the header says which four things that start does). Cleared, and then not
  // relied on.
  radio.clearStatus();
  const uint32_t started_us = radio.nowUs();
  int st = radio.scanStart(samples);
  if (st != 0) {
    out.outcome = kHistStartFailed;
    out.code = st;
    return false;
  }
  // The floor, which is the half of this that holds whatever the register does: a scan of
  // `samples` samples cannot have finished before histScanUs() has passed since it was started,
  // so a *completed* read sooner is not this scan's and is not acted on - the loop goes on
  // polling. The counters are read once, after a completion at or past the floor, and never
  // while the scan runs.
  const uint32_t scan_us = histScanUs(samples);
  const uint32_t wait_us = histWaitUs(samples);
  bool completed = false;
  for (;;) {
    const uint32_t elapsed_us = radio.nowUs() - started_us;
    if (radio.scanStatus() == kHistStatusCompleted) {
      if (elapsed_us >= scan_us) {
        completed = true;
        break;
      }
      out.completed_early = true;  // the chip of 21 November, and what this floor exists for
    }
    if (elapsed_us >= wait_us) break;
    // One turn of the loop's own work: both ports drained, so a `stop` sent during a scan is
    // read here and executed between steps, as it always was.
    radio.poll();
  }
  if (!completed) {
    radio.scanAbort();
    out.outcome = kHistNeverCompleted;
    return false;
  }
  st = radio.scanResult(bins);
  if (st != 0) {
    out.outcome = kHistResultFailed;
    out.code = st;
    return false;
  }
  return true;
}

}  // namespace

bool measureHistPoint(HistRadio& radio, double f_mhz, uint16_t samples, uint16_t* bins,
                      HistResult& out) {
  out = HistResult();
  if (bins == nullptr || samples == 0) {
    // Neither can happen through `hist` - clampHistSamples() floors the count at
    // kHistSamplesMin and the caller's array is its own - and a sum of nothing is not a
    // histogram either way.
    out.outcome = kHistDoesNotAddUp;
    return false;
  }
  int st = radio.standby();
  if (st != 0) {
    out.outcome = kHistStandbyFailed;
    out.code = st;
    return false;
  }
  // The frequency is checked where the scope's is not: a retune the driver refused would leave
  // the chip on the frequency before, and a histogram of *that* adds up perfectly well.
  st = radio.setFrequency(f_mhz);
  if (st != 0) {
    out.outcome = kHistFrequencyFailed;
    out.code = st;
    return false;
  }
  for (uint8_t scan = 1; scan <= kHistScansMax; scan++) {
    out.scans = scan;
    if (!oneHistScan(radio, samples, bins, out)) return false;
    out.sum = histSum(bins);
    if (out.sum == uint32_t(samples)) {
      out.outcome = kHistMeasured;
      return true;
    }
  }
  // kHistScansMax scans and not one of them a histogram: the caller says so in one `err` line
  // naming the frequency, the sum and `n`, and ends the pass. No `hist` line is written.
  out.outcome = kHistDoesNotAddUp;
  return false;
}

void histErrorMessage(const HistResult& result, double f_mhz, uint16_t samples, char* out,
                      size_t size) {
  if (out == nullptr || size == 0) return;
  switch (result.outcome) {
    case kHistStandbyFailed:
      snprintf(out, size, "scan standby failed (%d) at %.3f", result.code, f_mhz);
      return;
    case kHistFrequencyFailed:
      snprintf(out, size, "scan retune failed (%d) at %.3f", result.code, f_mhz);
      return;
    case kHistStartFailed:
      snprintf(out, size, "scan start failed (%d) at %.3f", result.code, f_mhz);
      return;
    case kHistNeverCompleted:
      snprintf(out, size, "scan timeout at %.3f", f_mhz);
      return;
    case kHistResultFailed:
      snprintf(out, size, "scan result failed (%d) at %.3f", result.code, f_mhz);
      return;
    case kHistDoesNotAddUp:
      snprintf(out, size, "hist bins at %.3f add up to %lu and not %u after %u scans; no line "
                          "was sent",
               f_mhz, (unsigned long)result.sum, unsigned(samples), unsigned(result.scans));
      return;
    default:
      out[0] = '\0';
      return;
  }
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

const char* scopeModeName(ScopeMode mode) {
  return mode == kScopeModeStepped ? "stepped" : "";
}

bool scopeModeOf(const char* word, ScopeMode& out) {
  if (word == nullptr) return false;
  if (strcmp(word, "stepped") == 0) {
    out = kScopeModeStepped;
    return true;
  }
  return false;
}

const char* scopeRegulatorName(ScopeRegulator reg) {
  switch (reg) {
    case kScopeRegulatorLdo: return "ldo";
    case kScopeRegulatorDcDc: return "dcdc";
    default: return "";
  }
}

const char* scopeGainName(ScopeGain gain) {
  switch (gain) {
    case kScopeGainBoost: return "boost";
    case kScopeGainNoBoost: return "noboost";
    default: return "";
  }
}

bool scopeRegulatorOf(const char* word, ScopeRegulator& out) {
  if (word == nullptr) return false;
  if (strcmp(word, "ldo") == 0) {
    out = kScopeRegulatorLdo;
    return true;
  }
  if (strcmp(word, "dcdc") == 0) {
    out = kScopeRegulatorDcDc;
    return true;
  }
  return false;
}

bool scopeGainOf(const char* word, ScopeGain& out) {
  if (word == nullptr) return false;
  if (strcmp(word, "boost") == 0) {
    out = kScopeGainBoost;
    return true;
  }
  if (strcmp(word, "noboost") == 0) {
    out = kScopeGainNoBoost;
    return true;
  }
  return false;
}

ScopeRadioSetup scopeRadioSetup(ScopeRegulator reg, ScopeGain gain, bool build_ldo,
                                bool boost_setting) {
  ScopeRadioSetup out;
  // No word is the build's and the setting's, which is what every mode that is not a scope asks
  // for and what a `scope` with no word of that pair asks for: there is nothing to put back.
  out.ldo = reg == kScopeRegulatorBuild ? build_ldo : (reg == kScopeRegulatorLdo);
  out.boost = gain == kScopeGainAsLeft ? boost_setting : (gain == kScopeGainBoost);
  return out;
}

const char* scopeArmCallName(ScopeArmCall call) {
  switch (call) {
    case kScopeArmStandby: return "standby";
    case kScopeArmSetFrequency: return "setFrequency";
    case kScopeArmCalibrateImage: return "calibrateImage";
    case kScopeArmSetSweep: return "setSweep";
    case kScopeArmSweep: return "sweep";
    case kScopeArmReload: return "reload";
    case kScopeArmSettle: return "settle";
    case kScopeArmPoint: return "point";
    case kScopeArmRead: return "read";
    default: return "";
  }
}

float scopeSegmentCentreMhz(const ScopePlan& plan, const ScopeSegment& seg) {
  return float(seg.first_mhz) + float(plan.step_khz / 1000.0) * float(seg.points) / 2.0f;
}

void scopeCalibrationRange(const ScopePlan& plan, const ScopeSegment& seg, double& lo_mhz,
                           double& hi_mhz) {
  lo_mhz = seg.cal_lo_mhz;
  hi_mhz = seg.cal_hi_mhz;
  // A plan whose segments are already as wide as kScopeSegmentMhz allows - which is every plan a
  // `scope` with no width asked for makes - calibrates for the segment and nothing more. That is
  // what this did before phase 3.16's task 02 and what the arm's tests hold it to.
  if (plan.seg_mhz >= kScopeSegmentMhz) return;
  const double step_mhz = plan.step_khz / 1000.0;
  const double rbw_mhz = plan.rbw_khz / 1000.0;
  if (step_mhz <= 0.0 || plan.points == 0) return;
  // The points of kScopeSegmentMhz, counted exactly as planScope() counts them for a segment of
  // that width: the first point's centre to the last one's, plus half a bandwidth at each end.
  double fits = floor((kScopeSegmentMhz - rbw_mhz) / step_mhz) + 1.0;
  if (fits < 1.0) fits = 1.0;
  uint32_t points = fits > double(kScopeMaxPoints) ? uint32_t(kScopeMaxPoints) : uint32_t(fits);
  const uint32_t left = uint32_t(plan.points) - seg.first;  // never past the plan's last point
  if (points > left) points = left;
  if (points < seg.points) points = seg.points;  // and never narrower than the segment itself
  hi_mhz = seg.first_mhz + double(points - 1u) * step_mhz + rbw_mhz / 2.0;
}

bool settleScopeSegment(ScopeArmRadio& radio, const ScopePlan& plan, const ScopeSegment& seg,
                        ScopeArmResult& out) {
  if (plan.settle_us == 0) return true;
  // The one move the plugin itself makes with the receiver in continuous receive: the frequency
  // written, nothing calibrated (the header says why this and not a sweep of one point).
  const int st = radio.setFrequency(seg.first_mhz);
  if (st != 0) {
    out.failed = kScopeArmSettle;
    out.code = st;
    return false;
  }
  radio.settle(plan.settle_us);
  return true;
}

namespace {

// Standby, the frequency, the calibration, the chip's verdict: the part of an arm that a
// reload may be followed by once more.
bool calibrateScopeSegment(ScopeArmRadio& radio, const ScopePlan& plan, const ScopeSegment& seg,
                           ScopeCalibration& cal, ScopeArmResult& out) {
  int st = radio.standby();
  if (st != 0) {
    out.failed = kScopeArmStandby;
    out.code = st;
    return false;
  }
  // The chip's own account of the sweep that has just run - the last segment's, for segment 0 -
  // read here because standby is where the word answers and this is the first thing the arm does
  // in it, and cleared at once so that the calibration's verdict below is the calibration's own.
  // Only for a plan of more than one segment: one segment arms once and sweeps for ever, and is
  // not given a standby it does not have (the frame's `dev`).
  if (plan.segments > 1) {
    out.before_errors |= radio.deviceErrors();
    radio.clearDeviceErrors();
  }
  // The frequency first, calibrated for nothing: RadioLib's own calibration, made before the
  // move and for a range around the new centre, is the one the first board refused.
  st = radio.setFrequency(double(scopeSegmentCentreMhz(plan, seg)));
  if (st != 0) {
    out.failed = kScopeArmSetFrequency;
    out.code = st;
    return false;
  }
  // Then our range - what the segment measures, or the kScopeSegmentMhz this one begins a
  // calibration for - and the chip asked whether it took, because the driver cannot see this
  // command's status (the header says why). A segment that lies inside the range the chip is
  // already calibrated for is armed without either: a sweep of thirty-three segments must not
  // pay thirty-three calibrations.
  if (cal.valid && seg.cal_lo_mhz >= cal.lo_mhz - kScopeCalibrationSlackMhz &&
      seg.cal_hi_mhz <= cal.hi_mhz + kScopeCalibrationSlackMhz) {
    return true;  // every frequency this segment measures is inside what the chip is set for
  }
  double lo_mhz = 0.0;
  double hi_mhz = 0.0;
  scopeCalibrationRange(plan, seg, lo_mhz, hi_mhz);
  st = radio.calibrateImage(lo_mhz, hi_mhz);
  if (st != 0) {
    cal = ScopeCalibration();
    out.failed = kScopeArmCalibrateImage;
    out.code = st;
    return false;
  }
  out.device_errors = radio.deviceErrors();
  if ((out.device_errors & kScopeImageCalibrationError) != 0) {
    cal = ScopeCalibration();
    out.failed = kScopeArmCalibrateImage;
    out.code = kScopeArmCalibrationFailed;
    return false;
  }
  cal.valid = true;
  cal.lo_mhz = lo_mhz;
  cal.hi_mhz = hi_mhz;
  return true;
}

}  // namespace

bool armScopeSegment(ScopeArmRadio& radio, const ScopePlan& plan, const ScopeSegment& seg,
                     ScopeCalibration& cal, ScopeArmResult& out) {
  out = ScopeArmResult();
  if (!calibrateScopeSegment(radio, plan, seg, cal, out)) {
    if (out.failed != kScopeArmCalibrateImage) return false;
    // The chip refused the calibration: the one thing known to clear that is a reload, and the
    // same arm is then asked for once more. Refused again, the `err` says which call and that
    // the reload was made.
    out.reloaded = true;
    const int st = radio.reload();
    if (st != 0) {
      out.failed = kScopeArmReload;
      out.code = st;
      return false;
    }
    cal = ScopeCalibration();  // the reset took the calibration with it
    out.failed = kScopeArmNone;  // the refusal is history if the chip accepts the arm now
    out.code = 0;
    if (!calibrateScopeSegment(radio, plan, seg, cal, out)) return false;
  }
  const int st = radio.setSweep(seg.first_mhz, plan.step_khz / 1000.0, seg.points, plan.rbw_khz);
  if (st != 0) {
    out.failed = kScopeArmSetSweep;
    out.code = st;
    return false;
  }
  return settleScopeSegment(radio, plan, seg, out);
}

bool armScopeSegment(ScopeArmRadio& radio, const ScopePlan& plan, const ScopeSegment& seg,
                     ScopeArmResult& out) {
  ScopeCalibration cal;  // nothing is calibrated yet, so every segment calibrates for itself
  return armScopeSegment(radio, plan, seg, cal, out);
}

void ScopeSweepTime::reset() {
  running_ = 0;
  shortest_ = 0;
  longest_ = 0;
  sweeps_ = 0;
}

void ScopeSweepTime::segment(uint32_t us) { running_ += us; }

void ScopeSweepTime::sweepDone() {
  if (sweeps_ == 0 || running_ < shortest_) shortest_ = running_;
  if (sweeps_ == 0 || running_ > longest_) longest_ = running_;
  if (sweeps_ != 0xFFFFu) sweeps_++;
  running_ = 0;
}

bool sweepScopeSegment(ScopeArmRadio& radio, uint8_t* levels, ScopeSweepTime& time,
                       ScopeArmResult& out) {
  const uint32_t started_us = radio.nowUs();
  const int st = radio.sweep(levels);
  const uint32_t ended_us = radio.nowUs();
  if (st != 0) {
    out.failed = kScopeArmSweep;
    out.code = st;
    return false;  // a call that refused is not a measurement of one that worked
  }
  time.segment(ended_us - started_us);  // unsigned: a clock that wrapped is still the difference
  return true;
}

uint32_t scopeBase64Length(uint16_t bytes) { return 4u * ((uint32_t(bytes) + 2u) / 3u); }

uint32_t scopeFrameBytes(uint16_t points) {
  // The longest the envelope of docs/noisescope.md's `scope` line can be - every number at its
  // widest, the quotes and the newline - and the base64 of one byte a point. Not a guess: the
  // emitted line is held against this by test/test_scanner, for a frame of one point and for one
  // of kScopeMaxPoints.
  // 200 before phase 3.16's task 03, and 46 bytes more for `us`, `usx` and `dev`: `,"us":` and
  // ten digits, `,"usx":` and ten, and `,"dev":"ffff"`. The port's floor below is worked out from
  // this length, so a frame still takes at most one part in kScopeSinkDutyDivisor of its sink.
  constexpr uint32_t kEnvelope = 246;
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

uint32_t scopeSweepMs(uint16_t points, uint16_t segments, double rbw_khz, uint32_t settle_us) {
  if (rbw_khz <= 0.0) return 0;
  const double updates = double(points) + double(segments) * double(kScopeSegmentUpdates);
  // Every point's own update, kScopeSegmentUpdates an arm, and the settle each of those arms
  // stands on its segment's first point for - so that the `ack`'s `sweep` and the interval a
  // sink's floor is computed beside stay true when a settle was asked for.
  const double ms = updates * kScopeUpdateMsKhz / rbw_khz +
                    double(segments) * double(settle_us) / 1000.0;
  return uint32_t(ceil(ms));
}

uint8_t scopeLatencyUpdates(double rbw_khz) {
  // The plugin's own three numbers (SX126xSpectrumScanner::latencyFor, which is private there).
  if (rbw_khz < 17.0) return 2;
  if (rbw_khz < 150.0) return 3;
  return 4;
}

uint32_t scopeSteppedStandUs(double rbw_khz, uint32_t settle_us) {
  if (rbw_khz <= 0.0) return settle_us;
  // The latency's own time: the updates a reading needs to be of the frequency just written, at
  // kScopeUpdateMsKhz / RBW each, rounded up to the microsecond.
  const double updates = double(scopeLatencyUpdates(rbw_khz)) + double(kScopeLatencyJumpUpdates);
  const double us = ceil(updates * kScopeUpdateMsKhz * 1000.0 / rbw_khz);
  const uint32_t floor_us = us <= 0.0 ? 0u : uint32_t(us);
  return settle_us > floor_us ? settle_us : floor_us;
}

uint32_t scopeSteppedSweepMs(uint16_t points, double rbw_khz, uint32_t settle_us) {
  if (rbw_khz <= 0.0) return 0;
  const double us = double(points) * double(scopeSteppedStandUs(rbw_khz, settle_us));
  return uint32_t(ceil(us / 1000.0));
}

uint16_t scopeSteppedSlicePoints(double rbw_khz, uint32_t settle_us) {
  const uint32_t stand_us = scopeSteppedStandUs(rbw_khz, settle_us);
  if (stand_us == 0) return kScopePluginMaxPoints;  // nothing to wait for: the whole segment
  const uint32_t points = (kScopeSteppedSliceMs * 1000u) / stand_us;
  if (points < 1u) return 1u;  // a point is never cut in half, however long its stand is
  return points > kScopePluginMaxPoints ? kScopePluginMaxPoints : uint16_t(points);
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
  // The settle, resolved here and in no other place: no ninth number on the line is
  // kScopeSettleDefaultUs, and only a `0` typed there is none at all. Resolved before the bound,
  // because kScopeSettleAbsentUs is deliberately outside it.
  const uint32_t settle_us =
      request.settle_us == kScopeSettleAbsentUs ? kScopeSettleDefaultUs : request.settle_us;
  if (settle_us > kScopeSettleMaxUs) return false;
  if (request.seg_mhz < 0.0) return false;
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

  // A segment is at most kScopePluginMaxPoints points, and what it measures is at most the width
  // asked for - kScopeSegmentMhz when none was: its first point's centre to its last one's, plus
  // half a receiver bandwidth at each end. One point always fits, however wide a step is - it is
  // one frequency through one filter - which is what keeps this from ever asking for a segment of
  // none.
  const double step_mhz = step_khz / 1000.0;
  const double rbw_mhz = rbw_khz / 1000.0;
  // The width asked for, taken to a tenth of a MHz: from one step - a segment narrower than that
  // is not a segment - up to kScopeSegmentMhz, which is the widest a calibration covers.
  double seg_mhz = kScopeSegmentMhz;
  if (request.seg_mhz > 0.0) {
    seg_mhz = floor(request.seg_mhz * 10.0 + 0.5) / 10.0;
    if (seg_mhz > kScopeSegmentMhz) return false;
    if (seg_mhz < step_mhz - 1e-9) return false;
  }
  double per_segment = floor((seg_mhz - rbw_mhz) / step_mhz) + 1.0;
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
  out.seg_mhz = seg_mhz;
  out.settle_us = settle_us;
  // Carried and nothing more: neither word changes a point, a segment or a millisecond of the
  // plan - they are what the chip is begun as for this scope, and the `ack` names them.
  out.reg = request.reg;
  out.gain = request.gain;
  // And which way it is swept, which changes no point and no segment either - only the two
  // durations below, because a stepped point costs its stand and a plugin point one RSSI update.
  out.mode = request.mode;
  const uint32_t floor_ms = scopeIntervalFloorMs(out.points, request.sink_baud);
  out.interval_ms = request.interval_ms > floor_ms ? request.interval_ms : floor_ms;
  // The longest segment, which is the first ones when the points do not divide evenly.
  const uint16_t longest_segment = uint16_t((points + segments - 1u) / segments);
  if (out.mode == kScopeModeStepped) {
    // A stepped sweep holds the loop for one slice at a time and no longer (kScopeSteppedSliceMs),
    // and a slice is never more than the segment the cursor is in; the sweep is the points times
    // what a point stands for.
    const uint16_t slice = scopeSteppedSlicePoints(rbw_khz, out.settle_us);
    out.segment_ms = scopeSteppedSweepMs(slice < longest_segment ? slice : longest_segment,
                                         rbw_khz, out.settle_us);
    out.sweep_ms = scopeSteppedSweepMs(out.points, rbw_khz, out.settle_us);
  } else {
    out.segment_ms = scopeSegmentMs(longest_segment, rbw_khz);
    out.sweep_ms = scopeSweepMs(out.points, out.segments, rbw_khz, out.settle_us);
  }
  return true;
}

// ------------------------------------------------------------------ what the ack says

// One word of each pair at most. Both words of a pair contradict each other and a word twice
// says nothing new, so either is a line nobody meant: the words are not positional and a session
// has forty characters, which is where a typo of this kind comes from.
const char kScopeWordRefusal[] =
    "refused: ldo or dcdc, boost or noboost, stepped - one word of each at most,"
    " anywhere after the two ends";

const char kScopeRefusal[] =
    "refused: 150 <= f0 < f1 <= 960 MHz, points 16..2048, interval 0..60000 ms,"
    " seg_mhz one step..32, settle_us 0..20000 - or the radio refused the mode";

void scopeAckMessage(const ScopePlan& plan, char* out, size_t bytes) {
  if (out == nullptr || bytes == 0) return;
  // What was taken, which is not always what was asked: the points, the step they make, the
  // bandwidth, how many segments the span was cut into, how much of it a reading covers and the
  // interval the sink allowed.
  const int written = snprintf(out, bytes,
                               "n=%u step=%.4fkHz rbw=%.1fkHz seg=%u cov=%.3f interval=%lums"
                               " sweep=%lums",
                               unsigned(plan.points), plan.step_khz, plan.rbw_khz,
                               unsigned(plan.segments), plan.cov,
                               (unsigned long)plan.interval_ms, (unsigned long)plan.sweep_ms);
  // Then the width of phase 3.16's task 02, named only when it is not kScopeSegmentMhz - so that
  // a `scope` that asked for no width says nothing about one.
  if (written <= 0 || size_t(written) >= bytes) return;
  size_t at = size_t(written);
  if (plan.seg_mhz != kScopeSegmentMhz) {
    const int more = snprintf(out + at, bytes - at, " segw=%.1fMHz", plan.seg_mhz);
    if (more <= 0 || size_t(more) >= bytes - at) return;
    at += size_t(more);
  }
  // And the settle, on **every** `scope`: since phase 3.17's task 01 every one of them stands on
  // its segments' first points unless the line said `0`, and a capture that does not say which
  // cannot be read back (tasks/phase-3.17/01-a-settle-by-default.md, deliverable 4).
  {
    const int more = snprintf(out + at, bytes - at, " settle=%luus",
                              (unsigned long)plan.settle_us);
    if (more <= 0 || size_t(more) >= bytes - at) return;
    at += size_t(more);
  }
  // And the mode, named only when the line carried `stepped`: a `scope` swept the plugin's way
  // says nothing about a mode, so its `ack` is the one phase 3.17's task 01 left to the byte.
  if (plan.mode != kScopeModePlugin) {
    const int more = snprintf(out + at, bytes - at, " mode=%s", scopeModeName(plan.mode));
    if (more <= 0 || size_t(more) >= bytes - at) return;
    at += size_t(more);
  }
  // And the two words of phase 3.16's task 03, each named only when its own word was on the
  // line: a `scope` with none of the four gets the line above and nothing after it.
  if (plan.reg != kScopeRegulatorBuild) {
    const int more = snprintf(out + at, bytes - at, " reg=%s", scopeRegulatorName(plan.reg));
    if (more <= 0 || size_t(more) >= bytes - at) return;
    at += size_t(more);
  }
  if (plan.gain != kScopeGainAsLeft) {
    snprintf(out + at, bytes - at, " gain=%s", scopeGainName(plan.gain));
  }
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

// ----------------------------------------------------------- a sweep the host steps, in slices

void SteppedScope::restart() {
  segment_ = 0;
  point_ = 0;
  armed_ = 0xFFFF;
  mid_ = false;
}

bool SteppedScope::slice(ScopeArmRadio& radio, const ScopePlan& plan, ScopeCalibration& cal,
                         ScopeAccumulator& acc, ScopeSweepTime& time, ScopeArmResult& out,
                         bool& whole) {
  whole = false;
  out = ScopeArmResult();
  ScopeSegment seg;
  if (!scopeSegment(plan, segment_, seg)) return false;  // kScopeArmNone: not a segment at all
  // The segment the cursor has just reached, armed exactly as a plugin sweep arms it - which is
  // also what puts the receiver in continuous receive at the plan's bandwidth and stands the
  // plan's settle on the first point (armScopeSegment, and the header above).
  if (armed_ != segment_) {
    if (!armScopeSegment(radio, plan, seg, cal, out)) return false;
    armed_ = segment_;
    point_ = 0;
  }
  const uint32_t stand_us = scopeSteppedStandUs(plan.rbw_khz, plan.settle_us);
  const uint16_t slice_points = scopeSteppedSlicePoints(plan.rbw_khz, plan.settle_us);
  const double step_mhz = plan.step_khz / 1000.0;
  // The points of this slice, timed together: `us` and `usx` of a stepped frame are the whole
  // sweep's stepping, added slice by slice, and no arm is in them - as no arm is in a plugin
  // frame's (ScopeSweepTime, above).
  const uint32_t started_us = radio.nowUs();
  for (uint16_t done = 0; done < slice_points && point_ < seg.points; done++) {
    // The frequency the chip's own way, the stand, and one reading of it. The stand is made with
    // the seam's delay and never by reading the clock: micros() steps by 977 us on this board.
    const int st = radio.setFrequency(seg.first_mhz + double(point_) * step_mhz);
    if (st != 0) {
      out.failed = kScopeArmPoint;
      out.code = st;
      return false;
    }
    radio.settle(stand_us);
    uint8_t level = kScopeNoReading;
    const int read = radio.readLevel(level);
    if (read != 0) {
      out.failed = kScopeArmRead;
      out.code = read;
      return false;
    }
    acc.add(uint16_t(seg.first + point_), &level, 1);
    mid_ = true;
    point_++;
  }
  time.segment(radio.nowUs() - started_us);  // unsigned: a clock that wrapped is the difference
  if (point_ < seg.points) return true;  // the rest of this segment in the next pass of loop()
  segment_++;
  point_ = 0;
  if (segment_ < plan.segments) return true;  // and the rest of the span after that
  // The last point of the last segment: the sweep is whole, and the next one begins at segment 0.
  segment_ = 0;
  mid_ = false;
  // A plan of more than one segment comes back to segment 0 from the other end of the span, so it
  // is armed again - as a plugin sweep arms it again. A plan of one segment stays armed: every
  // point of it stands for itself, so there is nothing a re-arm would add.
  if (plan.segments > 1) armed_ = 0xFFFF;
  whole = true;
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
