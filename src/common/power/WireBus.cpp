#include "common/power/WireBus.h"

#include "common/board/CoreHeaders.h"  // Arduino, Wire - as system headers

namespace twinoak {
namespace power {

bool WireBus::writeRegister(uint8_t address, uint8_t reg, uint16_t value) {
  const uint32_t started = micros();
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(uint8_t(value >> 8));
  Wire.write(uint8_t(value & 0xFF));
  const bool ok = Wire.endTransmission() == 0;
  last_us_ = micros() - started;
  return ok;
}

bool WireBus::readRegister(uint8_t address, uint8_t reg, uint16_t& value) {
  const uint32_t started = micros();
  Wire.beginTransmission(address);
  Wire.write(reg);
  bool ok = Wire.endTransmission(false) == 0;  // repeated start, as the INA3221 asks for
  if (ok) ok = Wire.requestFrom(address, size_t(2)) == 2;
  if (ok) {
    const uint16_t high = uint16_t(Wire.read());
    const uint16_t low = uint16_t(Wire.read());
    value = uint16_t((high << 8) | low);
  }
  last_us_ = micros() - started;
  return ok;
}

}  // namespace power
}  // namespace twinoak
