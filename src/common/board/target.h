// The board target for the single-RAK adapter - ours, replacing MeshCore's
// variants/rak4631/target.{h,cpp} (docs/ARCHITECTURE.md section 3).
//
// Upstream's MyMesh.h does `#include <target.h>` and expects the globals below: `board`,
// `radio_driver`, `rtc_clock`, `sensors`, radio_init() and radio_new_identity(). This directory is
// first on the include path (platformio.ini), so this file is the one that include resolves to,
// in our sources and in the MeshCore library alike (its MyMesh.cpp, UITask.cpp and
// helpers/NRF52Board.cpp include it too). Upstream's target.cpp is not compiled: MC_VARIANT is not
// set, and tools/pio/meshcore_variant.py compiles only RAK4631Board.cpp and variant.cpp out of the
// variant directory.
//
// What differs from upstream's target for this stamp: no display, no GPS (Serial1 is the link to
// the Walter and is not touched here), I2C at 100 kHz (one set of 1 k pull-ups through a 1000 pF
// bulkhead filter, INTERCONNECT.md), and `sensors` is ours so that settings of ours appear in
// `sensor get/set/list`. The radio wiring inside the RAK4630 stamp is the stamp's, so the radio,
// its DC/DC setup and - until task 06 - the battery reading are upstream's unchanged.

#pragma once

// Beside this file, not src-rooted: upstream's own sources include <target.h> through
// -I src/common/board and do not have src/ on their include path (platformio.ini).
#include "UpstreamHeaders.h"  // Arduino, RadioLib, MeshCore, RAK4631Board - as system headers

#include <stddef.h>
#include <stdint.h>

// Upstream's RAK4631Board (variants/rak4631/RAK4631Board.{h,cpp}, compiled from the pin) with the
// I2C clock pinned to 100 kHz after begin(), with the two ways this firmware stops running on
// purpose routed through the clock keeper first, and with the battery read where this board
// really has it.
class TwinOakBoard : public RAK4631Board {
 public:
  TwinOakBoard();
  void begin() override;

  // The battery, which on this adapter is **INA3221 channel 2** and not the WisBlock divider
  // upstream reads: PIN_VBAT_READ (P0.05) is not wired to anything here
  // (docs/ARCHITECTURE.md section 3), so upstream's eight ADC samples of it are eight samples of
  // a floating pin. This returns channel 2's bus voltage, or **0 when that channel is not
  // valid**, which is what spec/LINK-PROTOCOL.md section 5.8 means by a channel whose bit is
  // clear - and 0 is also what keeps upstream's boot-voltage protection out of the way, because
  // it only acts on a reading above 1000 mV (src/helpers/NRF52Board.cpp:128 at the pin
  // d9296435). tasks/phase-1/reports/06.md is the whole reading of that path and what it means
  // for this board.
  //
  // Three things of upstream's read it: `status` over the mesh and the console
  // (examples/simple_repeater/MyMesh.cpp:218, src/helpers/StatsFormatHelper.h:14), the base
  // channel of a telemetry reply (MyMesh.cpp:244) and that boot check. None of them performs
  // I2C through this call: it is the reader's cache (src/common/power/PowerReader.h).
  uint16_t getBattMilliVolts() override;


  // mesh::MainBoard::reboot() and powerOff() are both virtual (src/MeshCore.h:54 and :55 at the
  // pin d9296435; NRF52Board.h implements them as NVIC_SystemReset and SYSTEMOFF). Every restart
  // this firmware performs goes through one of them - the console's `poweroff`/`shutdown`,
  // `reboot` and `clkreboot` (src/helpers/CommonCLI.cpp:184, :186 and :190) as well as the box
  // link's REBOOT - so overriding them is how "the file is written on a restart the firmware
  // itself asks for" is kept without touching a line of upstream's (spec/LINK-PROTOCOL.md 5.4).
  void reboot() override;
  void powerOff() override;
};

// Upstream's EnvironmentSensorManager with three things added and **nothing of its own
// substituted**:
//
//   - the settings seam (SensorManager::getNumSettings/getSettingName/getSettingValue/
//     setSettingValue, src/helpers/SensorManager.h), so that the role's keys follow upstream's
//     in `sensor list`, `sensor get` and `sensor set` (src/helpers/CommonCLI.cpp). Which keys
//     those are is the role's (src/common/board/RoleHooks.h); the repeater's are
//     src/roles/repeater/app/Settings.{h,cpp}, which is portable and tested;
//   - begin() and loop(), which is where this unit's own power and environment readers are
//     driven (src/common/power/BoardPower.h) - the manager is the thing a role's main.cpp
//     already calls once in setup() and once in loop(), so nothing new is threaded through main;
//   - querySensors(), which adds this unit's telemetry after whatever upstream's manager adds.
//
// Upstream's own sensor table is **empty** in this build: no GPS, no INA3221 (it applies one
// shunt value to all three channels) and no BME280 (it samples the part inside querySensors,
// which is on a request's path) - platformio.ini and src/common/power/Environment.h say why for each,
// and test/test_rails holds the last two.
class TwinOakSensors : public EnvironmentSensorManager {
 public:
  bool begin() override;
  void loop() override;
  bool querySensors(uint8_t requester_permissions, CayenneLPP& telemetry) override;

  int getNumSettings() const override;
  const char* getSettingName(int i) const override;
  const char* getSettingValue(int i) const override;
  bool setSettingValue(const char* name, const char* value) override;
};

extern TwinOakBoard board;
extern WRAPPER_CLASS radio_driver;
extern AutoDiscoverRTCClock rtc_clock;
extern TwinOakSensors sensors;

bool radio_init();
mesh::LocalIdentity radio_new_identity();

// Random bytes from the radio's noise listener - the same source radio_new_identity() draws the
// node's key from. Slow, meant for first boot: the admin password
// (src/roles/repeater/app/FirstBoot.cpp).
void radio_random(uint8_t* dest, size_t len);
