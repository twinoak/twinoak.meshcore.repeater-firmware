// COBS - consistent overhead byte stuffing - as the box link uses it (LINK-PROTOCOL.md section 2).
//
// COBS removes every zero from a packet, so 0x00 appears on the wire only as the frame
// terminator. A packet is at most 244 bytes, so no run inside one reaches 254 bytes: the overhead
// is exactly one byte and the 0xFF code of general COBS never occurs. The encoder here does not
// implement it - a run that long is a caller's bug and is refused - and the decoder refuses a
// frame that carries it, as the specification says.
//
// Host-portable: no Arduino, no MeshCore. No heap and no recursion; both directions are bounded by
// the buffers they are given.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace twinoak {
namespace link {

// Encodes len bytes into out. Returns the number of bytes written, or 0 when they do not fit or a
// run of 254 non-zero bytes was met. The result holds no 0x00, so a caller appends the frame's
// terminator itself. An encoding is one byte longer than its input.
size_t cobsEncode(const uint8_t* raw, size_t len, uint8_t* out, size_t cap);

// Why a frame's COBS could not be decoded. Informative: a receiver has to drop and count the
// frame, not agree on a word (LINK-PROTOCOL.md section 11).
enum class CobsError : uint8_t {
  kNone = 0,
  kEmpty,        // nothing to decode (an empty frame is not an error, but it is not a packet)
  kZeroInside,   // a 0x00 where a code byte was expected: the frame was cut
  kPastEnd,      // a block that runs past the end of the frame
  kExtendedCode, // the 0xFF code of general COBS, which this link never sends
  kNoRoom,       // the decoding does not fit the buffer it was given
};

// Decodes len encoded bytes (the frame without its terminating 0x00) into out. On kNone,
// out_len holds the number of bytes written; on anything else out holds nothing of use.
CobsError cobsDecode(const uint8_t* enc, size_t len, uint8_t* out, size_t cap, size_t& out_len);

}  // namespace link
}  // namespace twinoak
