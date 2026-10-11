// The BME280 inside the enclosure, read on a schedule into one cache - so that the link's
// ENV_GET, a mesh telemetry request and anything else that wants the air in the box all get the
// **same sample** and none of them waits on I2C (tasks/phase-1/06-power-and-environment.md;
// docs/ARCHITECTURE.md section 6).
//
// This firmware reads the part itself rather than through upstream's manager, and
// tasks/phase-1/reports/06.md says why in full. The short of it: upstream's own BME280 support
// samples the part *inside* querySensors (MODE_FORCED, takeForcedMeasurement(),
// src/helpers/sensors/EnvironmentSensorManager.cpp:271-290 at the pin d9296435), and
// querySensors is on the path of LPP_GET - so with it in the build a request on the box link
// would perform I2C and wait for a conversion. ENV_INCLUDE_BME280 is therefore left out of the
// board environment, test/test_rails holds that, and this is what replaces it.
//
// Host-portable: no Arduino, no MeshCore, no heap. The part arrives through the port below - the
// compensation arithmetic is the driver's, which on the board is the pinned Adafruit library -
// and what is portable and tested here is the schedule, the validity and the conversion into the
// three fields spec/LINK-PROTOCOL.md section 5.8 names.

#pragma once

#include <stdint.h>

namespace twinoak {
namespace power {

// ----------------------------------------------------------------------------- the readings

// ENV_GET's body (section 5.8). A bit of `valid` is clear when that quantity could not be read
// or could not be represented; its field then reads 0.
struct EnvReadings {
  uint8_t valid = 0;       // bit 0 temperature, bit 1 humidity, bit 2 pressure
  int16_t temp_centi = 0;  // degrees Celsius x 100
  uint16_t rh_centi = 0;   // percent x 100
  uint32_t pressure_pa = 0;

  bool temperatureValid() const { return (valid & 0x01) != 0; }
};

// The bits of `valid`, with the link's own names and values (src/common/link/codec/Numbers.h's EnvValid;
// src/common/power/LinkPower.h static_asserts that the two agree).
constexpr uint8_t kEnvTemperatureBit = 0x01;
constexpr uint8_t kEnvHumidityBit = 0x02;
constexpr uint8_t kEnvPressureBit = 0x04;

// One sample of the part turned into those three fields. A value that is not a number, or one
// outside what its field can hold, clears that quantity's bit rather than wrapping: a part half
// way through a reset answers nonsense, and nonsense is a missing reading.
//
// The sane windows are the part's own operating ranges with room either side (BME280 datasheet
// table 1: -40..+85 C, 0..100 %RH, 300..1100 hPa).
EnvReadings envReadingsFrom(float temp_c, float rh_pct, float pressure_pa);

// ----------------------------------------------------------------------------- the port

// The part, as a driver presents it: degrees Celsius, percent relative humidity, pascals. A
// quantity the part cannot measure is handed back as a value that is not a number.
struct EnvSensor {
  virtual ~EnvSensor() {}

  // Probes the bus and configures the part. False when nothing answered: there is then no
  // environment on this unit and ENV_GET answers `valid` 0.
  virtual bool begin() = 0;
  // One sample, with no waiting: the part is configured to measure continuously, so this is
  // three register reads and the driver's compensation. False when a read failed.
  virtual bool read(float& temp_c, float& rh_pct, float& pressure_pa) = 0;
  // How long the last sample took, in microseconds, as the board measured it.
  virtual uint32_t lastTransactionMicros() const = 0;
};

// ----------------------------------------------------------------------------- the numbers

// How often the part is sampled. The air inside a sealed enclosure does not change in a second,
// and five seconds keeps the bus free for the power sweep, which is the one with a deadline.
constexpr uint32_t kEnvSampleIntervalMs = 5000;

// As PowerReader.h: a sample that takes longer than this is a bus in trouble, not a reading.
// Three register reads at 100 kHz are about 1.5 ms, so this is four times the honest cost.
constexpr uint32_t kEnvTransactionBudgetUs = 6000;

// How long a part that would not answer is left alone.
constexpr uint32_t kEnvRetryMs = 5000;

// ----------------------------------------------------------------------------- the reader

class EnvReader {
 public:
  explicit EnvReader(EnvSensor& sensor) : sensor_(sensor) {}

  // Probes and configures the part. Called from TwinOakSensors::begin().
  void begin(uint32_t now_ms);
  // One bounded pass, once per loop(): at most one sample, and nothing at all while the interval
  // has not passed or the part is in its back-off.
  void loop(uint32_t now_ms);

  const EnvReadings& readings() const { return readings_; }

  bool present() const { return present_; }
  uint32_t samples() const { return samples_; }
  uint32_t failures() const { return failures_; }
  uint32_t overruns() const { return overruns_; }

 private:
  void drop(uint32_t now_ms);

  EnvSensor& sensor_;
  EnvReadings readings_;

  bool present_ = false;
  bool have_sampled_ = false;
  uint32_t last_sample_ms_ = 0;
  uint32_t retry_at_ms_ = 0;

  uint32_t samples_ = 0;
  uint32_t failures_ = 0;
  uint32_t overruns_ = 0;
};

}  // namespace power
}  // namespace twinoak
