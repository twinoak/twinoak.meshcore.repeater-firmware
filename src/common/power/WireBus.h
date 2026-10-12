// The I2C master of src/common/power/PowerReader.h, over the Arduino core's `Wire`.
//
// It is the board half of the INA3221 reader, and it is in src/common/ rather than in a role's
// directory because both roles of this board read that part through it: the repeater's
// src/common/power/BoardPower.cpp, where it was written (phase 1's task 06), and the scanner's
// src/roles/scanner/Telemetry.cpp, whose own copy of the same arithmetic it replaced (phase 3's
// task 02). Nothing else of this file's include - the core's `Wire` - reaches MeshCore, so a
// role that compiles no MeshCore can use it (src/common/board/CoreHeaders.h).
//
// **What this cannot do, and the reader's back-off is why it does not have to.** The Adafruit
// core's TWIM driver busy-waits on the peripheral's event registers with no deadline
// (libraries/Wire/Wire_nRF52.cpp:166, :169, :230, :234 at the core's pinned commit d541301:
// `while(!_p_twim->EVENTS_RXSTARTED && !_p_twim->EVENTS_ERROR);`). A device that is absent or
// unpowered makes the hardware raise ERROR on the address NACK and the call returns at once,
// which is the failure this board actually meets; a bus held low by a stuck slave would hang
// that loop, and nothing short of our own TWIM driver could bound it. So what is measured here
// is how long the transaction *took*, the reader gives up on a device whose transactions go past
// its budget, and tasks/phase-1/reports/06.md records the residual risk rather than claiming a
// timeout this layer cannot enforce.

#pragma once

#include <stdint.h>

#include "common/power/PowerReader.h"

namespace twinoak {
namespace power {

// One I2C master over `Wire`, at the 100 kHz the board pinned it to (one set of 1 k pull-ups
// through a 1000 pF bulkhead filter, INTERCONNECT.md). Whoever owns it has already brought
// `Wire` up: TwinOakBoard::begin() for the repeater, setup() for the scanner.
class WireBus : public I2cBus {
 public:
  bool writeRegister(uint8_t address, uint8_t reg, uint16_t value) override;
  bool readRegister(uint8_t address, uint8_t reg, uint16_t& value) override;
  uint32_t lastTransactionMicros() const override { return last_us_; }

 private:
  uint32_t last_us_ = 0;
};

}  // namespace power
}  // namespace twinoak
