// The nRF52's RESETREAS register to the reset-reason codes of the box link
// (spec/LINK-PROTOCOL.md section 5.3): a pure function, so it is testable without a board.
//
// Host-portable: no Arduino, no MeshCore, no nRF headers - the bit masks are written out, with
// the register's own names beside them, because this file is compiled by [env:native] too.
// Who reads the register is the board's business (src/link/BoardLink.cpp).

#pragma once

#include <stdint.h>

namespace twinoak {
namespace link {

// The nRF52840's POWER->RESETREAS bits, as the Nordic product specification numbers them. The
// SoftDevice's nrf_soc.h and the Arduino core's wiring.c read the same register; MeshCore captures
// it in a constructor of priority 101 before anything clears it
// (src/helpers/NRF52Board.cpp, nrf52_early_reset_capture, pin d9296435).
enum Nrf52ResetReas : uint32_t {
  kReasResetPin = 0x00000001,  // RESETPIN
  kReasDog = 0x00000002,       // DOG, the watchdog
  kReasSoftReset = 0x00000004,  // SREQ, NVIC_SystemReset
  kReasLockup = 0x00000008,    // LOCKUP
  kReasOff = 0x00010000,       // OFF, woke from System OFF by a GPIO DETECT
  kReasLpComp = 0x00020000,    // LPCOMP, woke from System OFF by ANADETECT
  kReasDif = 0x00040000,       // DIF, woke from System OFF by the debug interface
  kReasNfc = 0x00080000,       // NFC, woke from System OFF by an NFC field
  kReasVbus = 0x00100000,      // VBUS, woke from System OFF by VBUS rising
};

// Maps RESETREAS to one of Numbers.h's ResetReason values. Several bits can be set at once - the
// register accumulates until it is cleared - so the order below is the order of what a reader
// most wants to know: a watchdog or a lockup before the soft reset that may have followed it, and
// a wake from System OFF before the pin that caused it.
//
// kResetBootloader (7, "left the bootloader") is never returned: the nRF52 has no bit for it. The
// UF2 bootloader restarts the chip when it is done, so a boot after a DFU reads as kResetSoftware
// - which is literally what happened.
uint8_t resetReasonFromNrf52(uint32_t resetreas);

}  // namespace link
}  // namespace twinoak
