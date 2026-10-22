#include "roles/scanner/Lines.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

namespace twinoak {
namespace scanner {
namespace {

// NoiseScope's out_printf buffer. A line longer than this - a `sweep` pass is about 2.5 kB - is
// written in fragments, so nothing here has to hold a whole line.
constexpr size_t kFragmentBytes = kLineFragmentBytes;

// RFC 4648 section 4, which is what base64 means to every reader of a `scope` frame: Python's
// base64.b64decode, JavaScript's atob, .NET's Convert.FromBase64String.
const char kBase64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 1 and 0 where docs/noisescope.md's `info` has them, `true` and `false` where its `boot` has
// them. Not the same field spelled twice by accident: it is what NoiseScope sent, and a reader
// of its lines already parses both.
int asFlag(bool value) { return value ? 1 : 0; }
const char* asBool(bool value) { return value ? "true" : "false"; }

}  // namespace

void Lines::text(const char* fmt, ...) {
  char buf[kFragmentBytes];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  sink_.write(buf);
}

void Lines::number(double value, int decimals) {
  // JSON has no NaN and no infinity, and a line a parser drops carries nothing at all. `null` is
  // the honest answer for a reading that is not a number - a window with no sample, a radio that
  // answered nothing - and it is what test/test_scanner holds every line to.
  if (!isfinite(value)) {
    raw("null");
    return;
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "%.*f", decimals, value);
  raw(buf);
}

void Lines::field(const char* name, double value, int decimals) {
  text("\"%s\":", name);
  number(value, decimals);
}

void Lines::meta(const Meta& m) {
  text("\"seq\":%lu,\"ts\":%lu,\"ms\":%lu", (unsigned long)m.seq, (unsigned long)m.ts,
       (unsigned long)m.ms);
}

void Lines::bootBody(const BootFields& f) {
  // The image's own three - role, board and version - are read out of its descriptor by the
  // caller (src/roles/scanner/main.cpp), so what the unit says and what the image says cannot
  // differ (phase 3's task 01).
  text("\"fw\":\"%s\",\"ver\":\"%s\",\"role\":\"%s\",\"board\":\"%s\",\"hw\":\"%s\","
       "\"radio\":\"%s\"",
       f.fw, f.version, f.role, f.board, f.hw, f.radio);
}

void Lines::homeBody(double f_mhz, double bw_khz, uint8_t sf, uint8_t cr) {
  raw("\"home\":{");
  field("f", f_mhz, 3);
  raw(",");
  field("bw", bw_khz, 1);
  text(",\"sf\":%u,\"cr\":%u}", unsigned(sf), unsigned(cr));
}

void Lines::doubles(const double* values, uint16_t count, int decimals) {
  raw("[");
  for (uint16_t i = 0; i < count; i++) {
    if (i != 0) raw(",");
    number(values == nullptr ? NAN : values[i], decimals);
  }
  raw("]");
}

void Lines::base64(const uint8_t* bytes, uint16_t count) {
  // Three bytes in, four characters out, and the fragment is flushed before it can overflow -
  // so a 2048-point frame costs this function 224 bytes of stack and not 2.7 kB.
  char buf[kFragmentBytes];
  size_t at = 0;
  for (uint16_t i = 0; i < count; i += 3) {
    if (at + 4 >= sizeof(buf)) {
      buf[at] = '\0';
      raw(buf);
      at = 0;
    }
    const uint8_t b0 = bytes == nullptr ? 0u : bytes[i];
    const bool have1 = uint32_t(i) + 1u < count;
    const bool have2 = uint32_t(i) + 2u < count;
    const uint8_t b1 = (have1 && bytes != nullptr) ? bytes[i + 1] : 0u;
    const uint8_t b2 = (have2 && bytes != nullptr) ? bytes[i + 2] : 0u;
    buf[at++] = kBase64Alphabet[b0 >> 2];
    buf[at++] = kBase64Alphabet[((b0 & 0x03u) << 4) | (b1 >> 4)];
    buf[at++] = have1 ? kBase64Alphabet[((b1 & 0x0Fu) << 2) | (b2 >> 6)] : '=';
    buf[at++] = have2 ? kBase64Alphabet[b2 & 0x3Fu] : '=';
  }
  buf[at] = '\0';
  if (at != 0) raw(buf);
}

void Lines::boot(const BootFields& f) {
  raw("{\"t\":\"boot\",");
  bootBody(f);
  text(",\"reset\":\"%s\",\"wdt\":\"%s\",\"radio_ok\":%s,\"rtc\":%d,\"ina\":%d,\"ts\":%lu}",
       f.reset, f.wdt == nullptr ? "" : f.wdt, asBool(f.radio_ok), asFlag(f.rtc), asFlag(f.ina),
       (unsigned long)f.ts);
  end();
}

void Lines::info(const InfoFields& f) {
  raw("{\"t\":\"info\",");
  bootBody(f.boot);
  text(",\"build\":\"%s\",\"devid\":\"%08lx%08lx\",\"reset\":\"%s\",\"wdt\":\"%s\","
       "\"rtc\":%d,\"ina\":%d,\"radio_ok\":%d,\"boost\":%d,\"auto_min\":%lu,",
       f.build, (unsigned long)f.devid_high, (unsigned long)f.devid_low, f.boot.reset,
       f.boot.wdt == nullptr ? "" : f.boot.wdt, asFlag(f.boot.rtc), asFlag(f.boot.ina),
       asFlag(f.boot.radio_ok), asFlag(f.boosted_gain), (unsigned long)f.auto_minutes);
  // The radio's ROM string, which is what says whether `scope` can run on this chip. Empty when
  // the radio did not answer at boot, never absent: a reader that looks for it finds a field.
  text("\"rom\":\"%s\",", f.rom == nullptr ? "" : f.rom);
  homeBody(f.home_f_mhz, f.home_bw_khz, f.home_sf, f.home_cr);
  raw("}");
  end();
}

void Lines::stat(const StatFields& f) {
  // `stat` carries no `seq`: it is a heartbeat and not a measurement, and the `seq` a reader
  // watches for gaps is the one the scan lines carry (docs/noisescope.md).
  text("{\"t\":\"stat\",\"ts\":%lu,\"ms\":%lu,\"up\":%lu,\"mode\":\"%s\",",
       (unsigned long)f.meta.ts, (unsigned long)f.meta.ms, (unsigned long)f.uptime_s, f.mode);
  field("home", f.home_f_mhz, 3);
  if (f.have_rssi) {
    text(",\"rssi\":{\"n\":%lu,", (unsigned long)f.rssi.n);
    field("avg", f.rssi.avg, 1);
    raw(",");
    field("max", f.rssi.max, 1);
    raw(",");
    field("min", f.rssi.min, 1);
    raw(",");
    field("dc", f.rssi.dc, 4);
    raw(",");
    field("th", f.rssi.th, 1);
    raw("}");
  }
  if (f.pwr.present) {
    // Millivolts and milliamps, with this board's three shunt values already applied
    // (src/common/power/Ina3221.h): `ipanel` positive = panel into the charger, `ibat` positive
    // = charging and negative = discharging, `iload` positive = draw.
    raw(",\"pwr\":{");
    field("vpanel", double(f.pwr.vpanel_mv) / 1000.0, 2);
    text(",\"ipanel\":%d,", int(f.pwr.ipanel_ma));
    field("vbat", double(f.pwr.vbat_mv) / 1000.0, 3);
    text(",\"ibat\":%d,", int(f.pwr.ibat_ma));
    field("vload", double(f.pwr.vload_mv) / 1000.0, 3);
    text(",\"iload\":%d}", int(f.pwr.iload_ma));
  }
  raw("}");
  end();
}

void Lines::sweep(const SweepFields& f) {
  raw("{\"t\":\"sweep\",");
  meta(f.meta);
  raw(",");
  field("f0", f.plan.f0, 3);
  raw(",");
  field("f1", f.plan.f1, 3);
  raw(",");
  field("step", f.plan.step_khz, 1);
  raw(",");
  field("rbw", f.plan.rbw_khz, 1);
  text(",\"dwell\":%u,\"n\":%u,\"unit\":\"dBm\",\"avg\":", unsigned(f.dwell_ms),
       unsigned(f.plan.points));
  doubles(f.avg, f.plan.points, 1);
  raw(",\"max\":");
  doubles(f.max, f.plan.points, 1);
  raw("}");
  end();
}

void Lines::scope(const ScopeFields& f) {
  // `f0` is the first point's **centre** and `step` its spacing, so point i is at
  // f0 + i*step/1000 MHz - the same arithmetic a `sweep` reader already does. Six decimals on a
  // frequency and four on a step, because a 2048-point scope of one band steps by a few hertz
  // more than three kilohertz and a reader that rounded that would bend the axis.
  raw("{\"t\":\"scope\",");
  meta(f.meta);
  raw(",");
  field("f0", f.plan.first_mhz, 6);
  raw(",");
  field("step", f.plan.step_khz, 4);
  raw(",");
  field("rbw", f.plan.rbw_khz, 1);
  text(",\"n\":%u,", unsigned(f.plan.points));
  // `cov` is the receiver bandwidth over the step: below 1 and a narrow carrier can fall between
  // two points, which is a thing a reader must be able to say on the picture (section 5).
  field("cov", f.plan.cov, 3);
  text(",\"agg\":\"%s\",\"sweeps\":%u,\"sps\":%u,\"lv\":\"", scopeAggName(f.plan.agg),
       unsigned(f.sweeps), unsigned(f.sps_x10));
  base64(f.levels, f.plan.points);
  raw("\"}");
  end();
}

void Lines::hist(const HistFields& f) {
  raw("{\"t\":\"hist\",");
  meta(f.meta);
  raw(",");
  field("f", f.f_mhz, 3);
  raw(",");
  field("rbw", f.rbw_khz, 1);
  // b0 and bw_db are the spectral-scan engine's own and not a setting: bin 0 is -11 dBm and
  // every bin is 4 dB wide, so bin i starts at b0 - 4*i (the SX126x datasheet; kHistBins bins).
  text(",\"n\":%u,\"b0\":-11,\"bw_db\":4,\"bins\":[", unsigned(f.samples));
  for (int i = 0; i < kHistBins; i++) {
    text(i != 0 ? ",%u" : "%u", f.bins == nullptr ? 0u : unsigned(f.bins[i]));
  }
  raw("]}");
  end();
}

void Lines::dwell(const DwellFields& f) {
  raw("{\"t\":\"dwell\",");
  meta(f.meta);
  raw(",");
  field("f", f.f_mhz, 3);
  text(",\"win\":%lu,\"n\":%lu,", (unsigned long)f.win_ms, (unsigned long)f.stats.n);
  field("avg", f.stats.avg, 1);
  raw(",");
  field("max", f.stats.max, 1);
  raw(",");
  field("min", f.stats.min, 1);
  raw(",");
  field("dc", f.stats.dc, 4);
  raw(",");
  field("th", f.stats.th, 1);
  raw("}");
  end();
}

void Lines::cad(const CadFields& f) {
  raw("{\"t\":\"cad\",");
  meta(f.meta);
  raw(",");
  field("f", f.f_mhz, 3);
  text(",\"sf\":%u,", unsigned(f.sf));
  field("bw", f.bw_khz, 1);
  text(",\"n\":%u,\"det\":%u}", unsigned(f.n), unsigned(f.detections));
  end();
}

void Lines::done(const char* command, const Meta& m) {
  text("{\"t\":\"done\",\"cmd\":\"%s\",", command);
  meta(m);
  raw("}");
  end();
}

void Lines::ack(const char* command, bool ok, const char* message) {
  text("{\"t\":\"ack\",\"cmd\":\"%s\",\"ok\":%s", command, asBool(ok));
  if (message != nullptr && message[0] != '\0') text(",\"msg\":\"%s\"", message);
  raw("}");
  end();
}

void Lines::err(const char* message) {
  text("{\"t\":\"err\",\"msg\":\"%s\"}", message == nullptr ? "" : message);
  end();
}

void Lines::err(const char* message, const char* call, int code) {
  if (call == nullptr || call[0] == '\0') {
    err(message);
    return;
  }
  text("{\"t\":\"err\",\"msg\":\"%s\",\"call\":\"%s\",\"code\":%d}",
       message == nullptr ? "" : message, call, code);
  end();
}

void Lines::help(const char* version) {
  text("# NoiseScope %s - commands (NDJSON out, '#' lines are chatter)\n", version);
  raw("#   sweep [f0 f1 [step_khz [dwell_ms [passes]]]]   RSSI sweep; passes 0 = until stop\n"
      "#   scope <f0> <f1> [points [rbw_khz [int_ms [agg]]]]  in-radio sweep, streamed to this\n"
      "#                                                  port; agg max|mean|last; scope off\n"
      "#   hist  [f0 f1 [step_khz [samples [passes]]]]    spectral-scan histograms (33 x 4dB bins)\n"
      "#   dwell <f_mhz> [secs [win_ms]]                  time-domain stats on one frequency\n"
      "#   cad   [f_mhz [count]]                          LoRa preamble detection ratio\n"
      "#   stop | stat | info | help\n"
      "#   home <f_mhz> [bw_khz sf cr]                    set monitor channel\n"
      "#   boost on|off                                   RX boosted gain\n"
      "#   auto <minutes>                                 periodic background sweep (0 = off)\n"
      "#   time <epoch>                                   set clock (writes RTC when present)\n"
      "#   reboot | dfu [serial]                          restart / enter bootloader\n");
}

}  // namespace scanner
}  // namespace twinoak
