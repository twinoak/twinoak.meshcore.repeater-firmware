#include "common/power/LinkPower.h"

namespace twinoak {
namespace power {

bool LinkPower::readPower(link::PowerResponse& out) {
  const TelemetrySnapshot s = src_.snapshot();
  out.valid = s.power.validBits();
  for (uint8_t i = 0; i < kChannels; i++) {
    // "A channel whose bit is clear reads 0" (section 5.8), so a stale value is never sent with
    // its validity bit down.
    const bool ok = s.power.ch[i].valid;
    out.channels[i].bus_mv = ok ? s.power.ch[i].bus_mv : uint16_t(0);
    out.channels[i].current_ma = ok ? s.power.ch[i].current_ma : int16_t(0);
  }
  return true;
}

bool LinkPower::readEnv(link::EnvResponse& out) {
  const TelemetrySnapshot s = src_.snapshot();
  out.valid = s.env.valid;
  out.temp_centi = (s.env.valid & kEnvTemperatureBit) ? s.env.temp_centi : int16_t(0);
  out.rh_centi = (s.env.valid & kEnvHumidityBit) ? s.env.rh_centi : uint16_t(0);
  out.pressure_pa = (s.env.valid & kEnvPressureBit) ? s.env.pressure_pa : uint32_t(0);
  return true;
}

size_t LinkPower::readLpp(uint8_t perm_mask, uint8_t* out, size_t cap) {
  const TelemetrySnapshot s = src_.snapshot();
  LppField fields[kMaxLppFields];
  const size_t n = telemetryFields(s, perm_mask, fields, kMaxLppFields);
  // The mesh's own cap, never the link's: this has to be what a mesh client would have been
  // told, and the link's payload is the larger of the two (kMeshLppCap is 180, a response body
  // holds 239).
  const size_t room = cap < kMeshLppCap ? cap : kMeshLppCap;
  return encodeLpp(fields, n, out, room);
}

}  // namespace power
}  // namespace twinoak
