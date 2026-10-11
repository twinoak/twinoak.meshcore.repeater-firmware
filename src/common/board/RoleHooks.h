// The four things src/common/ asks of whichever role an environment builds.
//
// src/common/ is what every role on this board shares - the board target, the box link's codec
// and session, power and environment, the image descriptor, the version - and an environment
// compiles it beside exactly one directory of src/roles/ (platformio.ini, build_src_filter;
// docs/ARCHITECTURE.md section 8). Common code therefore may not include a role's header, and
// these are the four places where it would otherwise have to:
//
//   beforeRestart()   src/common/board/target.cpp's reboot() and powerOff(), the two restarts
//                     this firmware performs, give the role its last word first - the repeater
//                     writes the clock file (spec/LINK-PROTOCOL.md 5.4)
//   raiseAttention()  src/common/power/BoardPower.cpp raises the battery's two attentions
//                     (spec/LINK-PROTOCOL.md section 8); the machine itself is common
//                     (src/common/link/attention/), but the one instance of it belongs to the
//                     role's board link
//   the settings      src/common/board/target.cpp's TwinOakSensors puts the role's own keys on
//                     upstream's `sensor get/set/list` after upstream's own
//                     (docs/ARCHITECTURE.md section 4). The repeater's are
//                     src/roles/repeater/app/Settings.{h,cpp}; another role's are its own
//
// A role defines all six functions - there is no default and no weak symbol, so a role that
// forgets one does not link, and the error names it.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace twinoak {
namespace role {

// Called from the board's reboot() and powerOff(), before upstream's, on every restart this
// firmware performs. It must return: what follows it does not.
void beforeRestart();

// Ask for the Walter's attention. `reason` is one of src/common/link/codec/Numbers.h's
// AttentionReason values and `detail` is that reason's own number. False when the role's
// attention machine refused it (already pending, or raised less than an hour ago).
bool raiseAttention(uint8_t reason, uint16_t detail);

// The role's own settings, in the order `sensor list` prints them after upstream's.
int settingsCount();
const char* settingName(int i);
const char* settingValue(int i);
// False when `name` is not one of the role's keys or the value was refused.
bool settingSet(const char* name, const char* value);

}  // namespace role
}  // namespace twinoak
