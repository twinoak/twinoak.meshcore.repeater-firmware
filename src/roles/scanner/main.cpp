// ------------------------------------------------------------------------------------------------
// NoiseScope - the SX1262 spectrum and noise scanner, as the role `scanner` of this family.
//
// Diagnostic firmware, flashed in place of the repeater when a site has a noise problem: it
// streams NDJSON over the Walter management UART (and USB CDC when a host is attached), takes
// text commands on either, and goes back to passive monitoring whenever anything finishes.
// docs/noisescope.md is the page; spec/LINK-PROTOCOL.md section 9 is how a Walter tells this
// image from a repeater, and phase 3's task 02 is how it got here.
//
// **It never transmits and it never writes flash.** The first is what keeps it legal on a mast
// with somebody else's licence beside it; the second is what makes flashing it and flashing the
// repeater back a round trip that keeps the node's identity, its name and its settings. Both are
// rails-test lines over this directory (test/test_rails) rather than promises in a comment.
//
// setup() and loop() are this role's, as src/roles/repeater/main.cpp is the repeater's. What is
// shared with the repeater is src/common/: the image descriptor these lines read their `role`,
// `board` and `ver` out of, the INA3221 with this board's three shunt values, and the band table
// the home channel and the default sweep come from.
// ------------------------------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>

#include "common/fwdesc/ImageDescriptor.h"
#include "roles/scanner/Commands.h"
#include "roles/scanner/Lines.h"
#include "roles/scanner/Radio.h"
#include "roles/scanner/ScannerHeaders.h"
#include "roles/scanner/Sweep.h"
#include "roles/scanner/Telemetry.h"
#include "roles/scanner/Wdt.h"
#include "roles/scanner/config.h"

namespace twinoak {
namespace scanner {
namespace {

// --- where a line goes --------------------------------------------------------------------------

// Which ports a line goes to. The Walter's UART always and USB CDC when a host is attached is
// what every line of this firmware has always done, and `g_lines` is still that - but a `scope`
// stream goes to **the port its command came in on and only there**: 115200 baud and USB CDC are
// not the same pipe, a waterfall belongs to the laptop that asked for it, and the other port
// must keep getting its `stat` (docs/noisescope.md section 5). So there are three sinks over the
// same two ports, and the one difference between them is which of the two they write to.
//
// `Serial` on this core is non-blocking with nobody on the other end, but the attached state is
// still read once per loop() and not once per fragment.
bool g_usb_attached = false;

class PortSink : public LineSink {
 public:
  PortSink(bool walter, bool usb) : walter_(walter), usb_(usb) {}

  void write(const char* text) override {
    if (walter_) WALTER_SERIAL.write(text);
    if (usb_ && g_usb_attached) Serial.write(text);
  }

 private:
  const bool walter_;
  const bool usb_;
};

PortSink g_sink_both(true, true);
PortSink g_sink_walter(true, false);
PortSink g_sink_usb(false, true);

Lines g_lines(g_sink_both);
Lines g_lines_walter(g_sink_walter);
Lines g_lines_usb(g_sink_usb);

void beginPorts() {
  WALTER_SERIAL.begin(WALTER_BAUD);
  Serial.begin(115200);  // USB CDC; non-blocking if no host
}

// Where a command came from, which is where its `scope` frames go back to.
enum Source : uint8_t { kFromWalter = 0, kFromUsb };

// --- the two line buffers -----------------------------------------------------------------------

// One per source, so that a command from the Walter and one from a laptop cannot interleave into
// a third command. A buffer that holds a finished line stops reading: the rest of the bytes wait
// in the UART's own ring (platformio.ini sizes it), which is how nothing is lost while a scan
// step is running.
struct LineBuffer {
  char buf[kMaxLineBytes];
  uint8_t len = 0;
  bool ready = false;
};

LineBuffer g_from_walter;
LineBuffer g_from_usb;

void feed(LineBuffer& line, Stream& from) {
  if (line.ready) return;
  while (from.available()) {
    const char c = char(from.read());
    if (c == '\n' || c == '\r') {
      if (line.len > 0) {
        line.buf[line.len] = 0;
        line.ready = true;
        return;
      }
    } else if (line.len < sizeof(line.buf) - 1) {
      line.buf[line.len++] = c;
    }
  }
}

// --- what this image is -------------------------------------------------------------------------

// Read out of the image's own 96 bytes and not out of the defines that made them: the firmware
// references its descriptor, which is what keeps the linker from discarding it and what makes
// the image and the unit unable to disagree (phase 3's task 01; tools/pio/descriptor.py checks
// the bytes afterwards).
const char* imageRoleName() { return fwdesc::roleName(fwdesc::imageRole()); }

char g_reset_reason[8] = "por";

void captureResetReason() {
  const uint32_t reason = NRF_POWER->RESETREAS;
  NRF_POWER->RESETREAS = reason;  // write 1 to clear
  if (reason & POWER_RESETREAS_DOG_Msk) {
    strcpy(g_reset_reason, "wdt");
  } else if (reason & POWER_RESETREAS_SREQ_Msk) {
    strcpy(g_reset_reason, "soft");
  } else if (reason & POWER_RESETREAS_LOCKUP_Msk) {
    strcpy(g_reset_reason, "lockup");
  } else if (reason & POWER_RESETREAS_RESETPIN_Msk) {
    strcpy(g_reset_reason, "pin");
  } else {
    strcpy(g_reset_reason, "por");
  }
}

BootFields bootFields() {
  BootFields f;
  f.fw = FW_NAME;
  f.version = fwdesc::imageVersion();
  f.role = imageRoleName();
  f.board = fwdesc::imageBoard();
  f.hw = "rak4630";
  f.radio = "sx1262";
  f.reset = g_reset_reason;
  f.radio_ok = radio::ok();
  f.rtc = telemetry::rtcPresent();
  f.ina = telemetry::inaPresent();
  f.ts = telemetry::epoch();
  return f;
}

// --- the periodic things ------------------------------------------------------------------------

uint32_t g_stat_next_ms = 0;
uint32_t g_auto_minutes = AUTO_SWEEP_MIN_DEFAULT;
uint32_t g_auto_last_ms = 0;

ScanDefaults scanDefaults() {
  ScanDefaults d;
  d.f0 = SWEEP_F0_MHZ;
  d.f1 = SWEEP_F1_MHZ;
  d.step_khz = SWEEP_STEP_KHZ;
  d.sweep_dwell_ms = SWEEP_DWELL_MS;
  d.hist_samples = HIST_SAMPLES_DEFAULT;
  return d;
}

void emitStat() {
  const HomeArgs home = radio::home();
  StatFields f;
  f.meta.ts = telemetry::epoch();
  f.meta.ms = millis();
  f.uptime_s = millis() / 1000u;
  f.mode = radio::modeName(radio::mode());
  f.home_f_mhz = home.f_mhz;
  f.have_rssi = radio::mode() == radio::kModeMonitor && radio::monitorStats(f.rssi, true);
  f.pwr = telemetry::power();
  g_lines.stat(f);
}

void emitInfo() {
  const HomeArgs home = radio::home();
  InfoFields f;
  f.boot = bootFields();
  f.build = __DATE__;
  f.devid_high = NRF_FICR->DEVICEID[1];
  f.devid_low = NRF_FICR->DEVICEID[0];
  f.boosted_gain = radio::boost();
  f.auto_minutes = g_auto_minutes;
  f.rom = radio::rom();
  f.home_f_mhz = home.f_mhz;
  f.home_bw_khz = home.bw_khz;
  f.home_sf = home.sf;
  f.home_cr = home.cr;
  g_lines.info(f);
}

// --- the commands -------------------------------------------------------------------------------

void handleCommand(char* line, Source from) {
  const Tokens tokens = tokenize(line);
  if (tokens.count == 0) return;
  const Verb verb = verbOf(tokens.at[0]);
  const char* name = verb == kVerbUnknown ? tokens.at[0] : nameOf(verb);

  switch (verb) {
    case kVerbHelp:
      g_lines.help(fwdesc::imageVersion());
      return;

    case kVerbInfo:
      emitInfo();
      return;

    case kVerbStat:
      emitStat();
      return;

    case kVerbStop:
      radio::stop();
      g_lines.ack(name, true, "monitor");
      return;

    case kVerbSweep:
    case kVerbHist: {
      const bool is_hist = verb == kVerbHist;
      const ScanArgs args = parseScan(tokens, is_hist, scanDefaults());
      radio::stop();
      const bool ok = is_hist ? radio::startHist(args.f0, args.f1, args.step_khz, args.fourth,
                                                 args.passes)
                              : radio::startSweep(args.f0, args.f1, args.step_khz, args.fourth,
                                                  args.passes);
      g_lines.ack(name, ok, ok ? "started" : "bad params or radio error");
      return;
    }

    case kVerbScope: {
      ScopeArgs args;
      if (!parseScope(tokens, args)) {
        g_lines.ack(name, false, usageOf(verb));
        return;
      }
      radio::stop();  // and the plugin goes with it, whatever is being started next
      if (args.off) {
        g_lines.ack(name, true, "monitor");
        return;
      }
      // The ROM gate, before anything is uploaded: the plugin is a program for the radio's own
      // core, made for one ROM and verified by its author on that one. On any other chip this
      // says which one it met and sends it nothing (docs/noisescope.md section 6).
      if (!scopeRomSupported(radio::rom())) {
        char msg[kMaxAckMessageBytes];
        snprintf(msg, sizeof(msg),
                 "scope needs the radio ROM '%s' and this chip says '%s'; nothing was uploaded -"
                 " use sweep for a stepped spectrum instead",
                 kScopeRom, radio::rom());
        g_lines.err(msg);
        g_lines.ack(name, false, "unsupported radio rom; sweep is the fallback");
        return;
      }
      ScopeRequest request;
      request.f0 = args.f0;
      request.f1 = args.f1;
      request.points = args.points;
      request.rbw_khz = args.rbw_khz;
      request.interval_ms = args.interval_ms;
      request.agg = args.agg;
      // The sink's own floor: the Walter's UART is 115200 baud and a frame has to fit inside a
      // fraction of the interval, while USB CDC is asked for every sweep there is.
      request.sink_baud = (from == kFromWalter) ? uint32_t(WALTER_BAUD) : 0u;
      ScopePlan plan;
      const bool ok =
          radio::startScope(request, from == kFromWalter ? g_lines_walter : g_lines_usb, plan);
      char msg[kMaxAckMessageBytes];
      if (ok) {
        // What was taken, which is not always what was asked: the points, the step they make,
        // the bandwidth, how many segments the span was cut into, how much of it a reading
        // covers and the interval the sink allowed.
        snprintf(msg, sizeof(msg),
                 "n=%u step=%.4fkHz rbw=%.1fkHz seg=%u cov=%.3f interval=%lums sweep=%lums",
                 unsigned(plan.points), plan.step_khz, plan.rbw_khz, unsigned(plan.segments),
                 plan.cov, (unsigned long)plan.interval_ms, (unsigned long)plan.sweep_ms);
      }
      g_lines.ack(name, ok, ok ? msg
                               : "refused: 150 <= f0 < f1 <= 960 MHz, points 16..2048,"
                                 " interval 0..60000 ms - or the radio refused the mode");
      return;
    }

    case kVerbDwell: {
      DwellArgs args;
      if (!parseDwell(tokens, args)) {
        g_lines.ack(name, false, usageOf(verb));
        return;
      }
      radio::stop();
      const bool ok = radio::startDwell(args.f_mhz, args.secs, args.win_ms);
      g_lines.ack(name, ok, ok ? "started" : "bad params");
      return;
    }

    case kVerbCad: {
      const CadArgs args = parseCad(tokens, radio::home().f_mhz);
      radio::stop();
      const bool ok = radio::startCad(args.f_mhz, args.count);
      g_lines.ack(name, ok, ok ? "started" : "bad params");
      return;
    }

    case kVerbHome: {
      HomeArgs args;
      if (!parseHome(tokens, radio::home(), args)) {
        g_lines.ack(name, false, usageOf(verb));
        return;
      }
      const bool ok = radio::setHome(args.f_mhz, args.bw_khz, args.sf, args.cr);
      g_lines.ack(name, ok, ok ? "retuned" : "bad params");
      return;
    }

    case kVerbBoost: {
      bool on = false;
      if (!parseBoost(tokens, on)) {
        g_lines.ack(name, false, usageOf(verb));
        return;
      }
      const bool ok = radio::setBoost(on);
      g_lines.ack(name, ok, on ? "boosted gain on" : "boosted gain off");
      return;
    }

    case kVerbAuto: {
      uint32_t minutes = 0;
      if (!parseAuto(tokens, minutes)) {
        g_lines.ack(name, false, usageOf(verb));
        return;
      }
      g_auto_minutes = minutes;
      g_auto_last_ms = millis();
      g_lines.ack(name, true, minutes != 0 ? "auto-sweep on" : "auto-sweep off");
      return;
    }

    case kVerbTime: {
      if (tokens.count < 2) {
        g_lines.ack(name, false, usageOf(verb));
        return;
      }
      uint32_t epoch = 0;
      const bool ok = parseEpoch(tokens, epoch) && telemetry::setEpoch(epoch);
      g_lines.ack(name, ok, ok ? "clock set" : "bad epoch or rtc write failed");
      return;
    }

    case kVerbReboot:
      g_lines.ack(name, true, "rebooting");
      delay(50);  // let the last line leave the UART
      NVIC_SystemReset();
      return;

    case kVerbDfu: {
      const bool serial_only = parseDfuSerialOnly(tokens);
      g_lines.ack(name, true, serial_only ? "entering serial DFU" : "entering UF2 bootloader");
      delay(50);
      // The Adafruit bootloader's two magic words (its own boot.c): serial-only DFU, and the
      // UF2 drive. Nothing of this firmware is written to flash by either - the bootloader is
      // what writes, and only what is handed to it afterwards.
      NRF_POWER->GPREGRET = serial_only ? 0x4E : 0x57;
      NVIC_SystemReset();
      return;
    }

    case kVerbUnknown:
    default:
      g_lines.ack(name, false, "unknown command, try: help");
      return;
  }
}

void checkCommands() {
  if (g_from_walter.ready) {
    handleCommand(g_from_walter.buf, kFromWalter);
    g_from_walter.len = 0;
    g_from_walter.ready = false;
  }
  if (g_from_usb.ready) {
    handleCommand(g_from_usb.buf, kFromUsb);
    g_from_usb.len = 0;
    g_from_usb.ready = false;
  }
}

}  // namespace

// Called from a blocking scan step as well, which is why a `stop` always arrives
// (src/roles/scanner/Radio.h).
void inputPoll() {
  feed(g_from_walter, WALTER_SERIAL);
  feed(g_from_usb, Serial);
}

void setup() {
  captureResetReason();
  beginPorts();
  wdtBegin();
  delay(100);  // let the 3V3 rail and the TCXO settle after a power-kill cycle
  telemetry::begin();
  radio::begin(g_lines);

  g_lines.boot(bootFields());

  g_stat_next_ms = millis() + STAT_INTERVAL_MS;
  g_auto_last_ms = millis();
}

void loop() {
  wdtFeed();
  g_usb_attached = (bool)Serial;
  inputPoll();
  checkCommands();
  radio::service();
  telemetry::loop();

  const uint32_t now = millis();
  if (int32_t(now - g_stat_next_ms) >= 0) {
    emitStat();
    g_stat_next_ms += STAT_INTERVAL_MS;
  }

  if (g_auto_minutes > 0 && radio::mode() == radio::kModeMonitor &&
      now - g_auto_last_ms >= g_auto_minutes * 60000u) {
    g_auto_last_ms = now;
    const ScanDefaults d = scanDefaults();
    if (radio::startSweep(d.f0, d.f1, d.step_khz, d.sweep_dwell_ms, 1)) {
      g_lines.ack("auto-sweep", true, "");
    }
  }
}

}  // namespace scanner
}  // namespace twinoak

// The Arduino core calls these two, and they must be at the global scope.
void setup() { twinoak::scanner::setup(); }
void loop() { twinoak::scanner::loop(); }
