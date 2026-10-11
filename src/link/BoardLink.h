// The box link as this board has it: the one Session, the one SerialTransport, and the handful of
// board facts the session asks for through the interfaces of session/Interfaces.h.
//
// This is the only file of src/link/ that knows what a RAK4630 is. src/main.cpp calls linkBegin()
// once and linkLoop() once; src/board/target.cpp reads the three accessors at the bottom for the
// `link.errors` and `walter.seen` settings (docs/ARCHITECTURE.md section 4).

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../app/UpstreamMyMesh.h"  // MyMesh: the node's identity and its advert name

namespace twinoak {
namespace link {

// Serial1 up, EV_BOOT queued. After radio_init() and the_mesh.begin(), because the boot
// identifier is drawn from the radio's RNG and the advert name comes from the loaded preferences.
void linkBegin(MyMesh& mesh);

// One bounded pass of the link. Once per loop().
void linkLoop();

// --- attention (spec/LINK-PROTOCOL.md section 8) -----------------------------------------------

// Ask for the Walter's attention. `reason` is one of Numbers.h's AttentionReason values and
// `detail` is that reason's own number. False when the machine refused it: this reason is already
// pending or queued, or it was raised less than an hour ago. Task 06 (the battery) and phase 2
// (the observation ring, a fault) raise theirs through this one call.
bool raiseAttention(uint8_t reason, uint16_t detail);
// `sensor set walter.wake 1`, from the USB console or from a mesh admin: attention reason 4,
// "a person asked for the Walter".
bool raiseWalterWake();
// Whether an attention is pending, which is what `sensor get walter.wake` and STATUS's
// `attention` field answer.
bool attentionPending();
// Whether the Walter is believed asleep (SLEEP_NOTICE, STATUS flag bit 2).
bool walterAsleep();

// --- what src/board/target.cpp reads for the settings -----------------------------------------

// Frames the link's receiver dropped since boot, saturating (`link.errors`).
uint16_t linkErrors();
// Whether a valid request has ever arrived on the link, and how long ago (`walter.seen`).
bool walterEverSeen();
uint32_t walterSeenSecondsAgo();
// The worst time one pass of the link has taken out of loop(), in microseconds
// (tasks/phase-1/03-the-link-session.md; docs/bench.md section 2).
uint32_t linkWorstPassMicros();

}  // namespace link
}  // namespace twinoak
