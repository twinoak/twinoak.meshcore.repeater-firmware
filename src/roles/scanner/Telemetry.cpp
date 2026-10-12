#include "roles/scanner/Telemetry.h"

#include "common/power/Ina3221.h"
#include "common/power/PowerReader.h"
#include "common/power/WireBus.h"
#include "roles/scanner/Commands.h"        // kEpochFloor: what counts as a time
#include "roles/scanner/ScannerHeaders.h"  // Arduino and Wire
#include "roles/scanner/config.h"

namespace twinoak {
namespace scanner {
namespace telemetry {
namespace {

power::WireBus g_bus;
power::PowerReader g_power(g_bus);

bool g_rtc = false;
uint32_t g_epoch_base = 0;  // the epoch at g_millis_base; 0 = never set
uint32_t g_millis_base = 0;

bool probe(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

// --- RV-3028-C7: the 32-bit UNIX-time counter at registers 0x1B..0x1E, LSB first ----------------

constexpr uint8_t kUnixTimeReg = 0x1B;

bool readUnixTime(uint32_t& out) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(kUnixTimeReg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(uint8_t(RTC_ADDR), uint8_t(4)) != 4) return false;
  uint32_t value = 0;
  for (int i = 0; i < 4; i++) value |= uint32_t(Wire.read()) << (8 * i);
  out = value;
  return true;
}

bool writeUnixTime(uint32_t value) {
  // The application note's advice: write it twice, so that a counter tick in the middle of the
  // first write cannot leave a clock that is off by a second.
  for (int pass = 0; pass < 2; pass++) {
    Wire.beginTransmission(RTC_ADDR);
    Wire.write(kUnixTimeReg);
    for (int i = 0; i < 4; i++) Wire.write(uint8_t((value >> (8 * i)) & 0xFF));
    if (Wire.endTransmission() != 0) return false;
  }
  return true;
}

}  // namespace

bool begin() {
  Wire.begin();
  Wire.setClock(I2C_CLOCK_HZ);
  g_rtc = probe(RTC_ADDR);
  if (g_rtc) {
    uint32_t stored = 0;
    // The same floor a `time` command is held to: a counter that has never been set reads as a
    // number from before this firmware existed (src/roles/scanner/Commands.h).
    if (readUnixTime(stored) && stored > kEpochFloor) {
      g_epoch_base = stored;
      g_millis_base = millis();
    }
  }
  g_power.begin(millis());
  return g_rtc || g_power.devicePresent();
}

void loop() { g_power.loop(millis()); }

bool rtcPresent() { return g_rtc; }
bool inaPresent() { return g_power.devicePresent(); }

uint32_t epoch() {
  if (g_epoch_base == 0) return 0;
  return g_epoch_base + (millis() - g_millis_base) / 1000u;
}

bool setEpoch(uint32_t epoch_value) {
  g_epoch_base = epoch_value;
  g_millis_base = millis();
  if (g_rtc) return writeUnixTime(epoch_value);
  return true;
}

PowerFields power() {
  PowerFields out;
  const power::PowerReadings& readings = g_power.readings();
  // All three channels or none: a `pwr` object with one channel missing would read as a rail at
  // zero volts, which is a different thing from a reading that is not there. NoiseScope did the
  // same (its emit_stat required all three reads to succeed).
  out.present = readings.ch[0].valid && readings.ch[1].valid && readings.ch[2].valid;
  if (!out.present) return out;
  out.vpanel_mv = readings.ch[0].bus_mv;
  out.ipanel_ma = readings.ch[0].current_ma;
  out.vbat_mv = readings.ch[1].bus_mv;
  out.ibat_ma = readings.ch[1].current_ma;
  out.vload_mv = readings.ch[2].bus_mv;
  out.iload_ma = readings.ch[2].current_ma;
  return out;
}

}  // namespace telemetry
}  // namespace scanner
}  // namespace twinoak
