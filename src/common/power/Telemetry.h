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
// upstream's request path, and the library puts its buffer on the heap. The mirror is twenty
// lines and its arithmetic is copied from the library at the pinned version with the line cited.
// Two tests hold it: `test/test_power` holds the two compositions against each other and against
// hand-computed bytes, and since p3.12-01 `test/test_cayennelpp` links the **library itself** -
// the same exact version, compiled for the host by `[env:native]` - and holds the mirror's bytes
// against the library's for every type in the table below.
// `docs/link.md` carries the channel table; the numbers are chosen once and never renumbered.
//
// Host-portable: no Arduino, no MeshCore, no heap.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "common/power/Environment.h"
#include "common/power/Ina3221.h"
#include "common/power/PowerTotals.h"

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
  kLppChannelEnclosure = 5,  // the BME280: temperature, humidity and pressure (p3.12-01)
  // What has flowed since the firmware started (PowerTotals.h, p3.10-02). One channel each, all
  // of them the generic sensor below, and the step of each is in docs/link.md section 2d: a
  // charge is 0.01 mAh and an energy 0.1 mWh. A reader takes the difference between two answers.
  kLppChannelPanelCharge = 6,
  kLppChannelPanelEnergy = 7,
  kLppChannelBatteryChargeIn = 8,
  kLppChannelBatteryChargeOut = 9,
  kLppChannelBatteryEnergyIn = 10,
  kLppChannelBatteryEnergyOut = 11,
  kLppChannelLoadCharge = 12,
  kLppChannelLoadEnergy = 13,
  kLppChannelCovered = 14,  // the seconds those totals cover, 1 s per step
};

// The Cayenne-LPP types this firmware writes, with their own values
// (src/helpers/sensors/LPPDataHelpers.h at the pin).
enum LppType : uint8_t {
  kLppGenericSensor = 100,  // 4 bytes, 1 per step, **unsigned**
  kLppTemperature = 103,  // 2 bytes, 0.1 C, signed
  kLppRelativeHumidity = 104,  // 1 byte, 0.5 %RH, unsigned
  kLppBarometricPressure = 115,  // 2 bytes, 0.1 hPa, unsigned
  kLppVoltage = 116,      // 2 bytes, 0.01 V
  kLppCurrent = 117,      // 2 bytes, 0.001 A, signed
};

// ----------------------------------------------------------------------------- the totals' steps

// One step of each of the nine totals, in the accumulators' own units (PowerTotals.h:
// microcoulomb and microjoule, both 3 600 000 to the milli-unit-hour). The steps are chosen so
// that the smallest hour this unit has - an hour at 5 mA - is hundreds of steps of each and not
// tens: 5 mAh is **500** steps of charge, and 5 mA at the lowest rail this board measures is
// 15 mWh, which is **150** steps of energy.
constexpr uint64_t kChargeStepMicroCoulomb = kMicroPerMilliHour / 100u;  // 0.01 mAh = 36 000 uC
constexpr uint64_t kEnergyStepMicroJoule = kMicroPerMilliHour / 10u;     // 0.1 mWh = 360 000 uJ
constexpr uint64_t kCoveredStepMillis = 1000u;                           // 1 s

// **What a total does at the top, decided rather than inherited.** The generic sensor is four
// bytes and the library takes a `float`, so the value on the wire is the step count rounded to
// the nearest float and then truncated into a `uint32_t` (CayenneLPP 1.6.1, addField). A float of
// 2^32 or more converted to a `uint32_t` is undefined in C++, and 4 294 967 295 **is** such a
// float: it rounds up to 2^32. So a total is held at the largest value a float holds below 2^32 -
// 0xFFFFFF00, which is 16 777 215 x 256 - and stays there. It saturates; it never wraps, and
// what it does at the ceiling is not left to a conversion.
//
// Unreachable in service: 0xFFFFFF00 steps of charge is 42 949 Ah, 245 years of a 20 mA load.
// The limit that is reached is the float's own: above 2^24 steps a total reads as the nearest
// multiple of two steps (then four above 2^25, and so on), which never reads lower than the
// answer before it - docs/link.md section 2d has the dates.
constexpr uint32_t kTotalCeiling = 0xFFFFFF00u;

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
  // What has flowed since the firmware started, as PowerTotals left it - the four directions in
  // its own order, and the milliseconds they cover. `totals_counted` is false until one sweep has
  // been added up, and then channels 6 to 14 are left out **together**. fillTotals() below is
  // the one place this is copied out of a reader.
  bool totals_counted = false;
  Flow totals[PowerTotals::kFlows];
  uint64_t totals_covered_ms = 0;
};

// The reader's accumulator into the three members above. One function, so that
// src/common/power/BoardPower.cpp and tools/linkloop/ cannot copy it two different ways.
void fillTotals(const PowerTotals& totals, TelemetrySnapshot& out);

// ----------------------------------------------------------------------------- the fields

// One field: a channel, a type and the value as a float, which is exactly what CayenneLPP's
// add* methods take.
struct LppField {
  uint8_t channel = 0;
  uint8_t type = 0;
  float value = 0.0f;
};

// Room for the longest list this firmware can produce: the base voltage, three channels of
// voltage and current, the enclosure's temperature, humidity and pressure, the nine totals, and
// the MCU temperature. Exactly the longest list and not one more, so that a field added without
// raising this bound is a red test (test/test_power) and not a channel missing from the wire.
constexpr size_t kMaxLppFields = 20;

// **Ours**: the three INA3221 channels' voltage and current, the enclosure's temperature,
// humidity and pressure, then the nine totals of channels 6 to 14. Only with the environment
// permission, which is the gate upstream's own manager uses
// (EnvironmentSensorManager::querySensors at the pin). A channel whose validity bit is clear is
// left out entirely - a missing reading is not a reading of zero - and the numbers of the
// channels that are there do not move because of it. The same holds field by field inside
// channel 5: the BME280's three quantities have a validity bit each, and one that is clear takes
// its field out and leaves the other two where they were.
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
