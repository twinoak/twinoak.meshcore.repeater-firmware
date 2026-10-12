// Every upstream header the repeater's side of the build uses - the Adafruit core's, RadioLib's,
// MeshCore's - included from one place, as system headers.
//
// The core's own two are src/common/board/CoreHeaders.h, which this file includes: a role that
// compiles no MeshCore (src/roles/scanner/) and a file of common/ that needs only `Wire`
// include that one and stop there. Everything above the core is here.
//
// src/ is built with -Wall -Wextra -Werror (platformio.ini, build_src_flags) and upstream's
// headers are not written to that standard: unused parameters of inline virtuals (MeshCore.h,
// Dispatcher.h, the SoftDevice's nrf_soc.h), a member initialised out of order (Dispatcher's
// constructor), a string literal passed as char* (RAK4631Board's constructor). They are not ours
// to fix. `#pragma GCC system_header` makes the rest of this file a system header, and GCC treats
// what a system header includes as system headers too, so their warnings are not reported and
// -Werror stays about our own code. This is twinoak/meshmaster-agent-firmware's
// src/mesh/MeshCoreHeaders.h, for this board. (An -isystem flag was tried first: on this
// toolchain it reorders the include search so that the core's WString.h is seen twice with
// conflicting declarations; the pragma changes no search order.)
//
// The one thing this does not silence is a `#warning` directive (GCC reports those from system
// headers too): RadioLib's "God mode active" is one, and -Wno-error=cpp in build_src_flags keeps
// it a warning.
//
// A file of ours includes this, or a role's own wrapper beside it
// (src/roles/repeater/app/UpstreamMyMesh.h), and never an upstream header directly: a header
// first met outside a system-header context is compiled with our warnings on.

#pragma once

#pragma GCC system_header

// The Arduino core's two, beside this file: a role that compiles no MeshCore includes that file
// and stops there (src/common/board/CoreHeaders.h says why the wrapper is split in two).
#include "CoreHeaders.h"

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>

#include <Mesh.h>
#include <MeshCore.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/AutoDiscoverRTCClock.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/sensors/EnvironmentSensorManager.h>
// FILESYSTEM - Adafruit_LittleFS on this platform (helpers/IdentityStore.h:5-8 at the pin
// d9296435). src/common/power/ takes one to keep its preferences file, and before the tree was
// split by role it reached the macro through upstream's MyMesh.h; nothing of common/ includes a
// role's header now, so it is named here, where the rest of upstream's headers are.
#include <helpers/IdentityStore.h>
// InternalFS, the Adafruit core's LittleFS instance on the internal flash - the file system
// upstream's own MyMesh.h reaches for the same way (examples/simple_repeater/MyMesh.h:9 at the
// pin). src/common/board/target.cpp hands it to src/common/power/ for the preferences file.
#include <InternalFileSystem.h>

// The BME280's driver, for src/common/power/: upstream's own BME280 support is out of this build
// (src/common/power/Environment.h says why) and this firmware drives the part itself. The library is a
// pinned dependency already (platformio.ini, [libs].sensors) and CayenneLPP comes in above,
// through upstream's SensorManager.h.
#include <Adafruit_BME280.h>

#include <RAK4631Board.h>
