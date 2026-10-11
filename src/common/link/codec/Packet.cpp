#include "common/link/codec/Packet.h"

#include "common/link/codec/Crc16.h"
#include "common/link/codec/Numbers.h"

namespace twinoak {
namespace link {

size_t buildPacket(uint8_t type, uint8_t seq, const uint8_t* body, size_t body_len, uint8_t* out,
                   size_t cap) {
  if (body_len > kMaxPayload) return 0;
  Writer w(out, cap);
  w.u8(type);
  w.u8(seq);
  w.raw(body, body_len);
  if (!w.ok()) return 0;
  w.u16(crc16(out, w.size()));
  return w.ok() ? w.size() : 0;
}

PacketError parsePacket(const uint8_t* packet, size_t len, ParsedPacket& out) {
  if (packet == nullptr || len < kMinPacket) return PacketError::kShort;
  if (len > kMaxPacket) return PacketError::kOversize;
  const size_t body_len = len - 2;
  const uint16_t said = uint16_t(uint16_t(packet[len - 1]) << 8 | packet[len - 2]);
  if (crc16(packet, body_len) != said) return PacketError::kCrc;
  out.type = packet[0];
  out.seq = packet[1];
  out.body = bytesOf(packet + 2, body_len - 2);
  return PacketError::kNone;
}

}  // namespace link
}  // namespace twinoak
