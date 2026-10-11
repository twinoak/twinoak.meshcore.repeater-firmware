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

#include "UpstreamHeaders.h"  // Arduino, RadioLib, MeshCore, RAK4631Board - as system headers

#include <stddef.h>
#include <stdint.h>

// Upstream's RAK4631Board (variants/rak4631/RAK4631Board.{h,cpp}, compiled from the pin) with the
// I2C clock pinned to 100 kHz after begin(). getBattMilliVolts() stays upstream's ADC reading
// until task 06 puts the INA3221's channel 2 behind it.
class TwinOakBoard : public RAK4631Board {
 public:
  TwinOakBoard();
  void begin() override;
};

// Upstream's EnvironmentSensorManager - which finds the BME280 at 0x76 by itself - with the
// settings seam (SensorManager::getNumSettings/getSettingName/getSettingValue/setSettingValue,
// src/helpers/SensorManager.h) overridden so that our keys follow upstream's in `sensor list`,
// `sensor get` and `sensor set` (src/helpers/CommonCLI.cpp). One key for now: fw.version,
// read-only, our version string. Tasks 05 and 06 add theirs here.
class TwinOakSensors : public EnvironmentSensorManager {
 public:
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
// node's key from. Slow, meant for first boot: the admin password (src/app/FirstBoot.cpp).
void radio_random(uint8_t* dest, size_t len);
