#include "common/board/target.h"

#include <stdio.h>
#include <string.h>

#include "common/board/RoleHooks.h"
#include "common/power/BoardPower.h"

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

// Both of these are a restart this firmware asked for, so the role gets its last word first -
// for the repeater, the clock's lower bound is written and the next boot carries on from this
// second rather than from an hour ahead of it (src/common/board/RoleHooks.h,
// src/roles/repeater/clock/ClockKeeper.h). Neither returns. During begin()'s own boot-voltage
// protection - which can call powerOff() before the file system is attached - the write simply
// fails and nothing is changed, which is why that path needs no guard of its own.
void TwinOakBoard::reboot() {
  twinoak::role::beforeRestart();
  RAK4631Board::reboot();
}

void TwinOakBoard::powerOff() {
  twinoak::role::beforeRestart();
  RAK4631Board::powerOff();
}

// INA3221 channel 2's bus voltage - the pack side of RS3 - out of the reader's cache, or 0 when
// that channel is not valid (target.h says what reads this and why 0 is the right answer). It is
// deliberately *not* upstream's RAK4631Board::getBattMilliVolts(), whose PIN_VBAT_READ is
// unconnected on this adapter.
uint16_t TwinOakBoard::getBattMilliVolts() { return twinoak::power::batteryMilliVolts(); }

TwinOakBoard board;

// --- the radio, as upstream's variants/rak4631/target.cpp has it --------------------------------

RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, SPI);

WRAPPER_CLASS radio_driver(radio, board);

// --- the clock: a volatile clock, and a hand-fitted RTC found by itself if there is one -----------

VolatileRTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);

// --- the readers of src/common/power/, where upstream's manager is already called --------------

// The role's main.cpp calls sensors.begin() in setup() and sensors.loop() once a pass, so this is
// where the INA3221 and the BME280 are brought up and swept. Upstream's begin() runs first: it
// scans the bus and would initialise anything of its own it found there - on this board, nothing
// (target.h says why for each part).
bool TwinOakSensors::begin() {
  const bool ok = EnvironmentSensorManager::begin();
  twinoak::power::begin(InternalFS);  // after board.begin()'s Wire and main.cpp's InternalFS
  return ok;
}

void TwinOakSensors::loop() {
  EnvironmentSensorManager::loop();
  twinoak::power::loop();  // one INA3221 channel, one BME280 sample, the battery thresholds
}

// The telemetry a mesh client is answered with. Upstream's MyMesh::handleRequest has already put
// the battery voltage on channel 1 and will add the MCU's temperature after this returns
// (examples/simple_repeater/MyMesh.cpp:241-259 at the pin d9296435); upstream's manager goes
// first here for the same reason the settings do, and ours follow on the fixed channels of
// src/common/power/Telemetry.h. LPP_GET on the box link answers with the same field list and the same
// cap, which is what makes the two the same bytes (src/common/power/LinkPower.cpp).
bool TwinOakSensors::querySensors(uint8_t requester_permissions, CayenneLPP& telemetry) {
  EnvironmentSensorManager::querySensors(requester_permissions, telemetry);
  twinoak::power::appendOurFields(telemetry, requester_permissions);
  return true;
}

// --- the settings -------------------------------------------------------------------------------

// The role's follow upstream's own, which on this board are none - EnvironmentSensorManager lists a
// setting only for a detected GPS (src/helpers/sensors/EnvironmentSensorManager.cpp:685 at the
// pin d9296435) and this adapter has none. They are added and not substituted all the same: every
// call below asks upstream first, so a board or a bump that gives the manager settings of its own
// keeps them, at the indices it chose, with ours after them.
int TwinOakSensors::getNumSettings() const {
  return EnvironmentSensorManager::getNumSettings() + twinoak::role::settingsCount();
}

const char* TwinOakSensors::getSettingName(int i) const {
  const int upstream = EnvironmentSensorManager::getNumSettings();
  if (i < upstream) return EnvironmentSensorManager::getSettingName(i);
  return twinoak::role::settingName(i - upstream);
}

const char* TwinOakSensors::getSettingValue(int i) const {
  const int upstream = EnvironmentSensorManager::getNumSettings();
  if (i < upstream) return EnvironmentSensorManager::getSettingValue(i);
  return twinoak::role::settingValue(i - upstream);
}

bool TwinOakSensors::setSettingValue(const char* name, const char* value) {
  if (EnvironmentSensorManager::setSettingValue(name, value)) return true;
  // The role's own keys - for the repeater, `walter.wake 1`, `power.low_mv <mv>` and
  // `power.crit_mv <mv>`; the rest of its nine are read-only.
  return twinoak::role::settingSet(name, value);
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
