#include "target.h"

#include <stdio.h>
#include <string.h>

#include "app/Settings.h"
#include "app/Version.h"
#include "clock/BoardClock.h"
#include "link/BoardLink.h"

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

// Both of these are a restart this firmware asked for, so the clock's lower bound is written
// first and the next boot can carry on from this second rather than from an hour ahead of it
// (src/clock/ClockKeeper.h). Neither returns. During begin()'s own boot-voltage protection -
// which can call powerOff() before the file system is attached - the write simply fails and
// nothing is changed, which is why that path needs no guard of its own.
void TwinOakBoard::reboot() {
  twinoak::clock::saveBeforeRestart();
  RAK4631Board::reboot();
}

void TwinOakBoard::powerOff() {
  twinoak::clock::saveBeforeRestart();
  RAK4631Board::powerOff();
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

// The five keys of docs/ARCHITECTURE.md section 4 are a portable table with native tests
// (src/app/Settings.{h,cpp}); this is the board's half of it - the six things that table reads
// and the one thing it writes, each one line. They are on upstream's CLI and not only on the box
// link so that they can be read from the USB console and, over the mesh, by an admin: that is how
// a unit whose Walter is silent is diagnosed without opening the box, and `walter.wake` is how
// such a unit's uplink is woken from a phone.
class BoardSettingsHost : public twinoak::SettingsHost {
 public:
  const char* fwVersion() const override { return twinoak::versionString(); }
  uint16_t linkErrors() const override { return twinoak::link::linkErrors(); }
  bool walterEverSeen() const override { return twinoak::link::walterEverSeen(); }
  uint32_t walterSeenSecondsAgo() const override {
    return twinoak::link::walterSeenSecondsAgo();
  }
  const char* clockStateName() const override { return twinoak::clock::stateName(); }
  bool attentionPending() const override { return twinoak::link::attentionPending(); }
  bool raiseWalterWake() override { return twinoak::link::raiseWalterWake(); }
};

BoardSettingsHost g_settings_host;
twinoak::Settings g_settings(g_settings_host);

}  // namespace

// Ours follow upstream's own, which on this board are none - EnvironmentSensorManager lists a
// setting only for a detected GPS (src/helpers/sensors/EnvironmentSensorManager.cpp:685 at the
// pin d9296435) and this adapter has none. They are added and not substituted all the same: every
// call below asks upstream first, so a board or a bump that gives the manager settings of its own
// keeps them, at the indices it chose, with ours after them.
int TwinOakSensors::getNumSettings() const {
  return EnvironmentSensorManager::getNumSettings() + twinoak::Settings::count();
}

const char* TwinOakSensors::getSettingName(int i) const {
  const int upstream = EnvironmentSensorManager::getNumSettings();
  if (i < upstream) return EnvironmentSensorManager::getSettingName(i);
  return twinoak::Settings::name(i - upstream);
}

const char* TwinOakSensors::getSettingValue(int i) const {
  const int upstream = EnvironmentSensorManager::getNumSettings();
  if (i < upstream) return EnvironmentSensorManager::getSettingValue(i);
  return g_settings.value(i - upstream);
}

bool TwinOakSensors::setSettingValue(const char* name, const char* value) {
  if (EnvironmentSensorManager::setSettingValue(name, value)) return true;
  return g_settings.set(name, value);  // `walter.wake 1`; the other four refuse every value
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
