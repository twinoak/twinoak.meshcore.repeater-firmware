// The 96 bytes an image carries about itself: spec/FIRMWARE-MANIFEST.md section 4, as code.
//
// A manifest describes files and a file can be renamed or mixed up, so every image also says,
// inside itself, what it is - role, slot, the box-link version it speaks, its own version, the
// board and the band - with a CRC-32 over the first 92 bytes. The Walter reads those bytes out
// of what it is about to flash and refuses the image if they are not what the manifest said
// (spec/FIRMWARE-MANIFEST.md section 5, step 5).
//
//   | Offset | Size | Field                                                    |
//   |-------:|-----:|----------------------------------------------------------|
//   |      0 |    8 | magic, ASCII `TWOKFWD1`                                   |
//   |      8 |    2 | length, 96, little-endian                                 |
//   |     10 |    1 | role (0 unknown, 1 repeater, 2 bridge, 3 scanner, 16 manager) |
//   |     11 |    1 | slot, 0xFF = any                                          |
//   |     12 |    2 | link major, link minor                                    |
//   |     14 |    2 | flags, 0                                                  |
//   |     16 |   32 | version, ASCII, zero-padded, at least one terminating zero |
//   |     48 |   24 | board, the same                                           |
//   |     72 |    8 | band, the same                                            |
//   |     80 |   12 | reserved, zero                                            |
//   |     92 |    4 | CRC-32 (IEEE 802.3, as zlib) over bytes 0-91, little-endian |
//
// **One generator, two uses.** `byteAt()` is byte `i` of the descriptor for a set of fields, and
// it is `constexpr`: the image's own 96 bytes are built from it at compile time, as a `const`
// object (src/common/fwdesc/ImageDescriptor.cpp), and the same function filled byte by byte in a
// loop is `build()` below, which the native tests hold against the specification's fixtures
// (`spec/fixtures/manifest/index.json`, `descriptors[*].hex`). So nothing computes the CRC twice
// in two ways, and the bytes in the image are the bytes the fixtures describe.
//
// Portable: no Arduino, no MeshCore, C++11 (the board's standard is the Adafruit core's
// gnu++11), compiled by [env:native] beside its tests.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace twinoak {
namespace fwdesc {

constexpr size_t kLength = 96;
constexpr size_t kBodyLength = 92;  // what the CRC covers
constexpr size_t kMagicLength = 8;
constexpr const char* kMagic = "TWOKFWD1";

// The offsets and sizes of the table above, so that nothing indexes the 96 bytes by eye.
constexpr size_t kRoleOffset = 10;
constexpr size_t kSlotOffset = 11;
constexpr size_t kLinkMajorOffset = 12;
constexpr size_t kLinkMinorOffset = 13;
constexpr size_t kFlagsOffset = 14;
constexpr size_t kVersionOffset = 16;
constexpr size_t kVersionSize = 32;
constexpr size_t kBoardOffset = 48;
constexpr size_t kBoardSize = 24;
constexpr size_t kBandOffset = 72;
constexpr size_t kBandSize = 8;
constexpr size_t kCrcOffset = 92;

// The role codes of spec/FIRMWARE-MANIFEST.md section 4. `manager` is the Walter's and is here
// because the specification's fixtures carry one; this firmware never builds it.
constexpr uint8_t kRoleUnknown = 0;
constexpr uint8_t kRoleRepeater = 1;
constexpr uint8_t kRoleBridge = 2;
constexpr uint8_t kRoleScanner = 3;
constexpr uint8_t kRoleManager = 16;

constexpr uint8_t kSlotAny = 0xFF;

// A constexpr strcmp and strlen: C++11 allows one return statement and recursion.
constexpr bool sameName(const char* a, const char* b) {
  return (*a == *b) && (*a == '\0' || sameName(a + 1, b + 1));
}

constexpr size_t textLength(const char* s) { return *s == '\0' ? 0 : 1 + textLength(s + 1); }

// Whether `s` fits a fixed field of `size` bytes **with its terminating zero**: the field is
// zero-padded and the specification asks for at least one terminating zero, so a string of
// exactly `size` characters does not fit.
constexpr bool fits(const char* s, size_t size) { return textLength(s) < size; }

// The role name of spec/FIRMWARE-MANIFEST.md section 2 as its code, or kRoleUnknown. The names
// are the manifest's own, so platformio.ini's TWINOAK_ROLE is spelled exactly as a manifest
// entry's `role`.
constexpr uint8_t roleCode(const char* role) {
  return sameName(role, "repeater")  ? kRoleRepeater
         : sameName(role, "bridge")  ? kRoleBridge
         : sameName(role, "scanner") ? kRoleScanner
         : sameName(role, "manager") ? kRoleManager
                                     : kRoleUnknown;
}

// --- the CRC ------------------------------------------------------------------------------

// CRC-32 as in IEEE 802.3 and zlib: the reflected polynomial, initial and final complement.
// Eight steps for one byte, written as recursion so that it is a C++11 constant expression.
constexpr uint32_t crcStep(uint32_t crc, int bit) {
  return bit == 0 ? crc
                  : crcStep((crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1), bit - 1);
}

constexpr uint32_t crcByte(uint32_t crc, uint8_t value) {
  return crcStep(crc ^ static_cast<uint32_t>(value), 8);
}

// --- the bytes ----------------------------------------------------------------------------

// Byte `i` of a fixed, zero-padded ASCII field holding `s`.
constexpr uint8_t textByte(const char* s, size_t i) {
  return i < textLength(s) ? static_cast<uint8_t>(s[i]) : uint8_t(0);
}

// Byte `i` of the 92 bytes the CRC covers.
constexpr uint8_t bodyByte(uint8_t role, uint8_t slot, uint8_t link_major, uint8_t link_minor,
                           const char* version, const char* board, const char* band, size_t i) {
  return i < kMagicLength            ? static_cast<uint8_t>(kMagic[i])
         : i == 8                    ? static_cast<uint8_t>(kLength & 0xFF)
         : i == 9                    ? static_cast<uint8_t>((kLength >> 8) & 0xFF)
         : i == kRoleOffset          ? role
         : i == kSlotOffset          ? slot
         : i == kLinkMajorOffset     ? link_major
         : i == kLinkMinorOffset     ? link_minor
         : i < kVersionOffset        ? uint8_t(0)  // flags
         : i < kBoardOffset          ? textByte(version, i - kVersionOffset)
         : i < kBandOffset           ? textByte(board, i - kBoardOffset)
         : i < kBandOffset + kBandSize ? textByte(band, i - kBandOffset)
                                     : uint8_t(0);  // reserved
}

// The CRC over those 92 bytes.
constexpr uint32_t bodyCrc(uint8_t role, uint8_t slot, uint8_t link_major, uint8_t link_minor,
                           const char* version, const char* board, const char* band,
                           size_t i = 0, uint32_t crc = 0xFFFFFFFFu) {
  return i >= kBodyLength
             ? (crc ^ 0xFFFFFFFFu)
             : bodyCrc(role, slot, link_major, link_minor, version, board, band, i + 1,
                       crcByte(crc, bodyByte(role, slot, link_major, link_minor, version, board,
                                             band, i)));
}

// Byte `i` of the whole 96: the body, then the CRC little-endian.
constexpr uint8_t byteAt(uint8_t role, uint8_t slot, uint8_t link_major, uint8_t link_minor,
                         const char* version, const char* board, const char* band, size_t i) {
  return i < kBodyLength
             ? bodyByte(role, slot, link_major, link_minor, version, board, band, i)
             : static_cast<uint8_t>(
                   (bodyCrc(role, slot, link_major, link_minor, version, board, band) >>
                    (8 * (i - kBodyLength))) &
                   0xFF);
}

// --- at run time ----------------------------------------------------------------------------

// What a descriptor says. `version`, `board` and `band` are NUL-terminated copies, so a field
// with no terminator inside it - which `parse()` refuses - cannot be read past.
struct Fields {
  uint8_t role = kRoleUnknown;
  uint8_t slot = kSlotAny;
  uint8_t link_major = 0;
  uint8_t link_minor = 0;
  uint16_t flags = 0;
  char version[kVersionSize] = {0};
  char board[kBoardSize] = {0};
  char band[kBandSize] = {0};
};

// The 96 bytes for these fields, into `out`. False - and `out` untouched - when a string does
// not fit its field with a terminating zero, or when `out` is not kLength bytes.
bool build(uint8_t* out, size_t out_len, uint8_t role, uint8_t slot, uint8_t link_major,
           uint8_t link_minor, const char* version, const char* board, const char* band);

// The CRC-32 of the first 92 bytes of `blob`, at run time (the same arithmetic as crcByte).
uint32_t crc32(const uint8_t* data, size_t len);

// Reads 96 bytes. False when the magic, the length, the CRC or a missing terminating zero says
// this is not a descriptor.
bool parse(const uint8_t* blob, size_t len, Fields& out);

}  // namespace fwdesc
}  // namespace twinoak
