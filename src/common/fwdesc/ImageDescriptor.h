// This image's own descriptor: the 96 bytes of spec/FIRMWARE-MANIFEST.md section 4 as a `const`
// object inside the firmware, and the handful of readers of it.
//
// The object is built at compile time from the environment's own names - TWINOAK_ROLE,
// TWINOAK_BOARD, TWINOAK_BAND and TWINOAK_FW_VERSION, which platformio.ini takes from one
// `[img_*]` section per image - so the bytes in the flash, the manifest entry the release writes
// and what the running firmware reports in the box link's `HELLO` all come from the same three
// strings.
//
// **The readers read the object, not the defines.** `HELLO`'s `role`, `board`, `fw_version` and
// - since link 1.1 - `band` are a byte and three pointers into those 96 bytes
// (src/roles/repeater/link/BoardLink.cpp), which does three things at once: the firmware
// references the descriptor, so the linker keeps it; what the unit says and what the image says
// cannot drift; and it costs no code and no RAM, because the fields are already ASCII and
// already zero-terminated - which the static_asserts beside the object are what guarantee. Parsing a descriptor (CRC and all) is for a *reader of somebody
// else's image* - the release tooling, the Walter, the native tests - and is
// src/common/fwdesc/Descriptor.h's `parse()`.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "common/fwdesc/Descriptor.h"

namespace twinoak {
namespace fwdesc {

// The image's 96 bytes, at a 4-byte boundary, as they lie in the flash.
const uint8_t* imageDescriptor();

// The role code of spec/FIRMWARE-MANIFEST.md section 4, which is also `HELLO`'s `role`
// (spec/LINK-PROTOCOL.md 5.1 gives the same numbers).
uint8_t imageRole();

// This image's version, board and band: pointers into the descriptor itself.
const char* imageVersion();
const char* imageBoard();
const char* imageBand();

// Which radio positions this image may be put in: 0xFF for any (the descriptor's `slot`).
uint8_t imageDescriptorSlot();

// The radio position this unit *is* - `HELLO`'s `slot` (spec/LINK-PROTOCOL.md 5.1), which is the
// board's and not the image's (src/common/board/Boards.h).
uint8_t imageSlot();

}  // namespace fwdesc
}  // namespace twinoak
