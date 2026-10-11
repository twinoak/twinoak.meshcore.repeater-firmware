#include "FrameWriter.h"

#include "Cobs.h"
#include "Packet.h"

namespace twinoak {
namespace link {

bool FrameWriter::build(uint8_t type, uint8_t seq, const uint8_t* body, size_t body_len,
                        bool line_idle) {
  size_ = 0;
  uint8_t packet[kMaxPacket];
  const size_t packet_len = buildPacket(type, seq, body, body_len, packet, sizeof(packet));
  if (packet_len == 0) return false;

  size_t at = 0;
  if (line_idle) buf_[at++] = 0x00;
  const size_t encoded = cobsEncode(packet, packet_len, buf_ + at, sizeof(buf_) - at - 1);
  if (encoded == 0) return false;
  at += encoded;
  buf_[at++] = 0x00;  // the terminator, the only 0x00 COBS leaves on the wire
  size_ = at;
  return true;
}

}  // namespace link
}  // namespace twinoak
