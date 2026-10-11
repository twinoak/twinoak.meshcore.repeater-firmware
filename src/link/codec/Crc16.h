// CRC-16/CCITT-FALSE, the checksum of every packet on the box link.
//
// Polynomial 0x1021, initial value 0xFFFF, no reflection, no final XOR; its check value over the
// ASCII string "123456789" is 0x29B1 (LINK-PROTOCOL.md section 2, and the first vector of
// spec/fixtures/link/link-v1.json). Bit by bit and without a table: a packet is at most 244 bytes,
// 1952 shifts, and a 512-byte table in flash would buy nothing a link that sends a frame every
// few seconds can measure.
//
// Host-portable: no Arduino, no MeshCore.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace twinoak {
namespace link {

uint16_t crc16(const uint8_t* data, size_t len);

}  // namespace link
}  // namespace twinoak
