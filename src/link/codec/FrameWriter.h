// The sending end of the box link's framing: one packet to the bytes that go on the wire - its
// COBS encoding, the 0x00 that ends it, and the leading 0x00 the sender adds when the line has
// been idle for 100 ms or more (LINK-PROTOCOL.md section 2).
//
// That leading zero ends whatever garbage the receiver may hold and costs nothing, because an
// empty frame is not an error. Whether the line has been idle is the transport's to know; this
// class is told.
//
// Host-portable: no Arduino, no MeshCore, no heap: the frame is built in the object's own 247-byte
// buffer and the transport sends it from there in one piece, never interleaving anything inside it.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Numbers.h"

namespace twinoak {
namespace link {

class FrameWriter {
 public:
  // Builds the frame for one packet. Returns false - and leaves nothing to send - when the body is
  // over 240 bytes. line_idle adds the leading 0x00.
  bool build(uint8_t type, uint8_t seq, const uint8_t* body, size_t body_len, bool line_idle);

  const uint8_t* bytes() const { return buf_; }
  size_t size() const { return size_; }

 private:
  uint8_t buf_[kMaxWrittenFrame] = {0};
  size_t size_ = 0;
};

}  // namespace link
}  // namespace twinoak
