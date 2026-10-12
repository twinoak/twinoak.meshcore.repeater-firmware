#include "common/power/BoardPower.h"

#include "common/board/RoleHooks.h"    // raiseAttention: the role's own attention machine
#include "common/board/target.h"        // `board`, for the MCU temperature
#include "common/link/codec/Numbers.h"  // the attention reasons of LINK-PROTOCOL.md section 8
#include "common/power/BatteryWatch.h"
#include "common/power/Environment.h"
#include "common/power/PowerReader.h"
#include "common/power/Prefs.h"
#include "common/power/WireBus.h"  // the I2C master of the reader, shared with the scanner

namespace twinoak {
namespace power {
namespace {

// --- the BME280 ---------------------------------------------------------------------------------

// The part on the A-board (PROJECT-KNOWLEDGE.md section 4: BME280 @0x76).
constexpr uint8_t kBme280Address = 0x76;

// Upstream's own BME280 support is out of the build (src/common/power/Environment.h says why), so the
// part is driven here - with the pinned Adafruit library for the compensation arithmetic, which
// is Bosch's and not something to write twice.
//
// **MODE_NORMAL, not MODE_FORCED.** In normal mode the part measures by itself every second and a
// read is three register reads with no waiting; upstream's forced mode triggers a conversion and
// then spins on the status register (Adafruit_BME280::takeForcedMeasurement), which is exactly
// the wait that must not happen on a request's path. X1 oversampling with the filter off is
// upstream's own choice of accuracy, and in normal mode at a one-second standby it costs about
// 4 uA.
class Bme280Sensor : public EnvSensor {
 public:
  bool begin() override {
    const uint32_t started = micros();
    // Probe before touching the driver, as upstream's own manager does (its scanI2CBus, so that
    // "a sensor whose address does not ACK during the scan is never touched by a library call").
    Wire.beginTransmission(kBme280Address);
    if (Wire.endTransmission() != 0) {
      last_us_ = micros() - started;
      return false;
    }
    if (!bme_.begin(kBme280Address, &Wire)) {
      last_us_ = micros() - started;
      return false;
    }
    bme_.setSampling(Adafruit_BME280::MODE_NORMAL, Adafruit_BME280::SAMPLING_X1,
                     Adafruit_BME280::SAMPLING_X1, Adafruit_BME280::SAMPLING_X1,
                     Adafruit_BME280::FILTER_OFF, Adafruit_BME280::STANDBY_MS_1000);
    last_us_ = micros() - started;
    return true;
  }

  bool read(float& temp_c, float& rh_pct, float& pressure_pa) override {
    const uint32_t started = micros();
    temp_c = bme_.readTemperature();
    rh_pct = bme_.readHumidity();
    pressure_pa = bme_.readPressure();
    last_us_ = micros() - started;
    // The driver has no error return: a part that stopped answering reads 0xFFFF out of every
    // register, which the compensation turns into a number far outside the part's own range -
    // and envReadingsFrom() is what refuses it (src/common/power/Environment.cpp). So there is nothing
    // for this to report false about, and `valid` is where a bad sample shows.
    return true;
  }

  uint32_t lastTransactionMicros() const override { return last_us_; }

 private:
  Adafruit_BME280 bme_;
  uint32_t last_us_ = 0;
};

// --- the preferences file -----------------------------------------------------------------------

// Beside upstream's own files and the clock's, in the region docs/ARCHITECTURE.md section 9 maps.
// An update never erases it.
const char* const kPrefsFile = "/power";

// The core's File lives in a namespace of its own (Adafruit_LittleFS_File.h), as src/roles/repeater/clock/
// BoardClock.cpp names it once.
typedef Adafruit_LittleFS_Namespace::File PrefsFileHandle;

FILESYSTEM* g_fs = nullptr;

bool loadPrefs(SavedPrefs& out) {
  if (g_fs == nullptr || !g_fs->exists(kPrefsFile)) return false;
  uint8_t buf[kSavedPrefsBytes] = {0};
  PrefsFileHandle file = g_fs->open(kPrefsFile);
  if (!file) return false;
  const int n = file.read(buf, uint16_t(sizeof(buf)));
  file.close();
  if (n != int(sizeof(buf))) return false;
  return decodeSavedPrefs(buf, sizeof(buf), out);
}

bool savePrefs(const SavedPrefs& in) {
  if (g_fs == nullptr) return false;
  uint8_t buf[kSavedPrefsBytes] = {0};
  encodeSavedPrefs(in, buf);
  // FILE_O_WRITE seeks to the end, so the seek is what makes this a rewrite in place of a
  // fixed-size record rather than an append (src/roles/repeater/clock/BoardClock.cpp says the same).
  PrefsFileHandle file = g_fs->open(kPrefsFile, Adafruit_LittleFS_Namespace::FILE_O_WRITE);
  if (!file) return false;
  bool ok = file.seek(0);
  if (ok) ok = (file.write(buf, sizeof(buf)) == sizeof(buf));
  file.close();
  return ok;
}

// --- the one of each ----------------------------------------------------------------------------

WireBus g_bus;
Bme280Sensor g_bme;
PowerReader g_power(g_bus);
EnvReader g_env(g_bme);
BatteryWatch g_battery;

// The snapshot the three link handlers and the mesh's telemetry are built from.
class BoardTelemetry : public TelemetrySource {
 public:
  TelemetrySnapshot snapshot() const override {
    TelemetrySnapshot s;
    s.power = g_power.readings();
    s.env = g_env.readings();
    s.batt_mv = s.power.batteryMilliVolts();
    // Upstream reads the MCU's own sensor here and leaves the field out when it is not a number
    // (MyMesh.cpp:254-257 at the pin). It is not an I2C read: on the nRF52 it is the die's own
    // TEMP peripheral, bounded at 5 ms by upstream's own loop (NRF52Board::getMCUTemperature) -
    // and with the SoftDevice up it is one sd_temp_get call.
    const float mcu = board.getMCUTemperature();
    s.mcu_temp_valid = (mcu >= -300.0f) || (mcu < -300.0f);  // false only for a NaN
    s.mcu_temp_c = s.mcu_temp_valid ? mcu : 0.0f;
    return s;
  }
};

BoardTelemetry g_telemetry;
LinkPower g_link_power(g_telemetry);

}  // namespace

void begin(FILESYSTEM& fs) {
  g_fs = &fs;
  SavedPrefs saved;
  if (loadPrefs(saved)) g_battery.setThresholds(saved.low_mv, saved.crit_mv);
  const uint32_t now = millis();
  g_power.begin(now);
  g_env.begin(now);
}

void loop() {
  const uint32_t now = millis();
  g_power.loop(now);
  g_env.loop(now);

  const ChannelReading& batt = g_power.readings().ch[1];
  const uint8_t entered = g_battery.poll(batt.valid, batt.bus_mv, now);
  if (entered == kBatteryLow) {
    role::raiseAttention(link::kAttentionBatteryLow, g_battery.lastMilliVolts());
  } else if (entered == kBatteryCritical) {
    role::raiseAttention(link::kAttentionBatteryCritical, g_battery.lastMilliVolts());
  }
  // A recovery raises nothing: section 8 has no reason for "the battery came back", and the
  // Walter reads the voltage itself with POWER_GET whenever it is awake.
}

uint16_t batteryMilliVolts() { return g_power.readings().batteryMilliVolts(); }

LinkPower& linkPower() { return g_link_power; }

size_t appendOurFields(CayenneLPP& telemetry, uint8_t perm_mask) {
  LppField fields[kMaxLppFields];
  const size_t n = ourFields(g_telemetry.snapshot(), perm_mask, fields, kMaxLppFields);
  for (size_t i = 0; i < n; i++) {
    switch (fields[i].type) {
      case kLppVoltage:
        telemetry.addVoltage(fields[i].channel, fields[i].value);
        break;
      case kLppCurrent:
        telemetry.addCurrent(fields[i].channel, fields[i].value);
        break;
      case kLppTemperature:
        telemetry.addTemperature(fields[i].channel, fields[i].value);
        break;
      default:
        break;  // a type src/common/power/Telemetry.cpp knows and this switch does not: added, not sent
    }
  }
  return n;
}

uint16_t batteryLowMilliVolts() { return g_battery.lowMilliVolts(); }
uint16_t batteryCritMilliVolts() { return g_battery.critMilliVolts(); }

bool setBatteryLowMilliVolts(uint16_t mv) {
  if (!g_battery.setThresholds(mv, g_battery.critMilliVolts())) return false;
  SavedPrefs out;
  out.low_mv = g_battery.lowMilliVolts();
  out.crit_mv = g_battery.critMilliVolts();
  savePrefs(out);  // a write that failed leaves the running value set and the file as it was
  return true;
}

bool setBatteryCritMilliVolts(uint16_t mv) {
  if (!g_battery.setThresholds(g_battery.lowMilliVolts(), mv)) return false;
  SavedPrefs out;
  out.low_mv = g_battery.lowMilliVolts();
  out.crit_mv = g_battery.critMilliVolts();
  savePrefs(out);
  return true;
}

}  // namespace power
}  // namespace twinoak
