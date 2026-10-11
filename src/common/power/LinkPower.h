// The readers of src/common/power/ as the box link's session sees them: POWER_GET, ENV_GET and LPP_GET
// (spec/LINK-PROTOCOL.md section 5.8), and the two capability bits POWER and ENV that come with
// the interfaces being there at all (src/common/link/session/Interfaces.h).
//
// **No I2C happens here.** Every one of the three answers out of the snapshot the readers keep,
// which is what makes "a request on the link never performs I2C" a property of the code's shape:
// the only calls into a bus are PowerReader::loop() and EnvReader::loop(), both driven from
// TwinOakSensors::loop() once a pass (src/common/power/BoardPower.cpp).
//
// Host-portable: no Arduino, no MeshCore. The snapshot arrives through the source below, so
// test/test_power and tools/linkloop/ drive the real handlers against fake readings.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "common/link/codec/Messages.h"
#include "common/link/codec/Numbers.h"
#include "common/link/session/Interfaces.h"
#include "common/power/Telemetry.h"

namespace twinoak {
namespace power {

// The bits and the permissions of src/common/power/ are the link's own, by value and not by accident.
static_assert(kEnvTemperatureBit == link::kEnvTemperature, "ENV_GET's valid bit 0");
static_assert(kEnvHumidityBit == link::kEnvHumidity, "ENV_GET's valid bit 1");
static_assert(kEnvPressureBit == link::kEnvPressure, "ENV_GET's valid bit 2");
static_assert(kPermBase == link::kLppBase, "LPP_GET's perm_mask bit 0");
static_assert(kPermLocation == link::kLppLocation, "LPP_GET's perm_mask bit 1");
static_assert(kPermEnvironment == link::kLppEnvironment, "LPP_GET's perm_mask bit 2");
static_assert(kChannels == link::kPowerChannels, "three INA3221 channels");

// What the three handlers read: one consistent snapshot of both readers.
struct TelemetrySource {
  virtual ~TelemetrySource() {}
  virtual TelemetrySnapshot snapshot() const = 0;
};

class LinkPower : public link::PowerAccess, public link::EnvAccess, public link::LppAccess {
 public:
  explicit LinkPower(TelemetrySource& source) : src_(source) {}

  // POWER_GET. Always answers: a channel the reader could not read has its validity bit clear
  // and reads 0, which is the protocol's own answer for it, so there is no case in which this
  // firmware has nothing to say.
  bool readPower(link::PowerResponse& out) override;

  // ENV_GET, the same way: `valid` is 0 on a unit with no BME280 fitted or one whose part
  // stopped answering.
  bool readEnv(link::EnvResponse& out) override;

  // LPP_GET: "the Cayenne-LPP bytes the node would answer a mesh telemetry request with at those
  // permissions" (section 5.8). Built from the same field list and with the same cap as the mesh
  // path (src/common/power/Telemetry.h), so the two are the same bytes.
  size_t readLpp(uint8_t perm_mask, uint8_t* out, size_t cap) override;

 private:
  TelemetrySource& src_;
};

}  // namespace power
}  // namespace twinoak
