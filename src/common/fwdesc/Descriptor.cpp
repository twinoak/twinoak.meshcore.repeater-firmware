#include "common/fwdesc/Descriptor.h"

#include <string.h>

namespace twinoak {
namespace fwdesc {
namespace {

// Byte `i` of a fixed field, copied out with its terminator guaranteed. Returns false when the
// field has no zero in it at all, which is a descriptor this reader will not trust: the
// specification asks for "at least one terminating zero".
bool copyField(const uint8_t* src, size_t size, char* out) {
  for (size_t i = 0; i < size; i++) {
    if (src[i] == 0) {
      memcpy(out, src, i);
      out[i] = 0;
      return true;
    }
  }
  return false;
}

}  // namespace

uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) crc = crcByte(crc, data[i]);
  return crc ^ 0xFFFFFFFFu;
}

bool build(uint8_t* out, size_t out_len, uint8_t role, uint8_t slot, uint8_t link_major,
           uint8_t link_minor, const char* version, const char* board, const char* band) {
  if (out == nullptr || out_len != kLength) return false;
  if (version == nullptr || board == nullptr || band == nullptr) return false;
  if (!fits(version, kVersionSize) || !fits(board, kBoardSize) || !fits(band, kBandSize)) {
    return false;
  }
  // byteAt() is the same function the image's own const object is built from at compile time
  // (src/common/fwdesc/ImageDescriptor.cpp), so there is one definition of what a descriptor's
  // bytes are and the CRC is computed one way.
  for (size_t i = 0; i < kLength; i++) {
    out[i] = byteAt(role, slot, link_major, link_minor, version, board, band, i);
  }
  return true;
}

bool parse(const uint8_t* blob, size_t len, Fields& out) {
  if (blob == nullptr || len != kLength) return false;
  if (memcmp(blob, kMagic, kMagicLength) != 0) return false;
  const uint16_t length = static_cast<uint16_t>(blob[8] | (blob[9] << 8));
  if (length != kLength) return false;
  uint32_t want = 0;
  for (size_t i = 0; i < 4; i++) {
    want |= static_cast<uint32_t>(blob[kCrcOffset + i]) << (8 * i);
  }
  if (crc32(blob, kBodyLength) != want) return false;

  Fields fields;
  fields.role = blob[kRoleOffset];
  fields.slot = blob[kSlotOffset];
  fields.link_major = blob[kLinkMajorOffset];
  fields.link_minor = blob[kLinkMinorOffset];
  fields.flags = static_cast<uint16_t>(blob[kFlagsOffset] | (blob[kFlagsOffset + 1] << 8));
  if (!copyField(blob + kVersionOffset, kVersionSize, fields.version)) return false;
  if (!copyField(blob + kBoardOffset, kBoardSize, fields.board)) return false;
  if (!copyField(blob + kBandOffset, kBandSize, fields.band)) return false;
  out = fields;
  return true;
}

}  // namespace fwdesc
}  // namespace twinoak
