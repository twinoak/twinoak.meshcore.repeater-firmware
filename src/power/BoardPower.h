// Power and environment as this board has them: the one INA3221 reader, the one BME280 reader,
// the battery thresholds and their preferences file, and the handful of things the rest of the
// firmware asks of them (docs/ARCHITECTURE.md section 6).
//
// This is the only file of src/power/ that knows what a RAK4630 is. The calls in and out:
//
//   src/board/target.cpp   TwinOakSensors::begin()/loop() drive begin()/loop();
//                          getBattMilliVolts() answers batteryMilliVolts();
//                          querySensors() adds appendOurFields() to upstream's CayenneLPP;
//                          the two settings read and write the thresholds
//   src/link/BoardLink.cpp linkPower() is the session's PowerAccess, EnvAccess and LppAccess
//
// Nothing here is called from a request handler: the only two calls into the I2C bus are inside
// loop(), which is why a request on the box link never performs one (src/power/LinkPower.h).

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../app/UpstreamMyMesh.h"  // FILESYSTEM, CayenneLPP - as system headers
#include "LinkPower.h"
#include "Telemetry.h"

namespace twinoak {
namespace power {

// Probes both parts, writes the INA3221's configuration and restores the saved thresholds.
// Called from TwinOakSensors::begin(), which src/main.cpp calls after board.begin() has brought
// Wire up at 100 kHz and after InternalFS.begin().
void begin(FILESYSTEM& fs);

// One bounded pass, once per loop(): at most one INA3221 channel and at most one BME280 sample,
// and the battery thresholds looked at with whatever channel 2 last said.
void loop();

// --- what the board answers --------------------------------------------------------------------

// Channel 2's bus voltage, or 0 when that channel is not valid: TwinOakBoard::getBattMilliVolts()
// (src/board/target.h), and through it upstream's `status`, the telemetry base channel and
// upstream's own boot-voltage protection.
uint16_t batteryMilliVolts();

// The session's PowerAccess, EnvAccess and LppAccess, all three of them this one object.
LinkPower& linkPower();

// Adds this unit's own telemetry to upstream's buffer - the three INA3221 channels' voltage and
// current and the enclosure's temperature, on the channels of src/power/Telemetry.h - and
// returns how many fields it added. Called from TwinOakSensors::querySensors() after upstream's
// manager has had its turn.
size_t appendOurFields(CayenneLPP& telemetry, uint8_t perm_mask);

// --- the two settings (src/app/Settings.h) ------------------------------------------------------

uint16_t batteryLowMilliVolts();
uint16_t batteryCritMilliVolts();
// Sets one of the pair - the other keeps its value - and writes the preferences file. False when
// the pair that would result is not one this firmware accepts (src/power/BatteryWatch.h), and
// nothing is changed then.
bool setBatteryLowMilliVolts(uint16_t mv);
bool setBatteryCritMilliVolts(uint16_t mv);

}  // namespace power
}  // namespace twinoak
