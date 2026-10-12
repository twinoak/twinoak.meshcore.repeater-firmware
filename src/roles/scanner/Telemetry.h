// The two parts on the I2C bus that are not the radio: the RV-3028-C7 clock on the RAK adapter
// and the INA3221 on connector board B.
//
// NoiseScope's telemetry.{h,cpp} (software/firmware/noisescope-rak4630/ at b798aab0 of the
// hardware repository), with the INA3221 half replaced by src/common/power/: the address, the
// three shunt values, the register arithmetic and the reader's schedule are the repeater's own
// (src/common/power/Ina3221.h, PowerReader.h, WireBus.h), which is the point of the move - a
// `pwr` line of the scanner and a POWER_GET of the repeater are now the same numbers from the
// same code, and the bench holds one against the other (docs/bench.md section 9).
//
// Both parts are optional: everything degrades to a missing reading when one is absent, so the
// same image runs on a bare RAK4631 on the bench.
//
// **Nothing here writes flash.** The clock is set in RAM and, when the part is there, in the
// RV-3028's own UNIX-time counter over I2C - which is what makes a repeater-scanner-repeater
// round trip safe: the node's identity and preferences are in the internal file system and this
// image never opens it (test/test_rails).

#pragma once

#include <stdint.h>

#include "roles/scanner/Lines.h"

namespace twinoak {
namespace scanner {
namespace telemetry {

// Brings `Wire` up at the bus's 100 kHz, probes both parts and configures the INA3221. True when
// at least one of them answered.
bool begin();

// One bounded pass of the INA3221 reader, once per loop(): at most one channel, and nothing at
// all while the device is in its back-off (src/common/power/PowerReader.h). A `stat` line
// reports what the reader last read and never waits on the bus for it.
void loop();

bool rtcPresent();
bool inaPresent();

// Unix time, from the RV-3028's counter when the part is there and from a `time <epoch>` command
// offset by millis() otherwise. 0 when it has never been set, which is what a reader re-stamps
// (docs/noisescope.md).
uint32_t epoch();

// Sets the clock, and writes the RV-3028 when the part is there. False when that write failed.
bool setEpoch(uint32_t epoch);

// The three channels as a `stat` line reports them, from the reader's cache.
PowerFields power();

}  // namespace telemetry
}  // namespace scanner
}  // namespace twinoak
