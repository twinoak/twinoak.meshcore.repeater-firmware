#include "common/power/Telemetry.h"

namespace twinoak {
namespace power {
namespace {

// The INA3221 channel each of ours reports, and the LPP channel it goes on.
struct ChannelMap {
  uint8_t index;       // 0..kChannels-1
  uint8_t lpp_channel;
};

const ChannelMap kMap[kChannels] = {
    {0, kLppChannelPanel},
    {1, kLppChannelBattery},
    {2, kLppChannelLoad},
};

bool push(LppField* out, size_t cap, size_t& n, uint8_t channel, uint8_t type, float value) {
  if (n >= cap) return false;
  out[n].channel = channel;
  out[n].type = type;
  out[n].value = value;
  n++;
  return true;
}

// CayenneLPP's own three numbers per type (src/CayenneLPP.cpp's getTypeSize, getTypeMultiplier
// and getTypeSigned at version 1.6.1). Only the types this firmware writes are here, so a field
// of a new type has to be added deliberately rather than silently encoded wrongly.
//
// The three the library refuses are worth knowing about and are in tasks/phase-1/reports/06.md:
// its `isType()` gate does not list LPP_RELATIVE_HUMIDITY, LPP_BAROMETRIC_PRESSURE or LPP_POWER,
// so addRelativeHumidity, addBarometricPressure and addPower write **nothing** and return 0.
// That is why the enclosure's humidity and pressure are on the box link and not in the LPP.
struct TypeInfo {
  uint8_t type;
  uint8_t size;
  uint32_t multiplier;
};

const TypeInfo kTypes[] = {
    {kLppTemperature, 2, 10},
    {kLppVoltage, 2, 100},
    {kLppCurrent, 2, 1000},
};

const TypeInfo* typeInfoOf(uint8_t type) {
  for (size_t i = 0; i < sizeof(kTypes) / sizeof(kTypes[0]); i++) {
    if (kTypes[i].type == type) return &kTypes[i];
  }
  return nullptr;
}

}  // namespace

size_t ourFields(const TelemetrySnapshot& s, uint8_t perm_mask, LppField* out, size_t cap) {
  size_t n = 0;
  if (out == nullptr) return 0;
  if ((perm_mask & kPermEnvironment) == 0) return 0;

  for (uint8_t i = 0; i < kChannels; i++) {
    const ChannelReading& r = s.power.ch[kMap[i].index];
    if (!r.valid) continue;
    // The shunts are already applied, so these are volts and amps (section 5.8: "applying them
    // is the RAK's job").
    push(out, cap, n, kMap[i].lpp_channel, kLppVoltage, float(r.bus_mv) / 1000.0f);
    push(out, cap, n, kMap[i].lpp_channel, kLppCurrent, float(r.current_ma) / 1000.0f);
  }

  if (s.env.temperatureValid()) {
    // The same centidegrees ENV_GET reports, so the link's number and the mesh's cannot differ.
    push(out, cap, n, kLppChannelEnclosure, kLppTemperature, float(s.env.temp_centi) / 100.0f);
  }
  return n;
}

size_t telemetryFields(const TelemetrySnapshot& s, uint8_t perm_mask, LppField* out, size_t cap) {
  size_t n = 0;
  if (out == nullptr) return 0;

  // MyMesh.cpp:244 at the pin: addVoltage(TELEM_CHANNEL_SELF, getBattMilliVolts() / 1000.0f),
  // before any permission is looked at.
  push(out, cap, n, kLppChannelSelf, kLppVoltage, float(s.batt_mv) / 1000.0f);

  // Then whatever the sensor manager adds - upstream's part of which is empty on this board
  // (Telemetry.h, LppChannel).
  if (n < cap) n += ourFields(s, perm_mask, out + n, cap - n);

  // MyMesh.cpp:254-257 at the pin: the MCU's own temperature on channel 1, unless it is NAN.
  if (s.mcu_temp_valid) push(out, cap, n, kLppChannelSelf, kLppTemperature, s.mcu_temp_c);
  return n;
}

size_t encodeLpp(const LppField* fields, size_t count, uint8_t* out, size_t cap) {
  if (fields == nullptr || out == nullptr) return 0;
  size_t cursor = 0;
  for (size_t i = 0; i < count; i++) {
    const TypeInfo* info = typeInfoOf(fields[i].type);
    if (info == nullptr) continue;  // the library's isType() gate: nothing is written
    if (cursor + info->size + 2 > cap) continue;  // its overflow check: nothing is written

    // addField, line for line: the sign is taken off the float first, the product is truncated
    // towards zero into a u32, and a negative value becomes the two's complement of that.
    float value = fields[i].value;
    const bool negative = value < 0.0f;
    if (negative) value = -value;
    uint32_t v = uint32_t(value * float(info->multiplier));
    if (negative) {
      const uint32_t mask = (uint32_t(1) << (info->size * 8)) - 1;
      v = v & mask;
      v = mask - v + 1;
    }

    out[cursor++] = fields[i].channel;
    out[cursor++] = fields[i].type;
    for (uint8_t b = 1; b <= info->size; b++) {
      out[cursor + info->size - b] = uint8_t(v & 0xFF);
      v >>= 8;
    }
    cursor += info->size;
  }
  return cursor;
}

}  // namespace power
}  // namespace twinoak
