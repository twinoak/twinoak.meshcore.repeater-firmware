#include "common/power/Ina3221.h"

namespace twinoak {
namespace power {

const uint16_t kShuntMilliOhm[kChannels] = {
    TWINOAK_SHUNT_CH1_MILLIOHM,
    TWINOAK_SHUNT_CH2_MILLIOHM,
    TWINOAK_SHUNT_CH3_MILLIOHM,
};

uint16_t busMilliVolts(int16_t raw) {
  if (raw <= 0) return 0;
  return uint16_t(int32_t(raw) * kBusMilliVoltsPerLsb);
}

int16_t currentMilliAmps(int16_t raw, uint16_t shunt_milliohm) {
  if (shunt_milliohm == 0) return 0;  // a table that was never filled in; not a current of zero
  const int32_t microvolts = int32_t(raw) * kShuntMicroVoltsPerLsb;
  const int32_t shunt = int32_t(shunt_milliohm);
  // Rounded to the nearest milliamp, away from zero at a half, and symmetric in the sign - the
  // battery channel is the one that goes negative and must not round differently for it.
  const int32_t half = shunt / 2;
  const int32_t ma = microvolts >= 0 ? (microvolts + half) / shunt : -((-microvolts + half) / shunt);
  return int16_t(ma);
}

uint8_t PowerReadings::validBits() const {
  uint8_t bits = 0;
  for (uint8_t i = 0; i < kChannels; i++) {
    if (ch[i].valid) bits = uint8_t(bits | (1u << i));
  }
  return bits;
}

uint16_t PowerReadings::batteryMilliVolts() const { return ch[1].valid ? ch[1].bus_mv : 0; }

}  // namespace power
}  // namespace twinoak
