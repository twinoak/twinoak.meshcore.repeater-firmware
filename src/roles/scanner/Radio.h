// All radio work, and nothing else: the SX1262 through RadioLib, driven directly.
//
// NoiseScope's scanner.{h,cpp} (software/firmware/noisescope-rak4630/ at b798aab0 of the
// hardware repository) with its arithmetic moved to src/roles/scanner/Sweep.h and its output to
// src/roles/scanner/Lines.h, so what is left here is the part that touches the part.
//
// **This role drives the radio itself.** The repeater's radio is MeshCore's
// (src/common/board/target.cpp: `radio_driver`, a CustomSX1262Wrapper), because a mesh node's
// receiver belongs to the mesh stack. A scanner has no mesh stack: it retunes between FSK and
// LoRa, uploads Semtech's spectral-scan patch into the chip's RAM and reads instantaneous RSSI a
// thousand times a second, none of which upstream's wrapper exposes. What it must keep is the
// RX path the repeater has, so that a reading means something next to what the repeater hears:
// the DIO2 RF switch, the DIO3 TCXO at 1.8 V, the 140 mA current limit and boosted gain on
// (src/roles/scanner/config.h, which takes all four from the same places the repeater's
// environment does).
//
// One activity at a time. kModeMonitor is the resting state - LoRa RX on the home channel with
// continuous RSSI sampling, which is what the repeater's radio sees when it idles - and every
// activity returns to it.
//
// **It never transmits.** Not one of RadioLib's transmit calls is named in this directory, and
// test/test_rails is what holds that rather than this comment.

#pragma once

#include <stdint.h>

#include "roles/scanner/Commands.h"
#include "roles/scanner/Lines.h"
#include "roles/scanner/Sweep.h"

namespace twinoak {
namespace scanner {

// Implemented in src/roles/scanner/main.cpp: drains both serial ports during a blocking scan
// step so that a command is never lost. Commands are executed between steps, from loop(), which
// is why `stop` always works even in the middle of a histogram sweep.
void inputPoll();

namespace radio {

enum Mode : uint8_t {
  kModeMonitor = 0,
  kModeSweep,
  kModeHist,
  kModeDwell,
  kModeCad,
};

// The word `stat.mode` carries.
const char* modeName(Mode mode);

// Brings SPI and the radio up on the home channel and starts receiving. `lines` is where a scan
// writes its results and its errors, and it must outlive this module (it is main.cpp's).
bool begin(Lines& lines);

// One pass of whatever is running, once per loop().
void service();

Mode mode();
bool ok();

// `passes` of 0 means "until stop". False when the arguments are not a scan this firmware runs
// (src/roles/scanner/Sweep.h) or the radio refused the mode change.
bool startSweep(double f0, double f1, double step_khz, uint32_t dwell_ms, int32_t passes);
bool startHist(double f0, double f1, double step_khz, uint32_t samples, int32_t passes);
bool startDwell(double f_mhz, uint32_t secs, uint32_t win_ms);
bool startCad(double f_mhz, uint32_t count);

// Aborts whatever is running - purging the spectral-scan patch with a hardware reset when a
// histogram sweep was - and returns to the monitor.
void stop();

// The monitor channel. False when the settings are not ones the radio has.
bool setHome(double f_mhz, double bw_khz, uint8_t sf, uint8_t cr);
HomeArgs home();

bool setBoost(bool on);
bool boost();

// The monitor's window. False when nothing has been sampled since the last one, and `out` is
// untouched then; `reset` begins the next window.
bool monitorStats(WindowStats& out, bool reset);

}  // namespace radio
}  // namespace scanner
}  // namespace twinoak
