// The firmware's version string: "twinoak-repeater <version> on MeshCore <tag> (<pin8>)".
//
// Host-portable: no Arduino, no MeshCore. The three parts come from the build:
//   TWINOAK_FW_VERSION    the release's version (phase 0's task 02 passes it); 0.0.0-dev without
//   TWINOAK_MESHCORE_TAG  [meshcore].tag of platformio.ini
//   TWINOAK_MESHCORE_PIN  [meshcore].pin of platformio.ini, of which the first eight hex digits
//                         are shown
// Nothing on the board calls this yet; phase 1's first task puts it behind `sensor get fw.version`
// and the link's HELLO carries it later.

#pragma once

#include <stddef.h>

#ifndef TWINOAK_FW_VERSION
#define TWINOAK_FW_VERSION "0.0.0-dev"
#endif
#ifndef TWINOAK_MESHCORE_TAG
#error "TWINOAK_MESHCORE_TAG is set by platformio.ini from [meshcore].tag"
#endif
#ifndef TWINOAK_MESHCORE_PIN
#error "TWINOAK_MESHCORE_PIN is set by platformio.ini from [meshcore].pin"
#endif

namespace twinoak {

// Writes "twinoak-repeater <version> on MeshCore <tag> (<pin8>)" into buf, always NUL-terminated,
// and returns the length the full string would have had (snprintf's contract), so a caller can
// tell a truncated buffer from a complete one. pin8 is the first eight characters of pin.
size_t formatVersion(char* buf, size_t len, const char* version, const char* tag, const char* pin);

// The string for this build, from the defines above.
const char* versionString();

}  // namespace twinoak
