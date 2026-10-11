// The battery thresholds: *low* and *critical* on channel 2's bus voltage, with hysteresis and a
// minimum time below, so that a transmit burst does not raise an attention
// (spec/LINK-PROTOCOL.md section 8, reasons 2 and 3; docs/ARCHITECTURE.md section 6).
//
// Why a minimum time at all: the pack and the load share RS3/RS4 (the hardware repository's
// CURRENT-SENSE.md section 1 - the load hangs on VCC), and a Walter LTE burst pulls up to 1.5 A
// through that series path. The pack's own terminal voltage sags for the length of the burst and
// comes back; an attention costs the battery an LTE attach, so the node waits to see the voltage
// stay down before it tells anybody.
//
// What is *not* here: the hourly limit. Every attention goes through the machine of
// src/common/link/attention/, which already allows at most one new attention per reason per hour
// (section 8) - so this class says "the battery entered this level" and the machine decides
// whether the Walter hears about it again.
//
// Host-portable: no Arduino, no MeshCore, no heap, no floating point. Every method takes `now_ms`
// and every comparison is a signed difference, so a millis() wrap is one subtraction and
// test/test_power can wind the clock in a line.

#pragma once

#include <stdint.h>

namespace twinoak {
namespace power {

// ----------------------------------------------------------------------------- the numbers

// The levels, in the order a discharge meets them.
enum BatteryLevel : uint8_t {
  kBatteryNormal = 0,
  kBatteryLow = 1,
  kBatteryCritical = 2,
};

// What poll() answers when the level did not change.
constexpr uint8_t kBatteryNoChange = 0xFF;

// **Placeholders until the bench has a discharge curve** of the Meshnology 10 Ah 1S pack
// (tasks/phase-1/06-power-and-environment.md). They are settable and kept across a restart -
// `power.low_mv` and `power.crit_mv`, src/roles/repeater/app/Settings.h - which is what makes them placeholders
// rather than decisions.
constexpr uint16_t kDefaultLowMv = 3500;
constexpr uint16_t kDefaultCritMv = 3300;

// What a settable threshold may be: inside the window a 1S lithium pack lives in, with room for
// a cell chemistry that is not this one. Below the floor the pack's own protection has already
// disconnected; above the ceiling the charger never gets there (VCC regulates at 4.200 V,
// CURRENT-SENSE.md section 1).
constexpr uint16_t kThresholdFloorMv = 2500;
constexpr uint16_t kThresholdCeilingMv = 4300;

// How far the voltage must come back before a level is left. 100 mV is more than the sag of a
// 1.5 A burst through the series path (0.25-0.35 V is that sag, CURRENT-SENSE.md section 3.3 -
// which is why the minimum time below does the real work and the hysteresis only stops a level
// from flickering at its own edge).
constexpr uint16_t kHysteresisMv = 100;

// How long a level must hold before it is believed, in or out.
constexpr uint32_t kMinTimeBelowMs = 60000;

// Whether a pair of thresholds is one this firmware accepts: both inside the window, and low far
// enough above critical that their hysteresis bands do not overlap.
bool thresholdsAcceptable(uint16_t low_mv, uint16_t crit_mv);

// ----------------------------------------------------------------------------- the watch

class BatteryWatch {
 public:
  // The thresholds, which the preferences file restores at boot (src/common/power/Prefs.h). Refused -
  // and nothing changed - when thresholdsAcceptable() says no.
  bool setThresholds(uint16_t low_mv, uint16_t crit_mv);
  uint16_t lowMilliVolts() const { return low_mv_; }
  uint16_t critMilliVolts() const { return crit_mv_; }

  // One reading of channel 2, once a pass. `valid` is that channel's validity bit: an INA3221
  // that stopped answering is not a low battery, so an invalid reading holds the level where it
  // is and restarts the clock on whatever change was pending.
  //
  // Returns the level this poll **entered** - kBatteryLow or kBatteryCritical, which the caller
  // raises as section 8's reason 2 or 3, or kBatteryNormal when the pack recovered - and
  // kBatteryNoChange the rest of the time.
  uint8_t poll(bool valid, uint16_t batt_mv, uint32_t now_ms);

  // --- what a report and the bench read ------------------------------------------------

  uint8_t level() const { return level_; }
  // The millivolts of the last valid reading: EV_ATTENTION's `detail` for reasons 2 and 3.
  uint16_t lastMilliVolts() const { return last_mv_; }
  // The level that is waiting for the minimum time to pass, or kBatteryNoChange.
  uint8_t pendingLevel() const { return pending_; }

 private:
  // The level this voltage means, given where the watch stands now (the hysteresis is relative
  // to the current level and to nothing else).
  uint8_t levelOf(uint16_t mv) const;

  uint16_t low_mv_ = kDefaultLowMv;
  uint16_t crit_mv_ = kDefaultCritMv;

  uint8_t level_ = kBatteryNormal;
  uint8_t pending_ = kBatteryNoChange;
  uint32_t pending_since_ms_ = 0;
  uint16_t last_mv_ = 0;
};

}  // namespace power
}  // namespace twinoak
