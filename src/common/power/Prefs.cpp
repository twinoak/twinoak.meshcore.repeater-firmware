#include "common/power/Prefs.h"

#include "common/power/BatteryWatch.h"

namespace twinoak {
namespace power {
namespace {

// "PWR1", little-endian: our preferences, version 1. It differs from the clock file's "CLK1"
// (src/roles/repeater/clock/ClockKeeper.cpp) so that neither can be read as the other.
const uint32_t kPrefsMagic = 0x31525750u;

void putU16(uint8_t* out, uint16_t v) {
  out[0] = uint8_t(v);
  out[1] = uint8_t(v >> 8);
}

uint16_t getU16(const uint8_t* in) { return uint16_t(uint16_t(in[0]) | (uint16_t(in[1]) << 8)); }

void putU32(uint8_t* out, uint32_t v) {
  out[0] = uint8_t(v);
  out[1] = uint8_t(v >> 8);
  out[2] = uint8_t(v >> 16);
  out[3] = uint8_t(v >> 24);
}

uint32_t getU32(const uint8_t* in) {
  return uint32_t(in[0]) | (uint32_t(in[1]) << 8) | (uint32_t(in[2]) << 16) |
         (uint32_t(in[3]) << 24);
}

// The clock file's check byte, same arithmetic: a byte that is not the sum of the others, which
// catches a zeroed record, a record of all ones and a single flipped byte.
uint8_t checkByte(const uint8_t* data, size_t len) {
  uint8_t sum = 0x5A;
  for (size_t i = 0; i < len; i++) sum = uint8_t(sum + data[i] + 1);
  return uint8_t(~sum);
}

}  // namespace

void encodeSavedPrefs(const SavedPrefs& in, uint8_t out[kSavedPrefsBytes]) {
  putU32(out, kPrefsMagic);
  putU16(out + 4, in.low_mv);
  putU16(out + 6, in.crit_mv);
  out[8] = 0;  // no flags yet; a later version gives them meaning behind a new magic
  out[9] = checkByte(out, 9);
}

bool decodeSavedPrefs(const uint8_t* data, size_t len, SavedPrefs& out) {
  if (data == nullptr || len < kSavedPrefsBytes) return false;
  if (getU32(data) != kPrefsMagic) return false;
  if (data[9] != checkByte(data, 9)) return false;
  const uint16_t low = getU16(data + 4);
  const uint16_t crit = getU16(data + 6);
  if (!thresholdsAcceptable(low, crit)) return false;
  out.low_mv = low;
  out.crit_mv = crit;
  return true;
}

}  // namespace power
}  // namespace twinoak
