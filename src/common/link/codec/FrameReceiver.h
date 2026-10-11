// The receiving end of the box link's framing: bytes in, packets out, and a count of what was
// dropped (LINK-PROTOCOL.md section 2, "Rules for a receiver").
//
//   - Bytes are collected until 0x00. Nothing collected: ignored - an empty frame is not an error.
//   - Otherwise the frame is COBS-decoded and its length (4..244) and CRC checked. A frame that
//     fails any of these is dropped and counted, and nothing is sent in reply: the sender's
//     timeout is the recovery.
//   - The buffer is 246 bytes. A frame that overflows it is dropped and counted when its
//     terminator arrives - not before, so that the bytes of the rest of it cannot be taken for a
//     frame of their own.
//   - Nothing of a frame is interpreted before its CRC has passed.
//
// Host-portable: no Arduino, no MeshCore. No heap: the two buffers are the object's own, and the
// object is meant to live as long as the link does. It may be fed one byte or many, in any
// chunking; `feed` stops after the byte that completed a packet, so a caller loops:
//
//     while (len > 0) {
//       const size_t used = rx.feed(data, len);
//       if (rx.hasPacket()) handle(rx.packet());
//       data += used; len -= used;
//     }
//
// The packet's body points into the receiver's own buffer and is valid until the next feed.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "common/link/codec/Numbers.h"
#include "common/link/codec/Packet.h"

namespace twinoak {
namespace link {

class FrameReceiver {
 public:
  // Why a frame was dropped. The reason is for a counter and a log, never for a reply.
  enum class Drop : uint8_t {
    kCobs = 0,   // the frame's COBS could not be decoded
    kShort,      // under four bytes of packet
    kOversize,   // over 244 bytes of packet
    kCrc,        // the CRC did not match
    kOverflow,   // more bytes arrived than the 246-byte buffer holds
    kCount,      // how many reasons there are; not a reason
  };

  // Feeds up to len bytes and returns how many were taken: all of them, or everything up to and
  // including the terminator that completed a packet. Ask hasPacket() afterwards.
  size_t feed(const uint8_t* data, size_t len);

  // Feeds one byte. Returns true when a complete, checked packet is now in packet().
  bool feedByte(uint8_t b);

  bool hasPacket() const { return has_packet_; }
  const ParsedPacket& packet() const { return packet_; }

  // Frames dropped since the object was made, saturating at 65535 (STATUS's link_errors,
  // LINK-PROTOCOL.md section 5.3).
  uint16_t drops() const { return drops_; }
  // The same, by reason. Saturating too.
  uint16_t drops(Drop reason) const;
  // Why the last drop happened. Meaningless until drops() is non-zero.
  Drop lastDrop() const { return last_drop_; }

  // How many bytes of the frame being collected are in the buffer.
  size_t pending() const { return held_; }

  // Forgets a half-collected frame and any packet on hand; keeps the counters. For a transport
  // that has reason to believe the line restarted.
  void reset();

  // reset(), and every byte of both buffers overwritten with zero through a volatile pointer, so
  // that the optimiser keeps the stores. For the session, after a request whose payload carried a
  // password (LINK-PROTOCOL.md section 6: "wiped from every buffer the RAK copied them to as soon
  // as the packet is built"). The packet on hand is gone with it.
  void wipe();

 private:
  void countDrop(Drop reason);
  // Decodes, checks and publishes what the buffer holds. Called when a terminator arrives.
  bool finishFrame();

  uint8_t frame_[kReceiveBuffer] = {0};   // the encoded bytes, terminator excluded
  uint8_t packet_bytes_[kReceiveBuffer] = {0};  // their decoding, before its length is known good
  size_t held_ = 0;
  bool overflowed_ = false;
  bool has_packet_ = false;
  ParsedPacket packet_;
  uint16_t drops_ = 0;
  uint16_t by_reason_[size_t(Drop::kCount)] = {0};
  Drop last_drop_ = Drop::kCobs;
};

}  // namespace link
}  // namespace twinoak
