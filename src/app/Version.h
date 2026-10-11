// The firmware's version string: "twinoak-repeater <version> on MeshCore <tag> (<pin8>)".
//
// Host-portable: no Arduino, no MeshCore. The three parts come from the build:
//   TWINOAK_FW_VERSION    the release's version (phase 0's task 02 passes it); 0.0.0-dev without
//   TWINOAK_MESHCORE_TAG  [meshcore].tag of platformio.ini
//   TWINOAK_MESHCORE_PIN  [meshcore].pin of platformio.ini, of which the first eight hex digits
//                         are shown
// `sensor get fw.version` answers versionString() (phase 1's task 01). The box link's HELLO keeps
// the three apart as spec/LINK-PROTOCOL.md 5.1 and spec/FIRMWARE-MANIFEST.md 4 do - `fw_version`
// is the release's version alone and `upstream` is what it is built on - so it carries
// TWINOAK_FW_VERSION and upstreamString() (src/link/BoardLink.cpp).

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

// Writes "<tag>+<pin8>" into buf, always NUL-terminated, and returns the length the full string
// would have had: the box link's `upstream` field (spec/LINK-PROTOCOL.md 5.1, whose example is
// `repeater-v1.17.1+d9296435`).
size_t formatUpstream(char* buf, size_t len, const char* tag, const char* pin);

// That string for this build, from TWINOAK_MESHCORE_TAG and TWINOAK_MESHCORE_PIN.
const char* upstreamString();

}  // namespace twinoak
