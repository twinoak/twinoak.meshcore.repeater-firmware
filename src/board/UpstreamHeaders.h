// Every upstream header the board side uses - the Adafruit core's, RadioLib's, MeshCore's -
// included from one place, as system headers.
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
// A file of ours includes this, or src/app/UpstreamMyMesh.h, and never an upstream header
// directly: a header first met outside a system-header context is compiled with our warnings on.

#pragma once

#pragma GCC system_header

#include <Arduino.h>
#include <Wire.h>

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>

#include <Mesh.h>
#include <MeshCore.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/AutoDiscoverRTCClock.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/sensors/EnvironmentSensorManager.h>

// The BME280's driver, for src/power/: upstream's own BME280 support is out of this build
// (src/power/Environment.h says why) and this firmware drives the part itself. The library is a
// pinned dependency already (platformio.ini, [libs].sensors) and CayenneLPP comes in above,
// through upstream's SensorManager.h.
#include <Adafruit_BME280.h>

#include <RAK4631Board.h>
