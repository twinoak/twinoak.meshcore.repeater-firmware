#include "FrameReceiver.h"

#include "Cobs.h"

namespace twinoak {
namespace link {

size_t FrameReceiver::feed(const uint8_t* data, size_t len) {
  has_packet_ = false;
  if (data == nullptr) return len;
  for (size_t i = 0; i < len; i++) {
    if (feedByte(data[i])) return i + 1;
  }
  return len;
}

bool FrameReceiver::feedByte(uint8_t b) {
  has_packet_ = false;
  if (b != 0) {
    if (held_ < kReceiveBuffer) {
      frame_[held_++] = b;
    } else {
      // The frame is already longer than any frame of this protocol. Keep swallowing it: the drop
      // is counted when its terminator arrives (LINK-PROTOCOL.md section 2).
      overflowed_ = true;
    }
    return false;
  }
  const bool complete = finishFrame();
  held_ = 0;
  overflowed_ = false;
  return complete;
}

bool FrameReceiver::finishFrame() {
  if (overflowed_) {
    countDrop(Drop::kOverflow);
    return false;
  }
  if (held_ == 0) return false;  // two terminators in a row, or garbage already ended: not an error

  size_t decoded = 0;
  if (cobsDecode(frame_, held_, packet_bytes_, sizeof(packet_bytes_), decoded) != CobsError::kNone) {
    countDrop(Drop::kCobs);
    return false;
  }
  switch (parsePacket(packet_bytes_, decoded, packet_)) {
    case PacketError::kNone:
      has_packet_ = true;
      return true;
    case PacketError::kShort:
      countDrop(Drop::kShort);
      return false;
    case PacketError::kOversize:
      countDrop(Drop::kOversize);
      return false;
    case PacketError::kCrc:
    default:
      countDrop(Drop::kCrc);
      return false;
  }
}

void FrameReceiver::countDrop(Drop reason) {
  last_drop_ = reason;
  if (drops_ < 0xFFFF) drops_++;
  uint16_t& n = by_reason_[size_t(reason)];
  if (n < 0xFFFF) n++;
}

uint16_t FrameReceiver::drops(Drop reason) const {
  if (reason >= Drop::kCount) return 0;
  return by_reason_[size_t(reason)];
}

void FrameReceiver::reset() {
  held_ = 0;
  overflowed_ = false;
  has_packet_ = false;
  packet_ = ParsedPacket();
}

}  // namespace link
}  // namespace twinoak
