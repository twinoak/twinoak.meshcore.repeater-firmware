// The boards this firmware is built for, and the radio position each one is.
//
// The Walter addresses radios by **slot** (spec/SYSTEM.md S-11): there is one slot today,
// because the adapter that was ordered carries one RAK4630. Two numbers follow from the board
// and from nothing else, so they are a table row here rather than a define somebody could set
// wrong:
//
//   `slot`             what the running firmware reports in the box link's `HELLO` - the radio
//                      position this unit *is* (spec/LINK-PROTOCOL.md 5.1): 0 on a single-radio
//                      unit, and the position on a dual adapter
//   `descriptor_slot`  what the image says about itself (spec/FIRMWARE-MANIFEST.md section 4):
//                      0xFF when the image may go in any position, otherwise the only position
//                      it is for. A single-RAK image is for any position; the dual adapter's two
//                      images will not be
//
// Portable: no Arduino, no MeshCore, so the native tests hold it against platformio.ini
// (test/test_image).
//
// **Adding a board** (docs/ARCHITECTURE.md section 8): a row here, a `[img_<role>_<board>]`
// section and an `[env:<role>_<board>]` in platformio.ini, and - if the pin map differs - a
// board target beside src/common/board/target.cpp.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace twinoak {
namespace board {

// 0xFF in the descriptor's `slot`: this image is for any radio position.
constexpr uint8_t kSlotAny = 0xFF;

struct BoardRow {
  const char* board;  // as spec/FIRMWARE-MANIFEST.md spells it, with hyphens
  uint8_t slot;
  uint8_t descriptor_slot;
};

constexpr BoardRow kBoards[] = {
    // The single-RAK adapter that was ordered: one radio, position 0, and an image of it is not
    // tied to a position.
    {"rak4630-single", 0, kSlotAny},
};
constexpr size_t kBoardCount = sizeof(kBoards) / sizeof(kBoards[0]);

// A constexpr strcmp: C++11 allows one return statement and recursion.
constexpr bool sameName(const char* a, const char* b) {
  return (*a == *b) && (*a == '\0' || sameName(a + 1, b + 1));
}

// The row for `board`, or -1 when the table has none.
constexpr int indexOf(const BoardRow* rows, size_t count, const char* board, size_t i = 0) {
  return i >= count ? -1
                    : (sameName(rows[i].board, board) ? static_cast<int>(i)
                                                      : indexOf(rows, count, board, i + 1));
}

// The radio position `HELLO` reports, and the descriptor's `slot`. Both answer for a board the
// table does not know with the safest value there is - slot 0 and "any position" would both be
// claims; `kSlotAny` as a position is not a position, and an unknown board fails the
// static_assert in src/common/fwdesc/ImageDescriptor.cpp before either is ever read.
constexpr uint8_t slotOf(const BoardRow* rows, size_t count, const char* board) {
  return indexOf(rows, count, board) < 0 ? kSlotAny
                                         : rows[indexOf(rows, count, board)].slot;
}

constexpr uint8_t descriptorSlotOf(const BoardRow* rows, size_t count, const char* board) {
  return indexOf(rows, count, board) < 0 ? kSlotAny
                                         : rows[indexOf(rows, count, board)].descriptor_slot;
}

}  // namespace board
}  // namespace twinoak
