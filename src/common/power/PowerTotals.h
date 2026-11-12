// What has flowed since the firmware started: charge and energy per INA3221 channel, the
// battery's by direction, and the time those totals really cover
// (tasks/phase-3.10/01-what-flowed.md; docs/ARCHITECTURE.md section 6).
//
// Why this exists. A reading of this board's power is the average of one of the device's
// averaging windows, handed out whenever somebody asks. The only moment anybody asks is the
// moment its own Walter is awake to ask - modem up, 110 to 170 mA - so a week of hourly
// telemetry says the unit draws 110 mA when it draws a fraction of that over the hour
// (tasks/phase-3.10/README.md, from meshmaster.dk's own graphs). The radio never sleeps and sees
// every window, so it adds them up: a reader takes the difference between two answers and has
// what really flowed in between, whenever it asked.
//
// **Not a fuel gauge.** The battery channel resolves 2 mA (CURRENT-SENSE.md section 3.1 in the
// design repository: *too coarse for coulomb counting*). These totals are good for a day's energy
// and an hour's average current; state of charge stays a matter of voltage (BatteryWatch.h).
//
// **A gap is a gap.** A sweep that did not read all three channels, the back-off after
// dropDevice, and a sweep that came later than kMaxIntervalMs add nothing and do not count
// towards the time covered. A sweep counts for all three channels or for none, so the three
// channels of one answer always cover the same interval and a reader may subtract them from each
// other.
//
// **Zero at start, and only then.** Kept in RAM; nothing here is written to flash, and nothing
// here resets a total - not a dropped device, not a reconfiguration, not a wrap of millis().
//
// Host-portable: no Arduino, no MeshCore, no heap, no floating point. Every step is integer
// arithmetic on the types below, which is what lets test/test_power hold a simulated hour, a
// burst, a gap and a year at full scale against hand-computed numbers.

#pragma once

#include <stdint.h>

#include "common/power/Ina3221.h"

namespace twinoak {
namespace power {

// ----------------------------------------------------------------------------- the numbers

// How late a sweep may be and still be added up: one and a half averaging windows. Normal is one
// window plus however long the ready flag waited to be noticed (PowerReader.h, kReadyPollMs), so
// about 845 to 870 ms; two windows - 1690 ms - means the device finished one that was never read,
// and then the average in its registers covers half of the interval and not the whole of it.
// Half a window of slack lies between the two, and that is the bound.
constexpr uint32_t kMaxIntervalMs = kConversionCycleMs + kConversionCycleMs / 2u;  // 1267

// 1 mAh is 3.6e6 mA*ms and 1 mWh is 3.6e6 mW*ms, which are the units of the two accumulators
// below: the same divisor, because both are "per 3600 seconds".
constexpr uint64_t kMicroPerMilliHour = 3600000u;

// ----------------------------------------------------------------------------- one direction

// How much has flowed one way through one channel since the firmware started.
//
// The units are the product of what the device reports and the milliseconds the firmware counts
// in, so that every step is exact and nothing is scaled twice:
//
//   charge   milliamp * millisecond = **microcoulomb**
//   energy   millivolt * milliamp * millisecond = nanojoule, carried into **microjoule** with
//            the remainder kept (PowerTotals::add), so that a step below a microjoule is saved
//            rather than lost
//
// A year at this board's full scale is 2.6e14 uC and 8.5e15 uJ (test/test_power holds the
// arithmetic), so a uint64 has thousands of years of room: a total never wraps, and 02 may carry
// it in whatever unit the wire wants.
struct Flow {
  uint64_t charge_uc = 0;
  uint64_t energy_uj = 0;
};

// The two units a reader thinks in, rounded down. Both divide by the same number.
inline uint64_t milliAmpHours(const Flow& f) { return f.charge_uc / kMicroPerMilliHour; }
inline uint64_t milliWattHours(const Flow& f) { return f.energy_uj / kMicroPerMilliHour; }

// ----------------------------------------------------------------------------- the totals

class PowerTotals {
 public:
  // The four directions, in the order 02 puts them on the wire.
  enum FlowIndex : uint8_t {
    kPanel = 0,       // channel 1, what the panel gave
    kBatteryIn = 1,   // channel 2 positive: charging
    kBatteryOut = 2,  // channel 2 negative: draining
    kLoad = 3,        // channel 3, what the two radios drew
    kFlows = 4,
  };

  // The next sweep covers the time since this call and nothing before it: the reader calls it
  // when it has just configured the device, which is also when the device starts its first
  // averaging window, so the first window is counted and the back-off before it is not.
  //
  // **It never clears a total.** The totals are zero at start and only then.
  void anchor(uint32_t now_ms);

  // One completed sweep, with the millisecond the device said the window had finished. Returns
  // true when it was added up and false when it was a gap - a channel that did not read, or an
  // interval of zero, backwards or longer than kMaxIntervalMs. Either way the next sweep is
  // measured from this one.
  bool note(const PowerReadings& readings, uint32_t now_ms);

  // --- what 02, a report and the bench read --------------------------------------------

  const Flow& flow(uint8_t index) const { return flow_[index < kFlows ? index : 0]; }
  const Flow& panel() const { return flow_[kPanel]; }
  const Flow& batteryIn() const { return flow_[kBatteryIn]; }
  const Flow& batteryOut() const { return flow_[kBatteryOut]; }
  const Flow& load() const { return flow_[kLoad]; }

  // The time the totals really cover: every interval that was added up, and no other.
  uint64_t millisCovered() const { return covered_ms_; }
  uint32_t secondsCovered() const { return uint32_t(covered_ms_ / 1000u); }

  // Sweeps that were a gap, since boot - what makes a thin "seconds covered" readable.
  uint32_t gaps() const { return gaps_; }

 private:
  // Adds one direction's share of one interval. `ma` is already the magnitude.
  void add(uint8_t index, uint16_t bus_mv, uint32_t ma, uint32_t ms);

  Flow flow_[kFlows];
  // What is left of the last energy step below one microjoule, per direction: 0..999.
  uint16_t energy_rem_nj_[kFlows] = {0, 0, 0, 0};

  uint64_t covered_ms_ = 0;
  uint32_t last_ms_ = 0;
  bool anchored_ = false;
  uint32_t gaps_ = 0;
};

}  // namespace power
}  // namespace twinoak
