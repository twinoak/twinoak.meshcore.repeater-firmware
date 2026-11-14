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
// **The library refuses none of these.** tasks/phase-1/reports/06.md said its `isType()` gate
// does not list LPP_RELATIVE_HUMIDITY, LPP_BAROMETRIC_PRESSURE or LPP_POWER, so that adding one
// would write nothing; that reading of the gate was wrong, and the enclosure's humidity and
// pressure were left off the wire for a year because of it (p3.12-01, 14 November 2026). The gate
// lists every type in it, each behind a `CAYENNE_DISABLE_*` that nothing in this build defines,
// and `test/test_cayennelpp` now links the library and shows the bytes rather than citing the
// source. Upstream's own BME280 writes the same two types (EnvironmentSensorManager.cpp's
// query_bme280 at the pin 81dbae59). LPP_POWER stays out by choice: meshmaster.dk computes it
// from the volts and the amps that are already here.
struct TypeInfo {
  uint8_t type;
  uint8_t size;
  uint32_t multiplier;
  // getTypeSigned()'s own list, which is a *different* list from isType()'s: LPP_VOLTAGE,
  // LPP_CURRENT and LPP_TEMPERATURE are in it and **LPP_GENERIC_SENSOR is not**, so a negative
  // generic sensor keeps its magnitude and is never two's-complemented (CayenneLPP.cpp's
  // `is_signed & sign` in addField). The totals are never negative, and this is the number the
  // mirror has to copy rather than assume.
  bool is_signed;
};

const TypeInfo kTypes[] = {
    {kLppGenericSensor, 4, 1, false},
    {kLppTemperature, 2, 10, true},
    {kLppRelativeHumidity, 1, 2, false},
    {kLppBarometricPressure, 2, 10, false},
    {kLppVoltage, 2, 100, true},
    {kLppCurrent, 2, 1000, true},
};

const TypeInfo* typeInfoOf(uint8_t type) {
  for (size_t i = 0; i < sizeof(kTypes) / sizeof(kTypes[0]); i++) {
    if (kTypes[i].type == type) return &kTypes[i];
  }
  return nullptr;
}

// One total as the generic sensor carries it: the step count, held at kTotalCeiling so that the
// float the library takes can never be 2^32 or more (Telemetry.h says why). The division is a
// floor, so a total always reads at or below the truth and never above it.
float stepsOf(uint64_t amount, uint64_t per_step) {
  const uint64_t steps = amount / per_step;
  return float(steps > uint64_t(kTotalCeiling) ? uint64_t(kTotalCeiling) : steps);
}

float chargeOf(const Flow& f) { return stepsOf(f.charge_uc, kChargeStepMicroCoulomb); }
float energyOf(const Flow& f) { return stepsOf(f.energy_uj, kEnergyStepMicroJoule); }

}  // namespace

void fillTotals(const PowerTotals& totals, TelemetrySnapshot& out) {
  out.totals_counted = totals.hasCounted();
  for (uint8_t i = 0; i < PowerTotals::kFlows; i++) out.totals[i] = totals.flow(i);
  out.totals_covered_ms = totals.millisCovered();
}

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

  // Channel 5, the BME280's three quantities in the order ENV_GET carries them. Each is the same
  // number ENV_GET reports, converted into the unit its Cayenne-LPP type takes - so the link's
  // answer and the mesh's cannot differ by more than the type's own resolution - and each is
  // there only while its validity bit is set (Environment.h). A quantity that is missing takes
  // its field out and moves none of the others.
  if (s.env.temperatureValid()) {
    // The same centidegrees ENV_GET reports, so the link's number and the mesh's cannot differ.
    push(out, cap, n, kLppChannelEnclosure, kLppTemperature, float(s.env.temp_centi) / 100.0f);
  }
  if (s.env.humidityValid()) {
    // Percent, from ENV_GET's hundredths of one. The type's step is half a per cent.
    push(out, cap, n, kLppChannelEnclosure, kLppRelativeHumidity, float(s.env.rh_centi) / 100.0f);
  }
  if (s.env.pressureValid()) {
    // Hectopascal, from ENV_GET's pascal - the unit the type takes, and the one upstream's own
    // BME280 hands it (`readPressure() / 100`). The type's step is a tenth of a hectopascal.
    push(out, cap, n, kLppChannelEnclosure, kLppBarometricPressure,
         float(s.env.pressure_pa) / 100.0f);
  }

  // Channels 6 to 14: what has flowed since the firmware started (PowerTotals.h). **All nine or
  // none** - until a first sweep has counted there is nothing to subtract and a zero would read
  // as a number, so they are left out together (tasks/phase-3.10/02-the-totals-told.md).
  if (s.totals_counted) {
    const Flow& panel = s.totals[PowerTotals::kPanel];
    const Flow& in = s.totals[PowerTotals::kBatteryIn];
    const Flow& out_flow = s.totals[PowerTotals::kBatteryOut];
    const Flow& load = s.totals[PowerTotals::kLoad];
    push(out, cap, n, kLppChannelPanelCharge, kLppGenericSensor, chargeOf(panel));
    push(out, cap, n, kLppChannelPanelEnergy, kLppGenericSensor, energyOf(panel));
    push(out, cap, n, kLppChannelBatteryChargeIn, kLppGenericSensor, chargeOf(in));
    push(out, cap, n, kLppChannelBatteryChargeOut, kLppGenericSensor, chargeOf(out_flow));
    push(out, cap, n, kLppChannelBatteryEnergyIn, kLppGenericSensor, energyOf(in));
    push(out, cap, n, kLppChannelBatteryEnergyOut, kLppGenericSensor, energyOf(out_flow));
    push(out, cap, n, kLppChannelLoadCharge, kLppGenericSensor, chargeOf(load));
    push(out, cap, n, kLppChannelLoadEnergy, kLppGenericSensor, energyOf(load));
    push(out, cap, n, kLppChannelCovered, kLppGenericSensor,
         stepsOf(s.totals_covered_ms, kCoveredStepMillis));
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
    if (info == nullptr) continue;  // not a type this firmware writes: nothing is written
    if (cursor + info->size + 2 > cap) continue;  // its overflow check: nothing is written

    // addField, line for line: the sign is taken off the float first, the product is truncated
    // towards zero into a u32, and a negative value becomes the two's complement of that.
    float value = fields[i].value;
    const bool negative = value < 0.0f;
    if (negative) value = -value;
    uint32_t v = uint32_t(value * float(info->multiplier));
    // addField's `is_signed & sign`: an **unsigned** type keeps the magnitude and is never
    // two's-complemented, which is the generic sensor's case. A four-byte mask is written out
    // rather than shifted, because `1u << 32` is undefined where the library's own expression
    // never meets a four-byte signed type.
    if (info->is_signed && negative) {
      const uint32_t mask = info->size >= 4 ? 0xFFFFFFFFu
                                            : uint32_t((uint32_t(1) << (info->size * 8)) - 1);
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
