// The INA3221 reader: it samples all three channels on a schedule, keeps the last good value and
// a validity bit per channel, and gives up on a transaction that takes too long
// (tasks/phase-1/06-power-and-environment.md; docs/ARCHITECTURE.md section 6).
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

// How often all three channels are sampled. The device averages 64 samples over about 422 ms
// (Ina3221.h, kConfigWord), so once a second reads a fresh average every time; it is also fast
// enough for the battery thresholds, whose own minimum time below is a minute.
constexpr uint32_t kSweepIntervalMs = 1000;

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

  // One bounded pass, once per loop(). Reads at most kChannelsPerPass channels, and nothing at
  // all while the device is in its back-off or the sweep interval has not passed.
  void loop(uint32_t now_ms);

  // --- what the four readers read ------------------------------------------------------

  const PowerReadings& readings() const { return readings_; }

  // --- what a report and the bench read ------------------------------------------------

  bool devicePresent() const { return present_; }
  // Completed sweeps of all three channels, failed transactions, and transactions that went past
  // kTransactionBudgetUs, since boot.
  uint32_t sweeps() const { return sweeps_; }
  uint32_t failures() const { return failures_; }
  uint32_t overruns() const { return overruns_; }

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
  uint32_t last_sweep_ms_ = 0;
  uint32_t retry_at_ms_ = 0;
  bool have_swept_ = false;     // nothing has been sampled yet, so the interval has not started

  uint32_t sweeps_ = 0;
  uint32_t failures_ = 0;
  uint32_t overruns_ = 0;
};

}  // namespace power
}  // namespace twinoak
