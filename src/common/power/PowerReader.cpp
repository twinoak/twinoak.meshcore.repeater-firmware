#include "common/power/PowerReader.h"

namespace twinoak {
namespace power {

void PowerReader::begin(uint32_t now_ms) {
  if (!configure(now_ms)) return;
  // The first sweep is due at once: a `status` a second after boot should already carry a
  // battery voltage.
  startSweep();
  last_sweep_ms_ = now_ms;
}

void PowerReader::loop(uint32_t now_ms) {
  if (!present_) {
    if (int32_t(now_ms - retry_at_ms_) < 0) return;  // still in the back-off
    if (!configure(now_ms)) return;
    startSweep();
    return;  // the pass that found the device does not also read it
  }

  if (!sweeping_) {
    const bool due = !have_swept_ || int32_t(now_ms - last_sweep_ms_) >= int32_t(kSweepIntervalMs);
    if (!due) return;
    startSweep();
  }

  for (uint8_t done = 0; done < kChannelsPerPass && next_channel_ < kChannels; done++) {
    const uint8_t index = next_channel_++;
    const ReadResult got = readChannel(index);
    if (got == kReadTooSlow) {
      // A bus that answers but answers far too slowly is one to leave alone: the thing it is
      // holding up is a repeater. The sweep is abandoned where it stands - "a stuck transaction
      // does not hold the schedule".
      overruns_++;
      dropDevice(now_ms);
      return;
    }
    if (got == kReadFailed) {
      // A missing reading, never a stalled loop. The other channels are still read: on this
      // board they are three ranges of one chip, and one register that would not answer is not
      // a reason to throw the other two away.
      readings_.ch[index].valid = false;
      failures_++;
      failed_this_sweep_++;
    }
  }

  if (next_channel_ >= kChannels) {
    sweeping_ = false;
    have_swept_ = true;
    last_sweep_ms_ = now_ms;
    if (failed_this_sweep_ >= kChannels) {
      // Not one channel answered: the device is gone (the B-board unplugged, the always-on 3.3 V
      // rail down), so stop asking until the back-off is over.
      dropDevice(now_ms);
      return;
    }
    sweeps_++;
  }
}

void PowerReader::startSweep() {
  sweeping_ = true;
  next_channel_ = 0;
  failed_this_sweep_ = 0;
}

bool PowerReader::configure(uint32_t now_ms) {
  present_ = false;
  uint16_t id = 0;
  if (!bus_.readRegister(address_, kRegManufacturerId, id)) {
    failures_++;
    dropDevice(now_ms);
    return false;
  }
  if (lastWasTooSlow()) {
    overruns_++;
    dropDevice(now_ms);
    return false;
  }
  // A device that answers at 0x42 but is not an INA3221 is not one to write a configuration word
  // into (INA3221 datasheet, register 0xFE = 0x5449).
  if (id != kManufacturerId) {
    dropDevice(now_ms);
    return false;
  }
  if (!bus_.writeRegister(address_, kRegConfig, kConfigWord)) {
    failures_++;
    dropDevice(now_ms);
    return false;
  }
  if (lastWasTooSlow()) {
    overruns_++;
    dropDevice(now_ms);
    return false;
  }
  present_ = true;
  return true;
}

PowerReader::ReadResult PowerReader::readChannel(uint8_t index) {
  const uint8_t channel = uint8_t(index + 1);
  uint16_t shunt_reg = 0;
  uint16_t bus_reg = 0;
  if (!bus_.readRegister(address_, shuntRegOf(channel), shunt_reg)) {
    return lastWasTooSlow() ? kReadTooSlow : kReadFailed;
  }
  if (lastWasTooSlow()) return kReadTooSlow;
  if (!bus_.readRegister(address_, busRegOf(channel), bus_reg)) {
    return lastWasTooSlow() ? kReadTooSlow : kReadFailed;
  }
  if (lastWasTooSlow()) return kReadTooSlow;
  readings_.ch[index].current_ma = currentMilliAmps(rawOf(shunt_reg), kShuntMilliOhm[index]);
  readings_.ch[index].bus_mv = busMilliVolts(rawOf(bus_reg));
  readings_.ch[index].valid = true;
  return kReadOk;
}

void PowerReader::dropDevice(uint32_t now_ms) {
  present_ = false;
  sweeping_ = false;
  next_channel_ = 0;
  failed_this_sweep_ = 0;
  for (uint8_t i = 0; i < kChannels; i++) readings_.ch[i].valid = false;
  retry_at_ms_ = now_ms + kDeviceRetryMs;
}

}  // namespace power
}  // namespace twinoak
