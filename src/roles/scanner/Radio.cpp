#include "roles/scanner/Radio.h"

#include <stdarg.h>
#include <stdio.h>

#include "roles/scanner/ScannerHeaders.h"  // Arduino, SPI, RadioLib, the scan patch
#include "roles/scanner/Telemetry.h"
#include "roles/scanner/config.h"
// The in-radio sweep, vendored from the MeshCore fork of issue #3578 (that directory's own
// headers say which commit, which branch and which licence). It is included after
// ScannerHeaders.h because it needs RadioLib, and it is ours now - so it is compiled with our
// own -Werror and not as a system header.
#include "roles/scanner/vendor/sx126x_spectrum/SX126xSpectrumScanner.h"

namespace twinoak {
namespace scanner {
namespace radio {
namespace {

// A SpectrumSX1262 and not an SX1262: the subclass of the vendored sweep plugin's own header,
// which is an SX1262 plus the four members RADIOLIB_GODMODE would have made public
// (src/roles/scanner/vendor/sx126x_spectrum/SX126xSpectrumScanner.h). Every other line of this
// file uses it as the SX1262 it is.
SpectrumSX1262 g_radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, SPI);
SX126xSpectrumScanner g_scanner(&g_radio);

// The histogram's bin count is the engine's and not ours: if a RadioLib bump changes it, the
// `hist` line of docs/noisescope.md changes with it, and that is a decision and not a build.
static_assert(RADIOLIB_SX126X_SPECTRAL_SCAN_RES_SIZE == kHistBins,
              "RadioLib's spectral-scan result is no longer 33 bins; docs/noisescope.md's `hist` "
              "line says 33 x 4 dB from -11 dBm (src/roles/scanner/Sweep.h, kHistBins)");

// And the plugin's own bound on one call, which the plan cuts segments to.
static_assert(int(SX126xSpectrumScanner::MAX_POINTS) == int(kScopePluginMaxPoints),
              "the sweep plugin's points-per-call is no longer 250; src/roles/scanner/Sweep.h's "
              "kScopePluginMaxPoints is what the segments are cut to");

Lines* g_lines = nullptr;
bool g_ok = false;
Mode g_mode = kModeMonitor;
// The boosted-gain setting, as `boost on|off` left it: the build's to begin with, what `info`
// and `stat` carry, and what every mode begins the chip with. A `scope` that said `boost` or
// `noboost` on its line does **not** touch it - that word is in that scope's plan and holds for
// that scope alone (phase 3.16's task 03).
bool g_boost = (SX126X_RX_BOOSTED_GAIN != 0);
uint32_t g_seq = 0;

// The home channel: the band's defaults to begin with (src/roles/scanner/config.h), and whatever
// `home` last set after that. Written field by field because HomeArgs has default members and so
// is not an aggregate at the board's C++11.
HomeArgs defaultHome() {
  HomeArgs home;
  home.f_mhz = HOME_FREQ_MHZ;
  home.bw_khz = HOME_BW_KHZ;
  home.sf = HOME_SF;
  home.cr = HOME_CR;
  return home;
}

HomeArgs g_home = defaultHome();

// --- the activities' state ----------------------------------------------------------------------

struct SweepState {
  SweepPlan plan;
  uint16_t dwell_ms = 0;
  uint16_t samples = 0;  // hist only
  uint16_t index = 0;
  int32_t passes_left = 0;  // -1 = until stop
  bool infinite = false;
  double avg[kMaxSweepPoints];
  double max[kMaxSweepPoints];
};
SweepState g_sweep;

struct DwellState {
  double f_mhz = 0.0;
  uint32_t end_ms = 0;
  uint32_t window_ms = 0;
  uint32_t window_started_ms = 0;
  uint32_t next_sample_us = 0;
  RssiWindow window;
};
DwellState g_dwell;

struct CadState {
  double f_mhz = 0.0;
  uint16_t target = 0;
  uint16_t done = 0;
  uint16_t detections = 0;
};
CadState g_cad;

// The scope's state. One segment of one sweep runs per service() call - at most
// kScopePluginMaxPoints points, which is some tens of milliseconds - so the loop that feeds the
// watchdog and drains the serial ports comes round between them however wide the span is.
struct ScopeState {
  ScopePlan plan;
  Lines* out = nullptr;      // the port the command came in on, and the only one frames go to
  uint16_t segment = 0;      // the one to run next
  uint16_t armed = 0xFFFF;   // the one the plugin's parameters are loaded for (0xFFFF = none)
  ScopeCalibration cal;      // what the chip's image calibration was last made for
  uint32_t beat_ms = 0;      // when the next frame is due
  uint32_t emitted_ms = 0;   // when the last one actually left, which is what `sps` is counted on
  SteppedScope stepped;      // and where a stepped sweep has got to, when the line asked for one
  ScopeSweepTime time;       // how long this frame's plugin calls took: `us` and `usx`
  unsigned dev = 0;          // and the OR of the chip's error word at this frame's arms: `dev`
  ScopeAccumulator acc;
  uint8_t levels[kScopePluginMaxPoints];  // one segment, as the plugin leaves it
  uint8_t frame[kScopeMaxPoints];         // what take() collapses the sweeps to
};
ScopeState g_scope;

// The radio's ROM version string (register 0x0320, 16 characters), read once at boot.
char g_rom[17] = "";

// The monitor's accumulator and its sampling pace.
RssiWindow g_monitor;
uint32_t g_monitor_next_us = 0;

// --- the chip -----------------------------------------------------------------------------------

Meta nextMeta() {
  Meta m;
  m.seq = ++g_seq;
  m.ts = telemetry::epoch();
  m.ms = millis();
  return m;
}

void error(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

void error(const char* fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (g_lines != nullptr) g_lines->err(buf);
}

// The build's own regulator, in the one place it is read: SX126X_USE_REGULATOR_LDO is defined
// nowhere in this repository, so this is false and the chip runs on its DC-DC converter - which
// in receive is clocked from the synthesizer at about 5 MHz (SX1261/2 data sheet 5.1), and is one
// of the two readings of the echoes of 23 November. **A `scope` may ask for the other one for
// itself** (scopeRadioSetup, src/roles/scanner/Sweep.h); every other mode asks for this.
bool buildUsesLdo() {
  #ifdef SX126X_USE_REGULATOR_LDO
  return SX126X_USE_REGULATOR_LDO != 0;
  #else
  return false;
  #endif
}

// What the chip is begun as for a mode that is not a scope, and for a `scope` that named neither
// of its two words: the build's regulator and the gain `boost on|off` left. There is no state to
// put back when a scope that named one ends, because the word lives in that scope's plan alone.
ScopeRadioSetup modeRadioSetup() {
  return scopeRadioSetup(kScopeRegulatorBuild, kScopeGainAsLeft, buildUsesLdo(), g_boost);
}

// The four settings that make a reading comparable with what the repeater hears. The RF switch
// and the TCXO are arguments of begin()/beginFSK() below; these two are calls of their own, and
// they are made again after every mode change because beginFSK() and begin() reset the chip's
// configuration. `boost` is the gain the mode being begun wants - the setting for every mode but
// a `scope` that asked for its own.
void applyRxPath(bool boost) {
  g_radio.setCurrentLimit(SX126X_CURRENT_LIMIT);
  g_radio.setDio2AsRfSwitch(SX126X_DIO2_AS_RF_SWITCH);
  g_radio.setRxBoostedGainMode(boost);
}

// The 10 is the transmit power begin() insists on having; nothing ever transmits with it
// (test/test_rails). The retry without a TCXO voltage is for a module whose crystal is not a
// TCXO, which answers the SetDIO3AsTCXOCtrl command with an error.
int beginLora(double f, double bw, uint8_t sf, uint8_t cr) {
  const ScopeRadioSetup setup = modeRadioSetup();
  int st = g_radio.begin(float(f), float(bw), sf, cr, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 10, 16,
                         SX126X_DIO3_TCXO_VOLTAGE, setup.ldo);
  if (st == RADIOLIB_ERR_SPI_CMD_FAILED || st == RADIOLIB_ERR_SPI_CMD_INVALID) {
    st = g_radio.begin(float(f), float(bw), sf, cr, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 10, 16,
                       0.0f, setup.ldo);
  }
  if (st == RADIOLIB_ERR_NONE) applyRxPath(setup.boost);
  return st;
}

int beginFsk(double f, double rbw_khz) {
  const ScopeRadioSetup setup = modeRadioSetup();
  int st = g_radio.beginFSK(float(f), 4.8f, 5.0f, float(rbw_khz), 10, 16,
                            SX126X_DIO3_TCXO_VOLTAGE, setup.ldo);
  if (st == RADIOLIB_ERR_SPI_CMD_FAILED || st == RADIOLIB_ERR_SPI_CMD_INVALID) {
    st = g_radio.beginFSK(float(f), 4.8f, 5.0f, float(rbw_khz), 10, 16, 0.0f, setup.ldo);
  }
  if (st == RADIOLIB_ERR_NONE) applyRxPath(setup.boost);
  return st;
}

bool toHomeRx() {
  int st = beginLora(g_home.f_mhz, g_home.bw_khz, g_home.sf, g_home.cr);
  if (st != RADIOLIB_ERR_NONE) {
    error("lora reinit failed (%d)", st);
    return false;
  }
  st = g_radio.startReceive();
  if (st != RADIOLIB_ERR_NONE) {
    error("startReceive failed (%d)", st);
    return false;
  }
  g_monitor.restart();
  g_monitor_next_us = micros();
  return true;
}

// The chip's ROM version string, once, at boot, before the monitor's LoRa channel is set up:
// sixteen characters at register 0x0320 (RADIOLIB_SX126X_REG_VERSION_STRING), read in FSK
// standby because a register read needs the chip configured and this one must not leave it in
// anything the monitor then inherits. `info` carries it and `scope` is decided by it. A
// character a JSON string cannot carry verbatim becomes '?', and a NUL ends the string: what a
// reader gets is what the part said, and always a line that parses.
void readRom() {
  g_rom[0] = '\0';
  if (beginFsk(HOME_FREQ_MHZ, 234.3) != RADIOLIB_ERR_NONE) return;
  if (g_radio.standby() != RADIOLIB_ERR_NONE) return;
  uint8_t raw[16];
  if (g_radio.readRegisters(RADIOLIB_SX126X_REG_VERSION_STRING, raw, sizeof(raw)) !=
      RADIOLIB_ERR_NONE) {
    return;
  }
  size_t at = 0;
  for (; at < sizeof(raw); at++) {
    const char c = char(raw[at]);
    if (c == '\0') break;
    g_rom[at] = (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') ? c : '?';
  }
  g_rom[at] = '\0';
}

void retuneRx(double f_mhz) {
  g_radio.standby();
  g_radio.setFrequency(float(f_mhz));
  g_radio.startReceive();
}

// Samples RSSI for `dur_ms` while keeping both serial ports drained, and answers with the
// average and the maximum of the window.
void sampleWindow(uint16_t dur_ms, double& avg, double& max, uint16_t& n) {
  const uint32_t end_us = micros() + uint32_t(dur_ms) * 1000u;
  uint32_t next_us = micros() + 500;  // let the AGC settle before the first sample
  double sum = 0.0;
  double top = kNoReadingDbm;
  uint16_t count = 0;
  while (int32_t(end_us - micros()) > 0) {
    if (int32_t(next_us - micros()) <= 0) {
      const double v = g_radio.getRSSI(false);
      sum += v;
      if (v > top) top = v;
      count++;
      next_us += 200;  // ~5 kHz inside the window
    }
    inputPoll();
  }
  avg = count > 0 ? sum / double(count) : kNoReadingDbm;
  max = top;
  n = count;
}

bool planFor(double f0, double f1, double step_khz, int32_t passes) {
  SweepPlan plan;
  if (!planSweep(f0, f1, step_khz, plan)) return false;
  g_sweep.plan = plan;
  g_sweep.index = 0;
  g_sweep.infinite = (passes == 0);
  g_sweep.passes_left = g_sweep.infinite ? -1 : passes;
  for (uint16_t i = 0; i < plan.points; i++) {
    g_sweep.avg[i] = kNoReadingDbm;
    g_sweep.max[i] = kNoReadingDbm;
  }
  return true;
}

void emitSweepPass() {
  SweepFields f;
  f.meta = nextMeta();
  f.plan = g_sweep.plan;
  f.dwell_ms = g_sweep.dwell_ms;
  f.avg = g_sweep.avg;
  f.max = g_sweep.max;
  g_lines->sweep(f);
}

// A pass has ended: either the next one begins at point 0, or the activity is finished and the
// radio goes back to the home channel.
void passFinished(const char* what) {
  g_sweep.index = 0;
  if (!g_sweep.infinite) {
    g_sweep.passes_left--;
    if (g_sweep.passes_left <= 0) {
      g_lines->done(what, nextMeta());
      if (g_mode == kModeHist) g_radio.reset();  // purge the RAM patch
      toHomeRx();
      g_mode = kModeMonitor;
      return;
    }
  }
  if (g_mode == kModeSweep) {
    for (uint16_t i = 0; i < g_sweep.plan.points; i++) {
      g_sweep.avg[i] = kNoReadingDbm;
      g_sweep.max[i] = kNoReadingDbm;
    }
  }
}

void serviceSweep() {
  const double f = pointFrequencyMhz(g_sweep.plan, g_sweep.index);
  g_radio.standby();
  g_radio.setFrequency(float(f));
  g_radio.startReceive();
  double avg = kNoReadingDbm;
  double max = kNoReadingDbm;
  uint16_t n = 0;
  sampleWindow(g_sweep.dwell_ms, avg, max, n);
  g_sweep.avg[g_sweep.index] = avg;
  g_sweep.max[g_sweep.index] = max;
  g_sweep.index++;
  if (g_sweep.index >= g_sweep.plan.points) {
    emitSweepPass();
    passFinished("sweep");
  }
}

// --- the histogram ------------------------------------------------------------------------------

// The chip half of measuring one frequency. Each member is one call of RadioLib's - or one
// register of the Module's, for the two that the driver has no call for - and nothing else; the
// order they are made in is measureHistPoint()'s (src/roles/scanner/Sweep.cpp, host-tested),
// because the order and what it does not take the chip's word for are the whole of the fix.
class ChipHistRadio : public HistRadio {
 public:
  int standby() override { return g_radio.standby(); }
  int setFrequency(double mhz) override { return g_radio.setFrequency(float(mhz)); }
  // The two the driver has no call for. RadioLib's spectralScanGetStatus() answers *completed or
  // not* (7.8.0, SX126x.cpp:1212) and nothing of its start or its abort writes this register, so
  // the register itself is both read and - before a start - put back to *none*.
  void clearStatus() override {
    g_radio.module()->SPIwriteRegister(RADIOLIB_SX126X_REG_SPECTRAL_SCAN_STATUS,
                                       RADIOLIB_SX126X_SPECTRAL_SCAN_NONE);
  }
  uint8_t scanStatus() override {
    return g_radio.module()->SPIreadRegister(RADIOLIB_SX126X_REG_SPECTRAL_SCAN_STATUS);
  }
  // The sample interval is passed by name rather than taken as the default, because 8.2 us is
  // kHistSampleUs and every duration of a `hist` is computed from it; the RSSI window is the
  // default the firmware has always used.
  int scanStart(uint16_t samples) override {
    return g_radio.spectralScanStart(samples, RADIOLIB_SX126X_SPECTRAL_SCAN_WINDOW_DEFAULT,
                                     RADIOLIB_SX126X_SCAN_INTERVAL_8_20_US);
  }
  void scanAbort() override { g_radio.spectralScanAbort(); }
  int scanResult(uint16_t* bins) override { return g_radio.spectralScanGetResult(bins); }
  uint32_t nowUs() override { return micros(); }
  void poll() override {
    inputPoll();
    yield();
  }
};
ChipHistRadio g_hist;

// The status register's four values are RadioLib's, and the sequence in Sweep.h names them so
// that it can tell a *completed* left over from the frequency before from one of this scan's.
static_assert(kHistStatusNone == RADIOLIB_SX126X_SPECTRAL_SCAN_NONE,
              "src/roles/scanner/Sweep.h's kHistStatusNone is RadioLib's "
              "RADIOLIB_SX126X_SPECTRAL_SCAN_NONE");
static_assert(kHistStatusOngoing == RADIOLIB_SX126X_SPECTRAL_SCAN_ONGOING,
              "src/roles/scanner/Sweep.h's kHistStatusOngoing is RadioLib's "
              "RADIOLIB_SX126X_SPECTRAL_SCAN_ONGOING");
static_assert(kHistStatusAborted == RADIOLIB_SX126X_SPECTRAL_SCAN_ABORTED,
              "src/roles/scanner/Sweep.h's kHistStatusAborted is RadioLib's "
              "RADIOLIB_SX126X_SPECTRAL_SCAN_ABORTED");
static_assert(kHistStatusCompleted == RADIOLIB_SX126X_SPECTRAL_SCAN_COMPLETED,
              "src/roles/scanner/Sweep.h's kHistStatusCompleted is RadioLib's "
              "RADIOLIB_SX126X_SPECTRAL_SCAN_COMPLETED");

// And the interval kHistSampleUs is the microseconds of: the engine's own table, whose 8.20 us
// entry is this value (RadioLib 7.8.0, SX126x_commands.h:302). A bump that renumbers the table
// is a red build, because kHistSampleUs would then be the wrong number for the value passed.
static_assert(RADIOLIB_SX126X_SCAN_INTERVAL_8_20_US == 11,
              "the SX126x's RSSI reading interval table has been renumbered; "
              "src/roles/scanner/Sweep.h's kHistSampleUs is 8.2 us because scanStart() asks for "
              "RADIOLIB_SX126X_SCAN_INTERVAL_8_20_US");

void serviceHist() {
  const double f = pointFrequencyMhz(g_sweep.plan, g_sweep.index);
  uint16_t bins[kHistBins];
  HistResult r;
  if (!measureHistPoint(g_hist, f, g_sweep.samples, bins, r)) {
    // The words are the portable half's (src/roles/scanner/Sweep.cpp, histErrorMessage):
    // three of them are what this function has always said, and the one for a result that does
    // not add up names the frequency, the sum and `n`. Either way the pass ends, as every other
    // failure of this function ends it.
    char msg[kHistErrorBytes];
    histErrorMessage(r, f, g_sweep.samples, msg, sizeof(msg));
    error("%s", msg);
    stop();
    return;
  }
  HistFields hf;
  hf.meta = nextMeta();
  hf.f_mhz = f;
  hf.rbw_khz = g_sweep.plan.rbw_khz;
  hf.samples = g_sweep.samples;
  hf.bins = bins;
  g_lines->hist(hf);
  g_sweep.index++;
  if (g_sweep.index >= g_sweep.plan.points) passFinished("hist");
}

// --- the scope ----------------------------------------------------------------------------------

// The chip's own account: the GetDeviceErrors word (0x17), which says whether an image
// calibration or a PLL lock failed - something no status code of RadioLib's tells apart. Read
// through the Module's public stream because RadioLib keeps getDeviceErrors() protected (7.8.0,
// SX126x.h:896). It answers in standby only (the comment in SX126x::calibrateImage: "unless mode
// is forced to standby, device errors will be 0"), which the arm is in when it asks.
unsigned readDeviceErrors() {
  uint8_t data[2] = {0, 0};
  g_radio.module()->SPIreadStream(RADIOLIB_SX126X_CMD_GET_DEVICE_ERRORS, data, 2);
  return (unsigned(data[0]) << 8) | unsigned(data[1]);
}

// Named apart from ScopeArmRadio::clearDeviceErrors() below, which is this and nothing else:
// a member of that name would otherwise hide this one inside the class.
void clearChipErrors() {
  const uint8_t none[2] = {0, 0};
  g_radio.module()->SPIwriteStream(RADIOLIB_SX126X_CMD_CLEAR_DEVICE_ERRORS, none, 2);
}

// Arming a segment - everything it needs before its sweep: the image rejection calibrated for
// *its* range and not for the whole span's centre, and the plugin's parameters for its own
// points. The order of the calls is armScopeSegment()'s (src/roles/scanner/Sweep.cpp, host-
// tested) and this is only the chip half of it: each member is one call and nothing else.
//
// Why the order is the fix (p3.5-02, the probe table in tasks/phase-3.5/reports/02.md): after a
// plugin sweep the first RAK4630 refuses an image calibration that starts more than about
// 15 MHz above where the sweep left the synthesizer, with IMG_CALIB_ERR in its device errors
// and "failure to execute" in its status - and once refused, every calibration after it fails
// too until the chip is hardware-reset. RadioLib's SX1262::setFrequency() makes exactly such a
// calibration by itself, +-4 MHz around the *new* centre and before the frequency has moved,
// whenever the move is 20 MHz or more (7.8.0, SX1262.cpp:178-189, SX126x::calibrateImage), which
// every segment after the first is. So the frequency is set with that calibration skipped, and
// the one calibration made is ours, for the range the segment measures - which begins where the
// last sweep ended. Twenty-six segments across 150-960 MHz arm that way on the board.
// The chip reset and the plugin uploaded again - what a `scope` starts with, and what a refused
// calibration is answered with (armScopeSegment). Back in standby, the RX path the repeater's,
// the device errors cleared: nothing of the last sweep is left in the chip.
int beginPlugin(double f0_mhz, ScopeRegulator reg, ScopeGain gain) {
  // The one place either of the two can be anything but the build's and the setting's, and it is
  // for this scope alone: `ldo`/`dcdc` and `boost`/`noboost` on the `scope` line.
  const ScopeRadioSetup setup = scopeRadioSetup(reg, gain, buildUsesLdo(), g_boost);
  g_scanner.end();
  g_radio.reset();
  const int st = g_scanner.begin(float(f0_mhz), SX126X_DIO3_TCXO_VOLTAGE, setup.ldo, setup.boost);
  if (st != RADIOLIB_ERR_NONE) return st;
  // The current limit, which the vendored begin() does not set: the RX path of a reading is the
  // repeater's, setting for setting (src/roles/scanner/config.h). The RF switch and the boosted
  // gain it does set, to the same values this would.
  applyRxPath(setup.boost);
  // The device errors are sticky until cleared (SX1261/2 datasheet 13.5.5, 13.5.6), and every
  // arm reads IMG_CALIB_ERR as its own calibration's verdict: cleared here, so that it is.
  clearChipErrors();
  return RADIOLIB_ERR_NONE;
}

class ChipArmRadio : public ScopeArmRadio {
 public:
  int standby() override { return g_radio.standby(); }
  // skipCalibration = true: SetRfFrequency alone (RadioLib 7.8.0, SX1262::setFrequency(float,
  // bool), SX1262.cpp:178).
  int setFrequency(double mhz) override { return g_radio.setFrequency(float(mhz), true); }
  int calibrateImage(double lo_mhz, double hi_mhz) override {
    return g_radio.calibrateImageRejection(float(lo_mhz), float(hi_mhz));
  }
  unsigned deviceErrors() override { return readDeviceErrors(); }
  void clearDeviceErrors() override { clearChipErrors(); }
  int setSweep(double first_mhz, double step_mhz, uint16_t points, double rbw_khz) override {
    return g_scanner.setSweep(float(first_mhz), float(step_mhz), uint8_t(points), float(rbw_khz));
  }
  // The plugin's own 0x9B command, which blocks for the sweep's duration, and the clock the time
  // inside it is measured on. The plugin's tag is checked inside sweep(): a chip that was reset
  // under us - a brownout - has no 0x9B command and must not be sent one.
  int sweep(uint8_t* levels) override { return g_scanner.sweep(levels); }
  // One reading of where the receiver stands, as the byte a frame carries. RadioLib's
  // getRSSI(false) is GetRssiInst - the raw register over -2 (7.8.0, SX126x.cpp:641-654) - and the
  // plugin files that register's own byte per point (spectrum.asm: "-2 * dBm like GetRssiInst"),
  // so a stepped point and a plugin point put the same number in the frame. The call has no code
  // to answer with, which is why this one always succeeds; kScopeNoReading is left to mean a point
  // nothing was read for, so a reading that would be it is the level below.
  int readLevel(uint8_t& level) override {
    const long raw = long(-2.0f * g_radio.getRSSI(false) + 0.5f);
    level = raw <= 0 ? uint8_t(0)
                     : (raw >= long(kScopeNoReading) ? uint8_t(kScopeNoReading - 1u)
                                                     : uint8_t(raw));
    return RADIOLIB_ERR_NONE;
  }
  uint32_t nowUs() override { return micros(); }
  // The receiver left standing where setFrequency() put it, in continuous receive: a delay and
  // nothing else, at most kScopeSettleMaxUs and so at most 20 ms of one pass of loop() - a
  // fraction of the watchdog, and less than the plugin call that follows it is allowed.
  void settle(uint32_t us) override { delayMicroseconds(us); }
  // The plugin uploaded again - with this scope's own regulator and gain, because a reload in
  // the middle of a scope must not change what is being measured.
  int reload() override {
    return beginPlugin(g_scope.plan.f0, g_scope.plan.reg, g_scope.plan.gain);
  }
};
ChipArmRadio g_arm;

// The two numbers the host-portable half carries for RadioLib's are RadioLib's.
static_assert(RADIOLIB_ERR_NONE == 0, "armScopeSegment() takes 0 for a call that succeeded");
static_assert(kScopeArmCalibrationFailed == RADIOLIB_ERR_SPI_CMD_FAILED,
              "src/roles/scanner/Sweep.h's kScopeArmCalibrationFailed is RadioLib's "
              "RADIOLIB_ERR_SPI_CMD_FAILED, the code the driver returns for the chip's failure "
              "to execute");

// An `err` line that names the call as well as the frequency (docs/noisescope.md section 5),
// with the device errors after it - read now, in standby, because they are sticky until cleared
// and the chip's account of what went wrong is what a report from the field needs - and whether
// the plugin was reloaded on the way.
void scopeError(const char* what, ScopeArmCall call, int code, double at_mhz, bool reloaded) {
  g_radio.standby();
  const unsigned dev = readDeviceErrors();
  char buf[160];
  snprintf(buf, sizeof(buf), "scope %s failed (%d) in %s at %.4f; device errors 0x%04x%s", what,
           code, scopeArmCallName(call), at_mhz, dev, reloaded ? "; after a reload" : "");
  if (g_lines != nullptr) g_lines->err(buf, scopeArmCallName(call), code);
}

void emitScopeFrame(uint32_t now) {
  g_scope.acc.take(g_scope.plan.agg, g_scope.frame);
  ScopeFields f;
  f.meta = nextMeta();
  f.plan = g_scope.plan;
  f.sweeps = g_scope.acc.sweeps();
  f.sps_x10 = scopeSweepsPerSecondX10(f.sweeps, now - g_scope.emitted_ms);
  f.us = g_scope.time.shortestUs();
  f.usx = g_scope.time.longestUs();
  f.device_errors = g_scope.dev;
  f.levels = g_scope.frame;
  g_scope.out->scope(f);
  g_scope.emitted_ms = now;
  // A steady beat while the sink keeps up, and the clock reset when it did not: the fork's rule
  // (SpectrumSession.cpp at 41472f97), which keeps frames evenly spaced without drifting.
  const uint32_t interval = g_scope.plan.interval_ms;
  if (interval == 0) {
    g_scope.beat_ms = now;
  } else {
    g_scope.beat_ms = (now - g_scope.beat_ms < 2u * interval) ? g_scope.beat_ms + interval : now;
  }
  g_scope.acc.reset(g_scope.plan.points);
  // Nothing of either is carried into the next frame: `us`, `usx` and `dev` are this frame's
  // sweeps and this frame's arms.
  g_scope.time.reset();
  g_scope.dev = 0;
}

// One slice of a **stepped** sweep, which is what a pass of loop() does for a
// `scope ... stepped`: every point tuned with the chip's own command, stood on and read once
// (SteppedScope, src/roles/scanner/Sweep.cpp - the order and the cursor are host-tested). A sweep
// counts only when its last point has gone in, so a `stop` in the middle of one leaves no frame.
void serviceSteppedScope() {
  ScopeArmResult out;
  bool whole = false;
  if (!g_scope.stepped.slice(g_arm, g_scope.plan, g_scope.cal, g_scope.acc, g_scope.time, out,
                             whole)) {
    if (out.failed == kScopeArmNone) {
      error("scope segment %u of %u is not a segment", unsigned(g_scope.stepped.segment()),
            unsigned(g_scope.plan.segments));
      stop();
      return;
    }
    // Where it failed: the point the cursor stands on, which is the one that was being measured.
    ScopeSegment seg;
    double at_mhz = g_scope.plan.first_mhz;
    if (scopeSegment(g_scope.plan, g_scope.stepped.segment(), seg)) {
      at_mhz = seg.first_mhz + double(g_scope.stepped.point()) * g_scope.plan.step_khz / 1000.0;
    }
    const bool measuring = out.failed == kScopeArmPoint || out.failed == kScopeArmRead;
    scopeError(measuring ? "sweep" : "setup", out.failed, out.code, at_mhz, out.reloaded);
    stop();
    return;
  }
  // What the chip's error word held at this slice's arm, if it armed one: the frame's `dev`, as
  // for a plugin sweep (0 for a plan of one segment, which has no standby to read it in).
  g_scope.dev |= out.before_errors;
  if (!whole) return;  // the rest of the sweep in the next passes of loop()
  g_scope.acc.sweepDone();
  g_scope.time.sweepDone();  // a whole stepped sweep, which is what `us` and `usx` are of
  const uint32_t now = millis();
  if (g_scope.plan.interval_ms != 0 && int32_t(now - g_scope.beat_ms) < 0) return;
  emitScopeFrame(now);
}

void serviceScope() {
  // The other way to sweep, which a `scope` line asks for by name and nothing else does.
  if (g_scope.plan.mode == kScopeModeStepped) {
    serviceSteppedScope();
    return;
  }
  ScopeSegment seg;
  if (!scopeSegment(g_scope.plan, g_scope.segment, seg)) {
    error("scope segment %u of %u is not a segment", unsigned(g_scope.segment),
          unsigned(g_scope.plan.segments));
    stop();
    return;
  }
  if (g_scope.armed != g_scope.segment) {
    ScopeArmResult arm;
    if (!armScopeSegment(g_arm, g_scope.plan, seg, g_scope.cal, arm)) {
      scopeError("setup", arm.failed, arm.code, seg.first_mhz, arm.reloaded);
      stop();
      return;
    }
    // What the chip's error word held when this arm reached standby, which is what the sweep
    // before it left there: the frame's `dev` is the OR of them (0 for a plan of one segment,
    // which is never given a standby to read it in).
    g_scope.dev |= arm.before_errors;
    g_scope.armed = g_scope.segment;
  } else {
    // A one-segment plan keeps its parameters from sweep to sweep, so nothing re-arms it and the
    // settle would be paid once and never again - while the receiver comes to every sweep but
    // the first from the *last* point of the one before it, a whole span away. So a segment that
    // is already armed stands on its first point too, before every sweep.
    ScopeArmResult settled;
    if (!settleScopeSegment(g_arm, g_scope.plan, seg, settled)) {
      scopeError("setup", settled.failed, settled.code, seg.first_mhz, false);
      stop();
      return;
    }
  }
  // Blocking for the segment's duration, timed by the clock the seam carries and nothing else -
  // no arm, no settle, nothing between two sweeps - which is what `us` and `usx` are
  // (sweepScopeSegment, src/roles/scanner/Sweep.cpp). The plugin's tag is checked inside the
  // call: a chip that was reset under us - a brownout - has no 0x9B command and must not be sent
  // one.
  ScopeArmResult swept;
  if (!sweepScopeSegment(g_arm, g_scope.levels, g_scope.time, swept)) {
    if (swept.code == RADIOLIB_ERR_CHIP_NOT_FOUND) {
      error("scope: the sweep plugin is gone (the chip was reset); scope ended");
    } else {
      scopeError("sweep", swept.failed, swept.code, seg.first_mhz, false);
    }
    stop();
    return;
  }
  g_scope.acc.add(seg.first, g_scope.levels, seg.points);
  g_scope.segment++;
  if (g_scope.segment < g_scope.plan.segments) return;  // the rest of the span first

  g_scope.segment = 0;
  if (g_scope.plan.segments > 1) g_scope.armed = 0xFFFF;  // the next sweep re-arms segment 0
  g_scope.acc.sweepDone();
  g_scope.time.sweepDone();  // a whole sweep, and the only kind `us` and `usx` are of

  const uint32_t now = millis();
  if (g_scope.plan.interval_ms != 0 && int32_t(now - g_scope.beat_ms) < 0) return;
  emitScopeFrame(now);
}

void serviceDwell() {
  const uint32_t now_us = micros();
  if (int32_t(g_dwell.next_sample_us - now_us) <= 0) {
    g_dwell.window.add(g_radio.getRSSI(false));
    g_dwell.next_sample_us += MON_SAMPLE_US;
  }
  const uint32_t now = millis();
  if (now - g_dwell.window_started_ms >= g_dwell.window_ms && g_dwell.window.count() > 0) {
    DwellFields f;
    f.meta = nextMeta();
    f.f_mhz = g_dwell.f_mhz;
    f.win_ms = g_dwell.window_ms;
    g_dwell.window.stats(f.stats);
    g_lines->dwell(f);
    g_dwell.window.reset();
    g_dwell.window_started_ms = now;
  }
  if (int32_t(g_dwell.end_ms - now) <= 0) {
    g_lines->done("dwell", nextMeta());
    toHomeRx();
    g_mode = kModeMonitor;
  }
}

void serviceCad() {
  for (int i = 0; i < 4 && g_cad.done < g_cad.target; i++) {
    const int16_t st = g_radio.scanChannel();
    if (st == RADIOLIB_LORA_DETECTED) g_cad.detections++;
    g_cad.done++;
    inputPoll();
  }
  if (g_cad.done >= g_cad.target) {
    CadFields f;
    f.meta = nextMeta();
    f.f_mhz = g_cad.f_mhz;
    f.sf = g_home.sf;
    f.bw_khz = g_home.bw_khz;
    f.n = g_cad.target;
    f.detections = g_cad.detections;
    g_lines->cad(f);
    toHomeRx();
    g_mode = kModeMonitor;
  }
}

void serviceMonitor() {
  const uint32_t now_us = micros();
  if (int32_t(g_monitor_next_us - now_us) <= 0) {
    g_monitor.add(g_radio.getRSSI(false));
    g_monitor_next_us += MON_SAMPLE_US;
    // Resync if the pace fell behind - a long serial write, a scan step.
    if (int32_t(now_us - g_monitor_next_us) > 100000) g_monitor_next_us = now_us;
  }
}

}  // namespace

const char* modeName(Mode mode) {
  switch (mode) {
    case kModeSweep: return "sweep";
    case kModeScope: return "scope";
    case kModeHist: return "hist";
    case kModeDwell: return "dwell";
    case kModeCad: return "cad";
    default: return "monitor";
  }
}

bool begin(Lines& lines) {
  g_lines = &lines;
  // The SX1262 inside the RAK4630 stamp is unpowered until this pin is driven high, and after a
  // reset the GPIO is an input: MeshCore's own board does exactly this before it talks to the
  // radio (variants/rak4631/RAK4631Board.cpp:58-66 at the pin 81dbae59 - pinMode, then high,
  // then 10 ms "give sx1262 some time to power up"). NoiseScope never drove it, which is the one
  // thing this port changes about its bring-up (tasks/phase-3/reports/02.md).
  pinMode(SX126X_POWER_EN, OUTPUT);
  digitalWrite(SX126X_POWER_EN, HIGH);
  delay(10);

  SPI.setPins(P_LORA_MISO, P_LORA_SCLK, P_LORA_MOSI);
  SPI.begin();
  readRom();
  g_ok = toHomeRx();
  g_mode = kModeMonitor;
  return g_ok;
}

Mode mode() { return g_mode; }
bool ok() { return g_ok; }

bool startSweep(double f0, double f1, double step_khz, uint32_t dwell_ms, int32_t passes) {
  if (!g_ok || !sweepDwellOk(dwell_ms)) return false;
  if (!planFor(f0, f1, step_khz, passes)) return false;
  g_sweep.dwell_ms = uint16_t(dwell_ms);
  const int st = beginFsk(g_sweep.plan.f0, g_sweep.plan.rbw_khz);
  if (st != RADIOLIB_ERR_NONE) {
    error("fsk init failed (%d)", st);
    toHomeRx();
    return false;
  }
  g_mode = kModeSweep;
  return true;
}

bool startHist(double f0, double f1, double step_khz, uint32_t samples, int32_t passes) {
  if (!g_ok) return false;
  if (!planFor(f0, f1, step_khz, passes)) return false;
  g_sweep.samples = clampHistSamples(samples);
  int st = beginFsk(g_sweep.plan.f0, g_sweep.plan.rbw_khz);
  if (st == RADIOLIB_ERR_NONE) {
    st = g_radio.uploadPatch(sx126x_patch_scan, sizeof(sx126x_patch_scan));
  }
  if (st != RADIOLIB_ERR_NONE) {
    error("hist init/patch failed (%d)", st);
    g_radio.reset();
    toHomeRx();
    return false;
  }
  g_mode = kModeHist;
  return true;
}

bool startScope(const ScopeRequest& request, Lines& out, ScopePlan& plan) {
  if (!g_ok) return false;
  // The ROM gate, here as well as in main.cpp's `err`: the plugin is made for one ROM and its
  // author verified it on that one, and a command for a ROM that does not have it would run
  // whatever happens to be in the chip's program RAM.
  if (!scopeRomSupported(g_rom)) return false;
  ScopePlan planned;
  if (!planScope(request, planned)) return false;

  // This scope's own regulator and gain, and nothing of it outlives the mode: the words are in
  // `planned` and the mode after this one begins the chip as it always did.
  const int st = beginPlugin(planned.f0, planned.reg, planned.gain);
  if (st != RADIOLIB_ERR_NONE) {
    error("scope init failed (%d)", st);
    g_scanner.end();
    g_radio.reset();  // the plugin never outlives the mode, uploaded or not
    toHomeRx();
    return false;
  }

  g_scope.plan = planned;
  g_scope.out = &out;
  g_scope.segment = 0;
  g_scope.armed = 0xFFFF;
  g_scope.stepped.restart();         // and a stepped sweep begins at segment 0, point 0
  g_scope.cal = ScopeCalibration();  // beginPlugin() reset the chip, and the calibration with it
  g_scope.acc.reset(planned.points);
  g_scope.time.reset();
  g_scope.dev = 0;
  const uint32_t now = millis();
  g_scope.beat_ms = now;
  g_scope.emitted_ms = now;
  g_mode = kModeScope;
  plan = planned;
  return true;
}

const char* rom() { return g_rom; }

bool startDwell(double f_mhz, uint32_t secs, uint32_t win_ms) {
  if (!g_ok || !frequencyOk(f_mhz)) return false;
  g_dwell.f_mhz = f_mhz;
  g_dwell.end_ms = millis() + clampDwellSeconds(secs) * 1000u;
  g_dwell.window_ms = clampDwellWindowMs(win_ms);
  g_dwell.window_started_ms = millis();
  g_dwell.window.restart();
  g_dwell.next_sample_us = micros();
  retuneRx(f_mhz);  // still in LoRa mode: the bandwidth is the home channel's own filter
  g_mode = kModeDwell;
  return true;
}

bool startCad(double f_mhz, uint32_t count) {
  if (!g_ok || !frequencyOk(f_mhz)) return false;
  g_cad.f_mhz = f_mhz;
  g_cad.target = clampCadCount(count);
  g_cad.done = 0;
  g_cad.detections = 0;
  g_radio.standby();
  g_radio.setFrequency(float(f_mhz));
  g_mode = kModeCad;
  return true;
}

void stop() {
  if (g_mode == kModeHist) {
    g_radio.spectralScanAbort();
    g_radio.reset();  // purge the RAM patch before going back to LoRa
  }
  if (g_mode == kModeScope) {
    g_scanner.end();
    g_radio.reset();  // and the sweep plugin, the same way: a reset of the chip removes it
    g_scope.out = nullptr;
    // And the two words with it: toHomeRx() below begins the chip with the build's regulator and
    // the gain `boost on|off` left, because modeRadioSetup() is what it asks for. However a
    // scope ends - `stop`, `scope off`, another command, an error - the radio is what it was.
    g_scope.plan = ScopePlan();
  }
  if (g_mode != kModeMonitor) {
    toHomeRx();
    g_mode = kModeMonitor;
  }
}

bool setHome(double f_mhz, double bw_khz, uint8_t sf, uint8_t cr) {
  if (!homeSettingsOk(f_mhz, sf, cr)) return false;
  g_home.f_mhz = f_mhz;
  g_home.bw_khz = bw_khz;
  g_home.sf = sf;
  g_home.cr = cr;
  if (g_mode == kModeMonitor) return toHomeRx();
  return true;
}

HomeArgs home() { return g_home; }

bool setBoost(bool on) {
  // The setting and the chip both, as before: `boost on|off` is the operator's word about the
  // gain and it is obeyed in whatever mode is running. A `scope` that asked for a gain of its own
  // gets that gain back at its next reload, and the setting is what the mode after it takes.
  g_boost = on;
  return g_radio.setRxBoostedGainMode(on) == RADIOLIB_ERR_NONE;
}

bool boost() { return g_boost; }

bool monitorStats(WindowStats& out, bool reset) {
  if (!g_monitor.stats(out)) return false;
  if (reset) g_monitor.reset();
  return true;
}

void service() {
  if (!g_ok) return;
  switch (g_mode) {
    case kModeSweep: serviceSweep(); break;
    case kModeScope: serviceScope(); break;
    case kModeHist: serviceHist(); break;
    case kModeDwell: serviceDwell(); break;
    case kModeCad: serviceCad(); break;
    default: serviceMonitor(); break;
  }
}

}  // namespace radio
}  // namespace scanner
}  // namespace twinoak
