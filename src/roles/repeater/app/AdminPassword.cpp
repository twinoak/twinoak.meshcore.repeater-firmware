#include "roles/repeater/app/AdminPassword.h"

#include <string.h>

namespace twinoak {

const char kPasswordAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

namespace {
// The largest multiple of the alphabet's size that fits a byte: 4 * 62. A byte at or above it is
// rejected rather than reduced, which is what keeps the reduction unbiased.
constexpr unsigned kRejectFrom = (256 / kPasswordAlphabetSize) * kPasswordAlphabetSize;  // 248
}  // namespace

size_t generatePassword(char* out, size_t out_size, RandomSource& rng) {
  if (out_size == 0) return 0;
  const size_t want = out_size - 1;
  size_t have = 0;
  uint8_t bytes[16];
  while (have < want) {
    rng.fill(bytes, sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes) && have < want; i++) {
      if (bytes[i] >= kRejectFrom) continue;
      out[have++] = kPasswordAlphabet[bytes[i] % kPasswordAlphabetSize];
    }
  }
  out[have] = 0;
  memset(bytes, 0, sizeof(bytes));
  return have;
}

bool needsFreshPassword(bool saved_prefs_found, const char* current, const char* build_default) {
  if (!saved_prefs_found) return true;
  return strcmp(current, build_default) == 0;
}

}  // namespace twinoak
