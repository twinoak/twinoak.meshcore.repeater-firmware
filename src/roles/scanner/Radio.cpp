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
  uint32_t beat_ms = 0;      // when the next frame is due
  uint32_t emitted_ms = 0;   // when the last one actually left, which is what `sps` is counted on
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

// The four settings that make a reading comparable with what the repeater hears. The RF switch
// and the TCXO are arguments of begin()/beginFSK() below; these two are calls of their own, and
// they are made again after every mode change because beginFSK() and begin() reset the chip's
// configuration.
void applyRxPath() {
  g_radio.setCurrentLimit(SX126X_CURRENT_LIMIT);
  g_radio.setDio2AsRfSwitch(SX126X_DIO2_AS_RF_SWITCH);
  g_radio.setRxBoostedGainMode(g_boost);
}

// The 10 is the transmit power begin() insists on having; nothing ever transmits with it
// (test/test_rails). The retry without a TCXO voltage is for a module whose crystal is not a
// TCXO, which answers the SetDIO3AsTCXOCtrl command with an error.
int beginLora(double f, double bw, uint8_t sf, uint8_t cr) {
  int st = g_radio.begin(float(f), float(bw), sf, cr, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 10, 16,
                         SX126X_DIO3_TCXO_VOLTAGE, false);
  if (st == RADIOLIB_ERR_SPI_CMD_FAILED || st == RADIOLIB_ERR_SPI_CMD_INVALID) {
    st = g_radio.begin(float(f), float(bw), sf, cr, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 10, 16,
                       0.0f, false);
  }
  if (st == RADIOLIB_ERR_NONE) applyRxPath();
  return st;
}

int beginFsk(double f, double rbw_khz) {
  int st = g_radio.beginFSK(float(f), 4.8f, 5.0f, float(rbw_khz), 10, 16,
                            SX126X_DIO3_TCXO_VOLTAGE, false);
  if (st == RADIOLIB_ERR_SPI_CMD_FAILED || st == RADIOLIB_ERR_SPI_CMD_INVALID) {
    st = g_radio.beginFSK(float(f), 4.8f, 5.0f, float(rbw_khz), 10, 16, 0.0f, false);
  }
  if (st == RADIOLIB_ERR_NONE) applyRxPath();
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

void serviceHist() {
  const double f = pointFrequencyMhz(g_sweep.plan, g_sweep.index);
  g_radio.standby();
  g_radio.setFrequency(float(f));
  int st = g_radio.spectralScanStart(g_sweep.samples);
  if (st != RADIOLIB_ERR_NONE) {
    error("scan start failed (%d) at %.3f", st, f);
    stop();
    return;
  }
  // 8.2 us per sample, and a generous margin.
  const uint32_t deadline = millis() + (g_sweep.samples / 100) + 100;
  bool done = false;
  while (int32_t(deadline - millis()) > 0) {
    if (g_radio.spectralScanGetStatus() == RADIOLIB_ERR_NONE) {
      done = true;
      break;
    }
    inputPoll();
    yield();
  }
  if (!done) {
    g_radio.spectralScanAbort();
    error("scan timeout at %.3f", f);
    stop();
    return;
  }
  uint16_t bins[RADIOLIB_SX126X_SPECTRAL_SCAN_RES_SIZE];
  st = g_radio.spectralScanGetResult(bins);
  if (st != RADIOLIB_ERR_NONE) {
    error("scan result failed (%d) at %.3f", st, f);
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

// Everything a segment needs before its sweep: the image rejection calibrated for *its* range
// and not for the whole span's centre, and the plugin's parameters for its own points.
//
// The order matters and is the one place this differs from the fork, which has one segment and
// lets setSweep() calibrate. `setFrequency()` recalibrates by itself when the frequency moves by
// 20 MHz or more (RadioLib 7.8.0, SX1262::setFrequency / RADIOLIB_SX126X_CAL_IMG_FREQ_TRIG_MHZ),
// and it would then calibrate for one of Semtech's five bands or +-4 MHz around the centre. So
// the centre is set first - which is what setSweep() computes too, so its own call is then a
// move of zero and calibrates nothing - and our range is calibrated for last, in standby, where
// the CalibrateImage command belongs.
int armSegment(const ScopeSegment& seg) {
  const float centre = float(seg.first_mhz) +
                       float(g_scope.plan.step_khz / 1000.0) * float(seg.points) / 2.0f;
  int st = g_radio.standby();
  if (st == RADIOLIB_ERR_NONE) st = g_radio.setFrequency(centre);
  if (st == RADIOLIB_ERR_NONE) {
    st = g_radio.calibrateImageRejection(float(seg.cal_lo_mhz), float(seg.cal_hi_mhz));
  }
  if (st == RADIOLIB_ERR_NONE) {
    st = g_scanner.setSweep(float(seg.first_mhz), float(g_scope.plan.step_khz / 1000.0),
                            uint8_t(seg.points), float(g_scope.plan.rbw_khz));
  }
  return st;
}

void emitScopeFrame(uint32_t now) {
  g_scope.acc.take(g_scope.plan.agg, g_scope.frame);
  ScopeFields f;
  f.meta = nextMeta();
  f.plan = g_scope.plan;
  f.sweeps = g_scope.acc.sweeps();
  f.sps_x10 = scopeSweepsPerSecondX10(f.sweeps, now - g_scope.emitted_ms);
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
}

void serviceScope() {
  ScopeSegment seg;
  if (!scopeSegment(g_scope.plan, g_scope.segment, seg)) {
    error("scope segment %u of %u is not a segment", unsigned(g_scope.segment),
          unsigned(g_scope.plan.segments));
    stop();
    return;
  }
  if (g_scope.armed != g_scope.segment) {
    const int st = armSegment(seg);
    if (st != RADIOLIB_ERR_NONE) {
      error("scope setup failed (%d) at %.4f", st, seg.first_mhz);
      stop();
      return;
    }
    g_scope.armed = g_scope.segment;
  }
  // Blocking for the segment's duration, and the plugin's tag is checked inside: a chip that was
  // reset under us - a brownout - has no 0x9B command and must not be sent one.
  const int st = g_scanner.sweep(g_scope.levels);
  if (st != RADIOLIB_ERR_NONE) {
    if (st == RADIOLIB_ERR_CHIP_NOT_FOUND) {
      error("scope: the sweep plugin is gone (the chip was reset); scope ended");
    } else {
      error("scope sweep failed (%d) at %.4f", st, seg.first_mhz);
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
  // radio (variants/rak4631/RAK4631Board.cpp:58-66 at the pin d9296435 - pinMode, then high,
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

  #ifdef SX126X_USE_REGULATOR_LDO
  const bool ldo = (SX126X_USE_REGULATOR_LDO != 0);
  #else
  const bool ldo = false;
  #endif
  int st = g_scanner.begin(float(planned.f0), SX126X_DIO3_TCXO_VOLTAGE, ldo, g_boost);
  if (st != RADIOLIB_ERR_NONE) {
    error("scope init failed (%d)", st);
    g_scanner.end();
    g_radio.reset();  // the plugin never outlives the mode, uploaded or not
    toHomeRx();
    return false;
  }
  // The current limit, which the vendored begin() does not set: the RX path of a reading is the
  // repeater's, setting for setting (src/roles/scanner/config.h). The RF switch and the boosted
  // gain it does set, to the same values this would.
  applyRxPath();

  g_scope.plan = planned;
  g_scope.out = &out;
  g_scope.segment = 0;
  g_scope.armed = 0xFFFF;
  g_scope.acc.reset(planned.points);
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
