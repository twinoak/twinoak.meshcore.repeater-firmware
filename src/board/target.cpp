#include "target.h"

#include <string.h>

#include "app/Version.h"

// --- the board --------------------------------------------------------------------------------

// NRF52Board's constructor takes a char* (src/helpers/NRF52Board.h) and only ever reads it - the
// name the bootloader's OTA mode advertises (NRF52Board.cpp, startOTAUpdate). RAK4631Board passes
// the literal "RAK4631_OTA" (variants/rak4631/RAK4631Board.h). NRF52Board is a *virtual* base of
// RAK4631Board (through NRF52BoardDCDC), so the most-derived class is the one that constructs it:
// the same literal, with the const_cast our -Werror asks for where upstream has the bare literal.
TwinOakBoard::TwinOakBoard() : NRF52Board(const_cast<char*>("RAK4631_OTA")) {}

void TwinOakBoard::begin() {
  RAK4631Board::begin();   // DC/DC, Wire.setPins(SDA 13, SCL 14) + Wire.begin(), SX126X_POWER_EN
  Wire.setClock(100000);   // 100 kHz: 1 k pull-ups through the bulkhead filter (INTERCONNECT.md)
}

TwinOakBoard board;

// --- the radio, as upstream's variants/rak4631/target.cpp has it --------------------------------

RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, SPI);

WRAPPER_CLASS radio_driver(radio, board);

// --- the clock: a volatile clock, and a hand-fitted RTC found by itself if there is one -----------

VolatileRTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);

// --- the sensors --------------------------------------------------------------------------------

namespace {

struct Setting {
  const char* name;
  const char* (*get)();
};

const char* fwVersion() { return twinoak::versionString(); }

// Ours, after upstream's own (which on this board are none unless a GPS is detected - and there is
// no GPS). Read-only for now; a `set` of one of these is refused.
const Setting kSettings[] = {
    {"fw.version", fwVersion},
};
const int kNumSettings = sizeof(kSettings) / sizeof(kSettings[0]);

}  // namespace

int TwinOakSensors::getNumSettings() const {
  return EnvironmentSensorManager::getNumSettings() + kNumSettings;
}

const char* TwinOakSensors::getSettingName(int i) const {
  const int upstream = EnvironmentSensorManager::getNumSettings();
  if (i < upstream) return EnvironmentSensorManager::getSettingName(i);
  i -= upstream;
  return (i >= 0 && i < kNumSettings) ? kSettings[i].name : NULL;
}

const char* TwinOakSensors::getSettingValue(int i) const {
  const int upstream = EnvironmentSensorManager::getNumSettings();
  if (i < upstream) return EnvironmentSensorManager::getSettingValue(i);
  i -= upstream;
  return (i >= 0 && i < kNumSettings) ? kSettings[i].get() : NULL;
}

bool TwinOakSensors::setSettingValue(const char* name, const char* value) {
  if (EnvironmentSensorManager::setSettingValue(name, value)) return true;
  return false;  // ours are read-only in this task; upstream's CLI then answers "can't find custom var"
}

TwinOakSensors sensors;

// --- the functions upstream's main.cpp calls ----------------------------------------------------

bool radio_init() {
  rtc_clock.begin(Wire);
  return radio.std_init(&SPI);
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);  // create new random identity
}

void radio_random(uint8_t* dest, size_t len) {
  RadioNoiseListener rng(radio);
  rng.random(dest, len);
}
