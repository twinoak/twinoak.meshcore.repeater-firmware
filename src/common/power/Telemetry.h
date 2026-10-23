// The Cayenne-LPP a telemetry request is answered with - on the mesh and, byte for byte, on the
// box link's LPP_GET (spec/LINK-PROTOCOL.md section 5.8; tasks/phase-1/06-power-and-environment.md).
//
// There is **one list of fields** and two things that write it:
//
//   - `TwinOakSensors::querySensors` (src/common/board/target.cpp) walks `ourFields()` and calls
//     upstream's CayenneLPP for each one, because a mesh telemetry reply is upstream's buffer and
//     upstream's library (examples/simple_repeater/MyMesh.cpp:241-259 at the pin 81dbae59);
//   - `LPP_GET` walks `telemetryFields()` - the same list with the two fields upstream's
//     handleRequest adds around it - and encodes it with `encodeLpp()` below, which is a mirror
//     of CayenneLPP's own `addField` and writes the same bytes.
//
// Why a mirror and not the library: LPP_GET must not perform I2C and must not depend on
// upstream's request path, and the library is not host-portable. The mirror is twenty lines, its
// arithmetic is copied from the library at the pinned version with the line cited, and
// test/test_power holds the two compositions against each other and against hand-computed bytes.
// `docs/link.md` carries the channel table; the numbers are chosen once and never renumbered.
//
// Host-portable: no Arduino, no MeshCore, no heap.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "common/power/Environment.h"
#include "common/power/Ina3221.h"

namespace twinoak {
namespace power {

// ----------------------------------------------------------------------------- the channels

// The LPP data channels of this unit. **Chosen once; never renumbered** - MeshMaster names them,
// and a channel that moves between two firmware versions is a chart that lies.
//
// Channel 1 is upstream's TELEM_CHANNEL_SELF (src/helpers/SensorManager.h), which carries the
// battery voltage and the MCU temperature and is not ours. Channels 2 to 5 are ours, and they
// are safe as fixed numbers only because upstream's own sensor table is **empty** in this build:
// no GPS (ENV_INCLUDE_GPS left out, platformio.ini), no BME280 (src/common/power/Environment.h says
// why) and no INA3221 (docs/ARCHITECTURE.md section 3) - so upstream's `next_available_channel`
// never hands one out. A board of this family whose manager does find a sensor of its own must
// give it a channel above these (tasks/phase-1/reports/06.md).
enum LppChannel : uint8_t {
  kLppChannelSelf = 1,   // upstream's: battery voltage, MCU temperature
  kLppChannelPanel = 2,  // INA3221 channel 1
  kLppChannelBattery = 3,  // INA3221 channel 2
  kLppChannelLoad = 4,   // INA3221 channel 3
  kLppChannelEnclosure = 5,  // the BME280
};

// The Cayenne-LPP types this firmware writes, with their own values
// (src/helpers/sensors/LPPDataHelpers.h at the pin).
enum LppType : uint8_t {
  kLppTemperature = 103,  // 2 bytes, 0.1 C, signed
  kLppVoltage = 116,      // 2 bytes, 0.01 V
  kLppCurrent = 117,      // 2 bytes, 0.001 A, signed
};

// MeshCore's telemetry permission bits, with the link's own values (LPP_GET's perm_mask,
// section 5.8; TELEM_PERM_* in src/helpers/SensorManager.h).
constexpr uint8_t kPermBase = 0x01;
constexpr uint8_t kPermLocation = 0x02;
constexpr uint8_t kPermEnvironment = 0x04;

// The buffer upstream gives its CayenneLPP: MAX_PACKET_PAYLOAD - 4
// (examples/simple_repeater/MyMesh.cpp:867, with MAX_PACKET_PAYLOAD 184 in src/MeshCore.h:20 at
// the pin 81dbae59) - the four bytes being the timestamp a reply is prefixed with. LPP_GET uses
// the same cap, because it answers with exactly what a mesh telemetry request gets.
constexpr size_t kMeshLppCap = 180;

// ----------------------------------------------------------------------------- the snapshot

// Everything a telemetry reply is built from, as the readers left it. No I2C happens anywhere
// below this line.
struct TelemetrySnapshot {
  PowerReadings power;
  EnvReadings env;
  // What board.getBattMilliVolts() answers - channel 2's bus voltage, or 0 when that channel is
  // not valid (src/common/board/target.h). Upstream adds it to channel 1 whatever it is, which is why
  // there is no validity flag beside it here.
  uint16_t batt_mv = 0;
  // board.getMCUTemperature() can be NAN (src/helpers/NRF52Board.cpp, getMCUTemperature), and
  // upstream leaves the field out when it is.
  bool mcu_temp_valid = false;
  float mcu_temp_c = 0.0f;
};

// ----------------------------------------------------------------------------- the fields

// One field: a channel, a type and the value as a float, which is exactly what CayenneLPP's
// add* methods take.
struct LppField {
  uint8_t channel = 0;
  uint8_t type = 0;
  float value = 0.0f;
};

// Room for the longest list this firmware can produce: the base voltage, three channels of
// voltage and current, the enclosure temperature and the MCU temperature.
constexpr size_t kMaxLppFields = 9;

// **Ours**: the three INA3221 channels' voltage and current, then the enclosure temperature.
// Only with the environment permission, which is the gate upstream's own manager uses
// (EnvironmentSensorManager::querySensors at the pin). A channel whose validity bit is clear is
// left out entirely - a missing reading is not a reading of zero - and the numbers of the
// channels that are there do not move because of it.
size_t ourFields(const TelemetrySnapshot& s, uint8_t perm_mask, LppField* out, size_t cap);

// **The whole reply**, in the order upstream's MyMesh::handleRequest builds it: the battery
// voltage on channel 1, then ours, then the MCU temperature on channel 1. The two fields that
// are not ours are added by upstream *whatever* the permissions say (MyMesh.cpp:244 and :255 at
// the pin), and so are they here.
size_t telemetryFields(const TelemetrySnapshot& s, uint8_t perm_mask, LppField* out, size_t cap);

// ----------------------------------------------------------------------------- the bytes

// The bytes CayenneLPP writes for those fields, with its own arithmetic: the channel, the type,
// then the value times the type's multiplier, truncated towards zero, two's complement, **most
// significant byte first** - and a field that would not fit in `cap` is dropped, as the
// library's own overflow check drops it (CayenneLPP 1.6.1, src/CayenneLPP.cpp, addField).
//
// Returns the number of bytes written. `cap` is the library's `_maxsize`, so pass kMeshLppCap to
// get what the mesh would have been told.
size_t encodeLpp(const LppField* fields, size_t count, uint8_t* out, size_t cap);

}  // namespace power
}  // namespace twinoak
