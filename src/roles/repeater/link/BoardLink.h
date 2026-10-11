// The box link as this board has it: the one Session, the one SerialTransport, and the handful of
// board facts the session asks for through the interfaces of session/Interfaces.h.
//
// This is the only file of src/common/link/ that knows what a RAK4630 is. src/roles/repeater/main.cpp calls linkBegin()
// once and linkLoop() once; src/common/board/target.cpp reads the three accessors at the bottom for the
// `link.errors` and `walter.seen` settings (docs/ARCHITECTURE.md section 4).

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "roles/repeater/app/TwinOakMesh.h"  // the node's identity, its advert name, and the mesh client's job
#include "roles/repeater/meshclient/ObservationRing.h"

namespace twinoak {
namespace link {

// Serial1 up, EV_BOOT queued. After radio_init() and the_mesh.begin(), because the boot
// identifier is drawn from the radio's RNG and the advert name comes from the loaded preferences.
void linkBegin(TwinOakMesh& mesh);

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

// --- observations (spec/LINK-PROTOCOL.md section 7) -------------------------------------------

// One advert this node heard, after its signature verified: into the ring, and section 8's
// reason 5 raised when the ring has just reached three quarters full while `obs.wake` is on and
// the Walter is believed asleep. src/roles/repeater/app/TwinOakMesh.cpp's onAdvertRecv is the only caller; the
// rules are src/roles/repeater/meshclient/LinkObservations.h's and the hourly limit is the attention machine's.
void noteAdvert(const meshclient::HeardAdvert& heard);

// The `obs.wake` setting (src/roles/repeater/app/Settings.h), off at boot: whether a filling ring may ask for
// the Walter at all.
bool obsWakeEnabled();
void setObsWakeEnabled(bool on);

// --- the mesh client's ceiling (tasks/phase-2/04-the-rails.md) ---------------------------------

// Sends the mesh client's ceiling has refused since boot, which is what `sensor get
// client.refused` answers (src/roles/repeater/meshclient/SendCeiling.h). 0 before the client exists.
uint32_t clientRefused();

// --- what src/common/board/target.cpp reads for the settings -----------------------------------------

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
