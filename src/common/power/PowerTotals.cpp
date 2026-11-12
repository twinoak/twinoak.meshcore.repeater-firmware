#include "common/power/PowerTotals.h"

namespace twinoak {
namespace power {

void PowerTotals::anchor(uint32_t now_ms) {
  last_ms_ = now_ms;
  anchored_ = true;
}

bool PowerTotals::note(const PowerReadings& readings, uint32_t now_ms) {
  const int32_t elapsed = int32_t(now_ms - last_ms_);
  const bool was_anchored = anchored_;
  // Whatever this sweep turns out to be, the next one is measured from it: a gap is this
  // interval and not the next one as well.
  anchor(now_ms);

  if (!was_anchored) return false;  // nothing said when the window before this one began
  if (elapsed <= 0 || uint32_t(elapsed) > kMaxIntervalMs) {
    gaps_++;
    return false;
  }
  // A sweep counts for all three channels or for none: one channel's reading would otherwise
  // cover an interval the other two do not, and then the three totals of one answer could not be
  // read against each other.
  for (uint8_t i = 0; i < kChannels; i++) {
    if (!readings.ch[i].valid) {
      gaps_++;
      return false;
    }
  }

  const uint32_t ms = uint32_t(elapsed);

  // Panel and load count what is positive. A negative reading there is noise around zero - the
  // device's own input offset is tens of microvolts (CURRENT-SENSE.md section 3.5) - and adds
  // nothing; the one-sided bias that costs is at most one rounded LSB, which
  // tasks/phase-3.10/reports/01.md puts a number on.
  const ChannelReading& panel = readings.ch[0];
  if (panel.current_ma > 0) add(kPanel, panel.bus_mv, uint32_t(panel.current_ma), ms);

  // The battery's sign decides the direction, sweep by sweep: positive is into the pack.
  const ChannelReading& battery = readings.ch[1];
  if (battery.current_ma > 0) {
    add(kBatteryIn, battery.bus_mv, uint32_t(battery.current_ma), ms);
  } else if (battery.current_ma < 0) {
    add(kBatteryOut, battery.bus_mv, uint32_t(-int32_t(battery.current_ma)), ms);
  }

  const ChannelReading& load = readings.ch[2];
  if (load.current_ma > 0) add(kLoad, load.bus_mv, uint32_t(load.current_ma), ms);

  covered_ms_ += ms;
  return true;
}

void PowerTotals::add(uint8_t index, uint16_t bus_mv, uint32_t ma, uint32_t ms) {
  Flow& f = flow_[index];
  // Charge is current times time: milliamp * millisecond is a microcoulomb, exactly.
  f.charge_uc += uint64_t(ma) * ms;
  // Energy is the channel's own bus voltage times its current times time: millivolt * milliamp *
  // millisecond is a nanojoule. The thousandth of it that is not a whole microjoule is kept and
  // added to the next step, so a year of small steps is not a year of rounding down. The product
  // is at most 32760 * 8190 * 1267 = 3.4e11, far inside a uint64.
  const uint64_t nanojoules = uint64_t(bus_mv) * ma * ms + energy_rem_nj_[index];
  f.energy_uj += nanojoules / 1000u;
  energy_rem_nj_[index] = uint16_t(nanojoules % 1000u);
}

}  // namespace power
}  // namespace twinoak
