#include "Crc16.h"

namespace twinoak {
namespace link {

uint16_t crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc = uint16_t(crc ^ (uint16_t(data[i]) << 8));
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x8000) ? uint16_t(uint16_t(crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
  }
  return crc;
}

}  // namespace link
}  // namespace twinoak
