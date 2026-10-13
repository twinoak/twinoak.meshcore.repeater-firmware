#include "roles/scanner/Commands.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace twinoak {
namespace scanner {
namespace {

bool same(const char* a, const char* b) { return strcmp(a, b) == 0; }

// A token that is not there reads as absent and never as an empty string: every accessor below
// goes through this.
const char* arg(const Tokens& tokens, int index) {
  return index < tokens.count ? tokens.at[index] : nullptr;
}

double asDouble(const char* text, double fallback) { return text == nullptr ? fallback : atof(text); }

uint32_t asU32(const char* text, uint32_t fallback) {
  return text == nullptr ? fallback : uint32_t(strtoul(text, nullptr, 10));
}

int32_t asI32(const char* text, int32_t fallback) {
  return text == nullptr ? fallback : int32_t(strtol(text, nullptr, 10));
}

}  // namespace

Tokens tokenize(char* line) {
  Tokens out;
  if (line == nullptr) return out;
  char* at = line;
  while (*at != '\0' && out.count < kMaxTokens) {
    while (*at == ' ' || *at == '\t') at++;
    if (*at == '\0') break;
    char* word = at;
    while (*at != '\0' && *at != ' ' && *at != '\t') at++;
    if (*at != '\0') *at++ = '\0';
    // The command word is case-insensitive; an argument is not ("on", "serial").
    if (out.count == 0) {
      for (char* p = word; *p != '\0'; p++) {
        *p = char(tolower(static_cast<unsigned char>(*p)));
      }
    }
    out.at[out.count++] = word;
  }
  return out;
}

Verb verbOf(const char* word) {
  if (word == nullptr) return kVerbUnknown;
  if (same(word, "help")) return kVerbHelp;
  if (same(word, "info")) return kVerbInfo;
  if (same(word, "stat")) return kVerbStat;
  if (same(word, "stop")) return kVerbStop;
  if (same(word, "sweep")) return kVerbSweep;
  if (same(word, "scope")) return kVerbScope;
  if (same(word, "hist")) return kVerbHist;
  if (same(word, "dwell")) return kVerbDwell;
  if (same(word, "cad")) return kVerbCad;
  if (same(word, "home")) return kVerbHome;
  if (same(word, "boost")) return kVerbBoost;
  if (same(word, "auto")) return kVerbAuto;
  if (same(word, "time")) return kVerbTime;
  if (same(word, "reboot")) return kVerbReboot;
  if (same(word, "dfu")) return kVerbDfu;
  return kVerbUnknown;
}

const char* nameOf(Verb verb) {
  switch (verb) {
    case kVerbHelp: return "help";
    case kVerbInfo: return "info";
    case kVerbStat: return "stat";
    case kVerbStop: return "stop";
    case kVerbSweep: return "sweep";
    case kVerbScope: return "scope";
    case kVerbHist: return "hist";
    case kVerbDwell: return "dwell";
    case kVerbCad: return "cad";
    case kVerbHome: return "home";
    case kVerbBoost: return "boost";
    case kVerbAuto: return "auto";
    case kVerbTime: return "time";
    case kVerbReboot: return "reboot";
    case kVerbDfu: return "dfu";
    default: return "";
  }
}

const char* usageOf(Verb verb) {
  switch (verb) {
    case kVerbScope: return "usage: scope <f0> <f1> [points [rbw_khz [interval_ms [max|mean|last]]]] | scope off";
    case kVerbDwell: return "usage: dwell <f_mhz> [secs [win_ms]]";
    case kVerbHome: return "usage: home <f_mhz> [bw_khz sf cr]";
    case kVerbBoost: return "usage: boost on|off";
    case kVerbAuto: return "usage: auto <minutes>";
    case kVerbTime: return "usage: time <unix_epoch>";
    default: return "";
  }
}

ScanArgs parseScan(const Tokens& tokens, bool is_hist, const ScanDefaults& defaults) {
  ScanArgs out;
  out.f0 = defaults.f0;
  out.f1 = defaults.f1;
  out.step_khz = defaults.step_khz;
  out.fourth = is_hist ? defaults.hist_samples : defaults.sweep_dwell_ms;
  out.passes = 1;
  // Both ends together: one number alone says nothing about a range, so it is ignored, which is
  // how NoiseScope read it.
  if (tokens.count >= 3) {
    out.f0 = asDouble(arg(tokens, 1), out.f0);
    out.f1 = asDouble(arg(tokens, 2), out.f1);
  }
  if (tokens.count >= 4) out.step_khz = asDouble(arg(tokens, 3), out.step_khz);
  if (tokens.count >= 5) out.fourth = asU32(arg(tokens, 4), out.fourth);
  if (tokens.count >= 6) out.passes = asI32(arg(tokens, 5), out.passes);
  return out;
}

bool parseScope(const Tokens& tokens, ScopeArgs& out) {
  const char* first = arg(tokens, 1);
  if (first == nullptr) return false;
  ScopeArgs parsed;
  // `scope off` ends the mode and takes nothing else: the one word that is not a number here.
  if (same(first, "off")) {
    parsed.off = true;
    out = parsed;
    return true;
  }
  // Both ends, together and required - a span is two numbers or it is not a span.
  if (tokens.count < 3) return false;
  parsed.f0 = asDouble(first, 0.0);
  parsed.f1 = asDouble(arg(tokens, 2), 0.0);
  if (tokens.count >= 4) parsed.points = asU32(arg(tokens, 3), 0);
  if (tokens.count >= 5) parsed.rbw_khz = asDouble(arg(tokens, 4), 0.0);
  if (tokens.count >= 6) parsed.interval_ms = asU32(arg(tokens, 5), 0);
  if (tokens.count >= 7 && !scopeAggOf(arg(tokens, 6), parsed.agg)) return false;
  out = parsed;
  return true;
}

bool parseDwell(const Tokens& tokens, DwellArgs& out) {
  if (tokens.count < 2) return false;
  DwellArgs parsed;
  parsed.f_mhz = asDouble(arg(tokens, 1), 0.0);
  parsed.secs = asU32(arg(tokens, 2), parsed.secs);
  parsed.win_ms = asU32(arg(tokens, 3), parsed.win_ms);
  out = parsed;
  return true;
}

CadArgs parseCad(const Tokens& tokens, double home_f_mhz) {
  CadArgs out;
  out.f_mhz = asDouble(arg(tokens, 1), home_f_mhz);
  out.count = asU32(arg(tokens, 2), out.count);
  return out;
}

bool parseHome(const Tokens& tokens, const HomeArgs& current, HomeArgs& out) {
  if (tokens.count < 2) return false;
  HomeArgs parsed = current;
  parsed.f_mhz = asDouble(arg(tokens, 1), current.f_mhz);
  // The three modem settings are read only when all three are there, as NoiseScope read them.
  if (tokens.count >= 5) {
    parsed.bw_khz = asDouble(arg(tokens, 2), current.bw_khz);
    parsed.sf = uint8_t(asU32(arg(tokens, 3), current.sf));
    parsed.cr = uint8_t(asU32(arg(tokens, 4), current.cr));
  }
  out = parsed;
  return true;
}

bool parseBoost(const Tokens& tokens, bool& on) {
  if (tokens.count < 2) return false;
  const char* word = arg(tokens, 1);
  on = same(word, "on") || same(word, "1");
  return true;
}

bool parseAuto(const Tokens& tokens, uint32_t& minutes) {
  if (tokens.count < 2) return false;
  minutes = asU32(arg(tokens, 1), 0);
  return true;
}

bool parseEpoch(const Tokens& tokens, uint32_t& epoch) {
  if (tokens.count < 2) return false;
  const uint32_t parsed = asU32(arg(tokens, 1), 0);
  if (parsed <= kEpochFloor) return false;
  epoch = parsed;
  return true;
}

bool parseDfuSerialOnly(const Tokens& tokens) {
  const char* word = arg(tokens, 1);
  return word != nullptr && same(word, "serial");
}

}  // namespace scanner
}  // namespace twinoak
