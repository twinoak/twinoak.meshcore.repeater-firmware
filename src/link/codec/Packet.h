// A packet of the box link: type, seq, 0 to 240 bytes of body, and a CRC-16/CCITT-FALSE over the
// three (LINK-PROTOCOL.md section 2). Building one and checking one.
//
// Host-portable: no Arduino, no MeshCore. The framing around a packet - COBS and the terminating
// 0x00 - is FrameWriter's and FrameReceiver's.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Bytes.h"

namespace twinoak {
namespace link {

// What one packet says. The view points into the buffer the packet was parsed in.
struct ParsedPacket {
  uint8_t type = 0;
  uint8_t seq = 0;
  Bytes body;
};

// Why a packet was refused. Informative, as LINK-PROTOCOL.md section 11 has it: a receiver drops
// and counts it, and the word is nobody's business but a log's.
enum class PacketError : uint8_t {
  kNone = 0,
  kShort,     // under four bytes: no room for a type, a seq and a CRC
  kOversize,  // over 244 bytes
  kCrc,       // the CRC does not match the bytes before it
};

// Builds type, seq, the body and the CRC into out. Returns the packet's length (4 + body_len), or
// 0 when the body is over 240 bytes or out is too small.
size_t buildPacket(uint8_t type, uint8_t seq, const uint8_t* body, size_t body_len, uint8_t* out,
                   size_t cap);

// Checks one packet's length and its CRC, and on kNone fills out. Nothing of a packet is
// interpreted before its CRC has passed (LINK-PROTOCOL.md section 2).
PacketError parsePacket(const uint8_t* packet, size_t len, ParsedPacket& out);

}  // namespace link
}  // namespace twinoak
