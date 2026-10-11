#include "common/power/Environment.h"

namespace twinoak {
namespace power {
namespace {

// A value that is not a number, written without <math.h> and without comparing two floats: a
// quiet NaN is the only value that is neither >= 0 nor < 0.
bool notANumber(float v) { return !(v >= 0.0f) && !(v < 0.0f); }

// The part's operating ranges with room either side (BME280 datasheet table 1). Outside them the
// reading is not the air in the box: it is a part that is resetting, or one whose calibration
// registers were read as zeroes.
const float kTempMinC = -60.0f;
const float kTempMaxC = 100.0f;
const float kRhMinPct = 0.0f;
const float kRhMaxPct = 100.0f;
const float kPressureMinPa = 20000.0f;
const float kPressureMaxPa = 130000.0f;

}  // namespace

EnvReadings envReadingsFrom(float temp_c, float rh_pct, float pressure_pa) {
  EnvReadings out;
  if (!notANumber(temp_c) && temp_c >= kTempMinC && temp_c <= kTempMaxC) {
    // Rounded to the nearest centidegree, and symmetric in the sign: the enclosure of a Danish
    // mast is below freezing for months and -3.50 C must not become -3.49 (the fixture's own
    // ENV_GET case is -350).
    const float scaled = temp_c * 100.0f;
    out.temp_centi = int16_t(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
    out.valid = uint8_t(out.valid | kEnvTemperatureBit);
  }
  if (!notANumber(rh_pct) && rh_pct >= kRhMinPct && rh_pct <= kRhMaxPct) {
    out.rh_centi = uint16_t(rh_pct * 100.0f + 0.5f);
    out.valid = uint8_t(out.valid | kEnvHumidityBit);
  }
  if (!notANumber(pressure_pa) && pressure_pa >= kPressureMinPa && pressure_pa <= kPressureMaxPa) {
    out.pressure_pa = uint32_t(pressure_pa + 0.5f);
    out.valid = uint8_t(out.valid | kEnvPressureBit);
  }
  return out;
}

void EnvReader::begin(uint32_t now_ms) {
  present_ = sensor_.begin();
  if (!present_) {
    drop(now_ms);
    return;
  }
  last_sample_ms_ = now_ms;
}

void EnvReader::loop(uint32_t now_ms) {
  if (!present_) {
    if (int32_t(now_ms - retry_at_ms_) < 0) return;
    present_ = sensor_.begin();
    if (!present_) {
      drop(now_ms);
      return;
    }
    last_sample_ms_ = now_ms;
    return;  // the pass that found the part does not also sample it
  }

  const bool due =
      !have_sampled_ || int32_t(now_ms - last_sample_ms_) >= int32_t(kEnvSampleIntervalMs);
  if (!due) return;

  float temp_c = 0.0f;
  float rh_pct = 0.0f;
  float pressure_pa = 0.0f;
  const bool ok = sensor_.read(temp_c, rh_pct, pressure_pa);
  const bool too_slow = sensor_.lastTransactionMicros() > kEnvTransactionBudgetUs;
  if (too_slow) overruns_++;
  if (!ok || too_slow) {
    // A missing reading, never a stalled loop: every bit of `valid` goes clear and the part is
    // left alone until the back-off is over.
    if (!ok) failures_++;
    readings_ = EnvReadings();
    drop(now_ms);
    return;
  }

  readings_ = envReadingsFrom(temp_c, rh_pct, pressure_pa);
  have_sampled_ = true;
  last_sample_ms_ = now_ms;
  samples_++;
}

void EnvReader::drop(uint32_t now_ms) {
  present_ = false;
  readings_.valid = 0;
  retry_at_ms_ = now_ms + kEnvRetryMs;
}

}  // namespace power
}  // namespace twinoak
