#include "common/power/BatteryWatch.h"

namespace twinoak {
namespace power {

bool thresholdsAcceptable(uint16_t low_mv, uint16_t crit_mv) {
  if (crit_mv < kThresholdFloorMv || low_mv > kThresholdCeilingMv) return false;
  // Low must sit above critical by more than the hysteresis, or leaving `critical` would enter
  // `low` and leaving `low` would enter `critical` at the same voltage.
  if (low_mv <= crit_mv) return false;
  return uint16_t(low_mv - crit_mv) > kHysteresisMv;
}

bool BatteryWatch::setThresholds(uint16_t low_mv, uint16_t crit_mv) {
  if (!thresholdsAcceptable(low_mv, crit_mv)) return false;
  low_mv_ = low_mv;
  crit_mv_ = crit_mv;
  // The level is left where it is and whatever was pending starts again: the next poll decides
  // what the new thresholds mean, after the same minimum time as any other change.
  pending_ = kBatteryNoChange;
  return true;
}

uint8_t BatteryWatch::levelOf(uint16_t mv) const {
  switch (level_) {
    case kBatteryCritical:
      if (mv >= uint16_t(low_mv_ + kHysteresisMv)) return kBatteryNormal;
      if (mv >= uint16_t(crit_mv_ + kHysteresisMv)) return kBatteryLow;
      return kBatteryCritical;
    case kBatteryLow:
      if (mv < crit_mv_) return kBatteryCritical;
      if (mv >= uint16_t(low_mv_ + kHysteresisMv)) return kBatteryNormal;
      return kBatteryLow;
    case kBatteryNormal:
    default:
      if (mv < crit_mv_) return kBatteryCritical;
      if (mv < low_mv_) return kBatteryLow;
      return kBatteryNormal;
  }
}

uint8_t BatteryWatch::poll(bool valid, uint16_t batt_mv, uint32_t now_ms) {
  if (!valid) {
    // No reading is not a reading of zero. The level stands and the clock on a pending change
    // starts again when the channel comes back.
    pending_ = kBatteryNoChange;
    return kBatteryNoChange;
  }
  last_mv_ = batt_mv;

  const uint8_t want = levelOf(batt_mv);
  if (want == level_) {
    pending_ = kBatteryNoChange;  // the dip, or the recovery, did not last
    return kBatteryNoChange;
  }
  if (want != pending_) {
    pending_ = want;
    pending_since_ms_ = now_ms;
    return kBatteryNoChange;
  }
  if (int32_t(now_ms - pending_since_ms_) < int32_t(kMinTimeBelowMs)) return kBatteryNoChange;

  level_ = want;
  pending_ = kBatteryNoChange;
  return level_;
}

}  // namespace power
}  // namespace twinoak
