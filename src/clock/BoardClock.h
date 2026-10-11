// The clock keeper as this board has it: the one ClockKeeper, the ports it reaches the RAK4630
// through, and the handful of calls the rest of the firmware makes.
//
// This is the only file of src/clock/ that knows what a RAK4630 is. src/main.cpp calls begin()
// once and loop() once and sends the boot advert when bootAdvertDue() says so; src/board/
// target.cpp reads stateName() for the `clock.state` setting and calls saveBeforeRestart() from
// TwinOakBoard::reboot(); src/link/BoardLink.cpp hands keeper() to the session through
// LinkClock (docs/ARCHITECTURE.md section 5).

#pragma once

#include <stdint.h>

#include "../app/UpstreamMyMesh.h"  // FILESYSTEM, and the clock helpers through <target.h>
#include "ClockKeeper.h"

namespace twinoak {
namespace clock {

// Decides what the time is and sets upstream's clock to it (ClockKeeper::begin). Called from
// setup() after radio_init() - which is where rtc_clock.begin(Wire) probes the I2C bus for a
// hardware RTC - and before the_mesh.begin(), so that nothing of upstream's can record a
// timestamp from the 2024 default first.
void begin(FILESYSTEM& fs);

// One pass. Once per loop(), before the mesh's own, so that a clock the console just set is
// noticed in the same pass.
void loop();

// The keeper itself, for src/link/BoardLink.cpp's LinkClock.
ClockKeeper& keeper();

// --- the boot advert, which src/main.cpp sends --------------------------------------------------

// True once the hold has lifted: the clock is set, or 60 seconds have passed.
bool bootAdvertDue();
// src/main.cpp has sent it.
void noteBootAdvertSent();

// --- what the application tells the keeper ------------------------------------------------------

// Something carrying this node's clock has gone, or may have gone, on the air
// (src/app/TwinOakMesh.cpp lists the places and why each is one).
void noteTimestampEmitted();

// Both memories, now, because a restart this firmware asked for is about to happen
// (src/board/target.cpp's reboot()/powerOff(), src/link/BoardLink.cpp's restartNow()).
void saveBeforeRestart();

// --- the `clock.state` setting (docs/ARCHITECTURE.md section 4) ---------------------------------

// "unset", "restored", "set" or "rtc".
const char* stateName();

}  // namespace clock
}  // namespace twinoak
