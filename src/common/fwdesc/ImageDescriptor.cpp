// The image's own 96 bytes, and nothing else.
//
// **The CRC is computed at build time, in the source.** The board's C++ standard is the Adafruit
// nRF52 core's gnu++11 (the platform's builder/frameworks/arduino/adafruit.py), and C++11
// `constexpr` - one return statement and recursion - is enough for both a CRC-32 and the string
// fields, so the 96 bytes below are a compile-time constant and the object in the flash is
// already right. No post-link step patches the image and nothing has to re-check it afterwards:
// a patch would mean the bytes the linker produced and the bytes that ship are not the same
// bytes, and every checksum downstream would be of the second. The generator is
// src/common/fwdesc/Descriptor.h's `byteAt()`, which the portable builder of the same header
// fills a buffer with in a loop, so the two cannot disagree (test/test_image holds both against
// spec/fixtures/manifest/index.json).
//
// The ninety-six `DESC_BYTE(i)` below are written out because C++11 has no way to initialise an
// array from a function of its index; the macro keeps each one to its index alone.

#include "common/fwdesc/ImageDescriptor.h"

#include "common/board/Boards.h"
#include "common/link/codec/Numbers.h"  // kProtoMajor, kProtoMinor: the box link this build speaks
#include "common/version/Version.h"     // TWINOAK_FW_VERSION and its 0.0.0-dev default

#ifndef TWINOAK_BOARD
#error "TWINOAK_BOARD is set by platformio.ini from the environment's [img_*] section"
#endif
#ifndef TWINOAK_BAND
#error "TWINOAK_BAND is set by platformio.ini from the environment's [img_*] section"
#endif

namespace twinoak {
namespace fwdesc {
namespace {

constexpr uint8_t kRole = roleCode(TWINOAK_ROLE);
constexpr uint8_t kDescriptorSlot =
    board::descriptorSlotOf(board::kBoards, board::kBoardCount, TWINOAK_BOARD);

static_assert(kRole != kRoleUnknown,
              "TWINOAK_ROLE is not one of the roles spec/FIRMWARE-MANIFEST.md section 4 gives a "
              "code: repeater, bridge, scanner, manager");
static_assert(board::indexOf(board::kBoards, board::kBoardCount, TWINOAK_BOARD) >= 0,
              "TWINOAK_BOARD has no row in src/common/board/Boards.h: a board is a target and a "
              "table row (docs/ARCHITECTURE.md section 8)");
static_assert(fits(TWINOAK_FW_VERSION, kVersionSize),
              "this build's version does not fit the descriptor's 32-byte field with its "
              "terminating zero (spec/FIRMWARE-MANIFEST.md section 4)");
static_assert(fits(TWINOAK_BOARD, kBoardSize),
              "TWINOAK_BOARD does not fit the descriptor's 24-byte field with its terminating "
              "zero (spec/FIRMWARE-MANIFEST.md section 4)");
static_assert(fits(TWINOAK_BAND, kBandSize),
              "TWINOAK_BAND does not fit the descriptor's 8-byte field with its terminating "
              "zero (spec/FIRMWARE-MANIFEST.md section 4)");

#define DESC_BYTE(i)                                                             \
  byteAt(kRole, kDescriptorSlot, link::kProtoMajor, link::kProtoMinor,           \
         TWINOAK_FW_VERSION, TWINOAK_BOARD, TWINOAK_BAND, (i))

// `used` so the compiler emits it although only imageDescriptor() names it, `aligned(4)` because
// a reader finds a descriptor on a 4-byte boundary (spec/FIRMWARE-MANIFEST.md section 4).
const uint8_t kImageDescriptor[kLength] __attribute__((used, aligned(4))) = {
    DESC_BYTE(0), DESC_BYTE(1), DESC_BYTE(2), DESC_BYTE(3), DESC_BYTE(4), DESC_BYTE(5),
    DESC_BYTE(6), DESC_BYTE(7), DESC_BYTE(8), DESC_BYTE(9), DESC_BYTE(10), DESC_BYTE(11),
    DESC_BYTE(12), DESC_BYTE(13), DESC_BYTE(14), DESC_BYTE(15), DESC_BYTE(16), DESC_BYTE(17),
    DESC_BYTE(18), DESC_BYTE(19), DESC_BYTE(20), DESC_BYTE(21), DESC_BYTE(22), DESC_BYTE(23),
    DESC_BYTE(24), DESC_BYTE(25), DESC_BYTE(26), DESC_BYTE(27), DESC_BYTE(28), DESC_BYTE(29),
    DESC_BYTE(30), DESC_BYTE(31), DESC_BYTE(32), DESC_BYTE(33), DESC_BYTE(34), DESC_BYTE(35),
    DESC_BYTE(36), DESC_BYTE(37), DESC_BYTE(38), DESC_BYTE(39), DESC_BYTE(40), DESC_BYTE(41),
    DESC_BYTE(42), DESC_BYTE(43), DESC_BYTE(44), DESC_BYTE(45), DESC_BYTE(46), DESC_BYTE(47),
    DESC_BYTE(48), DESC_BYTE(49), DESC_BYTE(50), DESC_BYTE(51), DESC_BYTE(52), DESC_BYTE(53),
    DESC_BYTE(54), DESC_BYTE(55), DESC_BYTE(56), DESC_BYTE(57), DESC_BYTE(58), DESC_BYTE(59),
    DESC_BYTE(60), DESC_BYTE(61), DESC_BYTE(62), DESC_BYTE(63), DESC_BYTE(64), DESC_BYTE(65),
    DESC_BYTE(66), DESC_BYTE(67), DESC_BYTE(68), DESC_BYTE(69), DESC_BYTE(70), DESC_BYTE(71),
    DESC_BYTE(72), DESC_BYTE(73), DESC_BYTE(74), DESC_BYTE(75), DESC_BYTE(76), DESC_BYTE(77),
    DESC_BYTE(78), DESC_BYTE(79), DESC_BYTE(80), DESC_BYTE(81), DESC_BYTE(82), DESC_BYTE(83),
    DESC_BYTE(84), DESC_BYTE(85), DESC_BYTE(86), DESC_BYTE(87), DESC_BYTE(88), DESC_BYTE(89),
    DESC_BYTE(90), DESC_BYTE(91), DESC_BYTE(92), DESC_BYTE(93), DESC_BYTE(94), DESC_BYTE(95),
};

#undef DESC_BYTE

}  // namespace

const uint8_t* imageDescriptor() { return kImageDescriptor; }

// A byte and three pointers into the object above. No parsing and no copy: the strings are
// zero-padded ASCII by construction and the static_asserts at the top of this file are what say
// each one has room for its terminator, so a pointer into the field is a C string.
uint8_t imageRole() { return kImageDescriptor[kRoleOffset]; }

uint8_t imageDescriptorSlot() { return kImageDescriptor[kSlotOffset]; }

const char* imageVersion() {
  return reinterpret_cast<const char*>(kImageDescriptor + kVersionOffset);
}

const char* imageBoard() {
  return reinterpret_cast<const char*>(kImageDescriptor + kBoardOffset);
}

const char* imageBand() {
  return reinterpret_cast<const char*>(kImageDescriptor + kBandOffset);
}

uint8_t imageSlot() {
  return board::slotOf(board::kBoards, board::kBoardCount, TWINOAK_BOARD);
}

}  // namespace fwdesc
}  // namespace twinoak
