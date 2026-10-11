#include "common/link/codec/Cobs.h"

namespace twinoak {
namespace link {

size_t cobsEncode(const uint8_t* raw, size_t len, uint8_t* out, size_t cap) {
  if (out == nullptr || cap == 0 || (len > 0 && raw == nullptr)) return 0;
  // out[code_i] is the code byte of the block being filled: how many bytes follow it before the
  // next zero of the input, plus one. It is written when that zero is met, and at the end.
  size_t code_i = 0;
  size_t o = 1;
  unsigned code = 1;
  for (size_t i = 0; i < len; i++) {
    if (raw[i] == 0) {
      out[code_i] = uint8_t(code);
      if (o >= cap) return 0;
      code_i = o++;
      code = 1;
    } else {
      // The next byte would make a block of 254 and need the 0xFF code, which this link never
      // sends (LINK-PROTOCOL.md section 2): refuse rather than encode something a decoder of
      // ours would drop.
      if (code == 254 || o >= cap) return 0;
      out[o++] = raw[i];
      code++;
    }
  }
  out[code_i] = uint8_t(code);
  return o;
}

CobsError cobsDecode(const uint8_t* enc, size_t len, uint8_t* out, size_t cap, size_t& out_len) {
  out_len = 0;
  if (enc == nullptr || out == nullptr) return CobsError::kNoRoom;
  if (len == 0) return CobsError::kEmpty;
  size_t i = 0;
  while (i < len) {
    const uint8_t code = enc[i];
    if (code == 0) return CobsError::kZeroInside;
    if (code == 0xFF) return CobsError::kExtendedCode;
    i++;
    const size_t end = i + code - 1;
    if (end > len) return CobsError::kPastEnd;
    while (i < end) {
      if (out_len >= cap) return CobsError::kNoRoom;
      out[out_len++] = enc[i++];
    }
    // A code shorter than its block's room stood for a zero of the input - unless the frame ends
    // here, where the last code byte is just the tail's length.
    if (end < len) {
      if (out_len >= cap) return CobsError::kNoRoom;
      out[out_len++] = 0;
    }
  }
  return CobsError::kNone;
}

}  // namespace link
}  // namespace twinoak
