// The INA3221 reader: it samples all three channels once per averaging window of the device's
// own, keeps the last good value and a validity bit per channel, adds the window up into
// PowerTotals, and gives up on a transaction that takes too long
// (tasks/phase-1/06-power-and-environment.md, tasks/phase-3.10/01-what-flowed.md;
// docs/ARCHITECTURE.md section 6).
//
// **The device says when.** A sweep begins when the conversion-ready flag says an averaging
// window has finished (Ina3221.h, kMaskConversionReady), not when a timer of ours says a second
// has passed. In continuous mode those windows tile time end to end, so one reading per window
// is every reading there is: no window is read twice, none is skipped, and the sum of the
// intervals is the whole of the time the device was answering - which is what makes
// PowerTotals.h's "the seconds covered" an honest number rather than 84 % of one.
//
// **Nothing reads the bus on demand.** A POWER_GET, a mesh telemetry request, a `status` and the
// battery thresholds all read the cache this class fills, so no request ever waits on I2C and a
// sensor that stopped answering is a missing reading rather than a stalled loop. That is the
// acceptance criterion "a request on the link never performs I2C", and it is a property of this
// shape rather than of any one handler.
//
// Host-portable: no Arduino, no MeshCore, no heap. The bus arrives through the port below, which
// is what lets test/test_power drive the whole reader against a fake bus - a channel that fails,
// a device that goes away and comes back, and a transaction that hangs.

#pragma once

#include <stdint.h>

#include "common/power/Ina3221.h"
#include "common/power/PowerTotals.h"

namespace twinoak {
namespace power {

// ----------------------------------------------------------------------------- the port

// One I2C master, with registers as the INA3221 has them: a one-byte register address, then a
// 16-bit big-endian value.
//
// `false` means *this transaction did not complete*: the device did not acknowledge, the bus
// reported an error, or the board gave up waiting. The reader never retries inside one pass; a
// failure is a missing reading and the next tick tries again.
struct I2cBus {
  virtual ~I2cBus() {}

  virtual bool writeRegister(uint8_t address, uint8_t reg, uint16_t value) = 0;
  virtual bool readRegister(uint8_t address, uint8_t reg, uint16_t& value) = 0;

  // How long the last transaction took, in microseconds, as the board measured it. The reader
  // compares it with kTransactionBudgetUs: a bus that answers but answers far too slowly is a
  // bus to leave alone for a while, because the thing it is holding up is a repeater.
  virtual uint32_t lastTransactionMicros() const = 0;
};

// ----------------------------------------------------------------------------- the numbers

// When the reader starts asking whether the averaging window has finished, measured from the
// moment the last one did. The window is kConversionCycleMs (845 ms, Ina3221.h) and the device's
// own oscillator is not ours, so the poll starts an eighth of a window early: a device running
// up to 12 % fast is still found before it could finish a *second* window, which is the one
// thing this schedule may not miss.
constexpr uint32_t kReadyPollFromMs = kConversionCycleMs - kConversionCycleMs / 8u;  // 740

// How often it asks after that: one register read, less than the two a channel costs. Five of
// them in a window at the worst, and the 25 ms is also the whole of the jitter between the
// window ending and this reader noticing - which is in the interval the totals integrate over
// and so is not lost.
constexpr uint32_t kReadyPollMs = 25;

// How long the reader waits for that flag before it reads the channels anyway.
//
// **Why there is a fallback at all.** That the INA3221 raises the flag once per averaging window
// in *continuous* mode is read out of the datasheet and out of the pinned Adafruit library's
// register map, and no test on a machine with no INA3221 on it can prove it: the fake raises the
// flag because the datasheet says the part does. A part that never raised it would, without this,
// read nothing for ever - no POWER_GET, no battery voltage, no thresholds - on a unit that is on
// a roof and reachable only by telemetry. So after two windows of silence the reader reads the
// registers regardless, which is exactly what it did before p3.10-01 and is a true reading of
// the last window the device finished.
//
// Two windows is past kMaxIntervalMs (PowerTotals.h), so a reading taken this way is a **gap**
// and adds nothing: the firmware keeps answering and the totals say honestly that they do not
// cover that time. `sweepsWithoutTheFlag()` is then the number to read, and docs/bench.md section 5
// is the line that settles it on a real part.
constexpr uint32_t kReadyWaitMaxMs = 2u * kConversionCycleMs;  // 1690

// What a sweep costs the loop: **one channel per pass**, two register reads. At 100 kHz that is
// about half a millisecond, and a sweep is spread over three passes of loop() rather than taken
// in one.
constexpr uint8_t kChannelsPerPass = 1;

// A transaction that takes longer than this is treated as a bus in trouble: the channel goes
// invalid and the device is left alone for kDeviceRetryMs. A register read at 100 kHz is about
// 500 us, so this is six times the honest cost and cannot be reached by a healthy bus.
constexpr uint32_t kTransactionBudgetUs = 3000;

// How long the reader leaves a device alone after it failed to answer, or answered too slowly.
// Long enough that a dead sensor costs the loop two transactions a minute; short enough that one
// that comes back (a B-board plugged in again) is found within seconds.
constexpr uint32_t kDeviceRetryMs = 5000;

// ----------------------------------------------------------------------------- the reader

class PowerReader {
 public:
  explicit PowerReader(I2cBus& bus, uint8_t address = kIna3221Address)
      : bus_(bus), address_(address) {}

  // Probes the device and writes its configuration. Called from TwinOakSensors::begin(); a
  // device that is not there is simply not present and every channel reads invalid, which is
  // what POWER_GET then answers.
  void begin(uint32_t now_ms);

  // One bounded pass, once per loop(). Reads at most kChannelsPerPass channels - or, between
  // sweeps, at most one ready-flag register - and nothing at all while the device is in its
  // back-off or the next poll is not due.
  void loop(uint32_t now_ms);

  // --- what the four readers read ------------------------------------------------------

  const PowerReadings& readings() const { return readings_; }

  // What has flowed since boot, added up from every completed sweep (PowerTotals.h). Nothing
  // puts it on the wire yet; task 02 of this phase does that.
  const PowerTotals& totals() const { return totals_; }

  // --- what a report and the bench read ------------------------------------------------

  bool devicePresent() const { return present_; }
  // Completed sweeps of all three channels, failed transactions, and transactions that went past
  // kTransactionBudgetUs, since boot.
  uint32_t sweeps() const { return sweeps_; }
  uint32_t failures() const { return failures_; }
  uint32_t overruns() const { return overruns_; }
  // Sweeps taken after kReadyWaitMaxMs of silence from the conversion-ready flag. On a healthy
  // part this is 0 and stays 0; anything else says the flag is not doing what the datasheet
  // says, and every one of those sweeps is a gap in the totals.
  uint32_t sweepsWithoutTheFlag() const { return unflagged_; }

 private:
  enum ReadResult : uint8_t {
    kReadOk = 0,
    kReadFailed = 1,    // the device did not answer this register: a missing reading
    kReadTooSlow = 2,   // it answered, but past kTransactionBudgetUs: a bus in trouble
  };

  // Probes and configures. True when the device answered with the INA3221's manufacturer ID and
  // took its configuration; false puts it in the back-off.
  bool configure(uint32_t now_ms);
  // Reads one channel's shunt and bus registers into the cache.
  ReadResult readChannel(uint8_t index);
  // Reads the Mask/Enable register and says whether the conversion-ready flag was set. Reading
  // it is what clears the flag, so a window is seen exactly once (Ina3221.h).
  ReadResult readReady(bool& ready);
  // Begins a sweep of all three channels at channel 1.
  void startSweep();
  // The device is not to be asked again until the back-off is over, and no channel is valid in
  // the meantime: a bus that could not be trusted for one reading cannot be for the others.
  void dropDevice(uint32_t now_ms);
  // Whether the last transaction went past its budget.
  bool lastWasTooSlow() const { return bus_.lastTransactionMicros() > kTransactionBudgetUs; }

  I2cBus& bus_;
  uint8_t address_;

  PowerReadings readings_;

  bool present_ = false;
  bool sweeping_ = false;       // a sweep is in progress; next_channel_ is where it stands
  uint8_t next_channel_ = 0;    // 0..kChannels-1
  uint8_t failed_this_sweep_ = 0;
  uint32_t next_poll_ms_ = 0;   // when the ready flag is next worth asking about
  uint32_t ready_at_ms_ = 0;    // when the flag said this window had finished
  uint32_t retry_at_ms_ = 0;

  PowerTotals totals_;

  uint32_t sweeps_ = 0;
  uint32_t failures_ = 0;
  uint32_t overruns_ = 0;
  uint32_t unflagged_ = 0;
};

}  // namespace power
}  // namespace twinoak
