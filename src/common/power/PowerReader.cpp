#include "common/power/PowerReader.h"

namespace twinoak {
namespace power {

void PowerReader::begin(uint32_t now_ms) {
  configure(now_ms);
  // configure() wrote the configuration word, which is also what starts the device's first
  // averaging window and clears the ready flag: the first sweep is that window, about 845 ms
  // from here, and a `status` before it has no reading - as it had none before this schedule
  // either, because the registers of a device that has just been configured hold no average yet.
}

void PowerReader::loop(uint32_t now_ms) {
  if (!present_) {
    if (int32_t(now_ms - retry_at_ms_) < 0) return;  // still in the back-off
    configure(now_ms);
    // The pass that found the device does not also read it: configure() set the first poll for a
    // window from now, because the device has only just begun that window.
    return;
  }

  if (!sweeping_) {
    if (int32_t(now_ms - next_poll_ms_) < 0) return;  // the window cannot have finished yet
    bool ready = false;
    const ReadResult asked = readReady(ready);
    if (asked == kReadTooSlow) {
      overruns_++;
      dropDevice(now_ms);
      return;
    }
    if (asked == kReadFailed) {
      // The one register that is the device itself rather than a channel of it: a part that will
      // not say whether it has finished converting is a part there is nothing to read from, so
      // this is the device going away and not a missing reading (configure() treats the
      // manufacturer ID the same way).
      failures_++;
      dropDevice(now_ms);
      return;
    }
    if (!ready && int32_t(now_ms - ready_at_ms_) < int32_t(kReadyWaitMaxMs)) {
      next_poll_ms_ = now_ms + kReadyPollMs;
      return;
    }
    if (!ready) {
      // Two windows and no flag: read the registers anyway rather than go blind, which is what
      // this reader did before p3.10-01 (PowerReader.h, kReadyWaitMaxMs). The interval is past
      // the totals' bound, so this sweep is a gap and the readings are still answers.
      unflagged_++;
    }
    // The window finished at about now, and reading the flag cleared it, so the next one is a
    // whole kConversionCycleMs away. This is the instant the sweep's charge is attributed to.
    ready_at_ms_ = now_ms;
    next_poll_ms_ = now_ms + kReadyPollFromMs;
    startSweep();
    return;  // the pass that found the flag does not also read a channel
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
    if (failed_this_sweep_ >= kChannels) {
      // Not one channel answered: the device is gone (the B-board unplugged, the always-on 3.3 V
      // rail down), so stop asking until the back-off is over.
      dropDevice(now_ms);
      return;
    }
    sweeps_++;
    // The window, added to what has flowed since boot - at the instant the device said it had
    // finished, not at the instant the last of its three channels was read off the bus.
    totals_.note(readings_, ready_at_ms_);
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
  // The configuration write started the device's first averaging window and cleared the ready
  // flag: time begins here for the totals, so the back-off that came before is in no total.
  totals_.anchor(now_ms);
  ready_at_ms_ = now_ms;
  next_poll_ms_ = now_ms + kReadyPollFromMs;
  return true;
}

PowerReader::ReadResult PowerReader::readReady(bool& ready) {
  uint16_t flags = 0;
  if (!bus_.readRegister(address_, kRegMaskEnable, flags)) {
    return lastWasTooSlow() ? kReadTooSlow : kReadFailed;
  }
  if (lastWasTooSlow()) return kReadTooSlow;
  ready = (flags & kMaskConversionReady) != 0;
  return kReadOk;
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
