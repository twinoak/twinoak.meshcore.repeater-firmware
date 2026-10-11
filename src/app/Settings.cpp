#include "Settings.h"

#include <stdio.h>
#include <string.h>

namespace twinoak {
namespace {

// The order `sensor list` prints, and the order upstream's paging counts in. **It is phase 1's
// task 05's order of five keys with the later tasks' appended** - task 06's two thresholds, then
// phase 2's task 03's `obs.wake` - which is the only place a new key can go without renumbering
// one a bench instruction or a document already names (docs/link.md, docs/bench.md,
// tasks/phase-1/reports/05.md).
//
// That order was chosen for one quirk of upstream's: `sensor list` stops adding lines once it has
// written **134 bytes** into a 160-byte reply (src/helpers/CommonCLI.cpp at the pin d9296435),
// and the `fw.version` line alone is 78 of them. With five keys everything fitted on one page.
// With seven it does not, and what the order buys is that **the key pushed off is pushed by a
// width and the pair of thresholds stays together**:
//
//   - a fresh node lists all five of task 05's and `... next:5`, 159 bytes of the 160;
//   - a node whose counters have grown wide lists four and `... next:4`;
//   - `sensor list 5` is then how the two thresholds and `obs.wake` are read, and
//     `sensor get <key>` always works whatever the paging does.
//
// A listing that would be longer than the window is **cut, not spilled**: the session keeps 64
// guard bytes behind upstream's 160 and counts the cut (src/link/session/Session.h,
// cliTruncations). The one case that reaches it is a release whose version string is longer than
// `0.0.0-dev` *and* a clock state of `restored` - 162 bytes, cut at 159, which costs the tail of
// `... next:5` and nothing else. tasks/phase-1/reports/06.md has that arithmetic; the test below
// walks upstream's own loop over this table so that it is measured and not assumed.

enum Key : int {
  kFwVersion = 0,
  kWalterWake,
  kLinkErrors,
  kWalterSeen,
  kClockState,
  kPowerLowMv,
  kPowerCritMv,
  kObsWake,
  kKeyCount,
};

const char* const kNames[kKeyCount] = {
    "fw.version",  "walter.wake",  "link.errors",   "walter.seen",
    "clock.state", "power.low_mv", "power.crit_mv", "obs.wake",
};

// A decimal millivolt value, with nothing else in it: "3500" yes, "3500 " no, "3.5" no, "+3500"
// no, "" no. Upstream's CLI has two words for a `set` - "ok" and "can't find custom var" - so a
// value it cannot use must be refused and not rounded into something it can.
bool parseMilliVolts(const char* text, uint16_t& out) {
  if (text == nullptr || *text == 0) return false;
  uint32_t value = 0;
  for (const char* p = text; *p != 0; p++) {
    if (*p < '0' || *p > '9') return false;
    value = value * 10u + uint32_t(*p - '0');
    if (value > 0xFFFFu) return false;
  }
  out = uint16_t(value);
  return true;
}

}  // namespace

int Settings::count() { return kKeyCount; }

const char* Settings::name(int i) {
  return (i >= 0 && i < kKeyCount) ? kNames[i] : nullptr;
}

const char* Settings::value(int i) {
  switch (i) {
    case kFwVersion:
      return host_.fwVersion();
    case kWalterWake:
      // "0, or 1 while an attention is pending" (ARCHITECTURE section 4). Any reason, not only
      // the one this key raises: what it answers is "does this node still want the box awake".
      return host_.attentionPending() ? "1" : "0";
    case kClockState:
      return host_.clockStateName();
    case kLinkErrors:
      snprintf(buf_, sizeof(buf_), "%u", unsigned(host_.linkErrors()));
      return buf_;
    case kWalterSeen:
      if (!host_.walterEverSeen()) return "never";
      snprintf(buf_, sizeof(buf_), "%lu", (unsigned long)host_.walterSeenSecondsAgo());
      return buf_;
    case kPowerLowMv:
      snprintf(buf_, sizeof(buf_), "%u", unsigned(host_.batteryLowMv()));
      return buf_;
    case kPowerCritMv:
      snprintf(buf_, sizeof(buf_), "%u", unsigned(host_.batteryCritMv()));
      return buf_;
    case kObsWake:
      // "0" or "1": whether a filling observation ring may ask for a sleeping Walter
      // (LINK-PROTOCOL.md section 8, reason 5). Off at boot.
      return host_.obsWake() ? "1" : "0";
    default:
      return nullptr;
  }
}

bool Settings::set(const char* name, const char* value) {
  if (name == nullptr || value == nullptr) return false;
  if (strcmp(name, kNames[kWalterWake]) == 0) {
    if (strcmp(value, "1") != 0) return false;  // 1 is the only value this takes
    host_.raiseWalterWake();
    return true;
  }
  if (strcmp(name, kNames[kPowerLowMv]) == 0) {
    uint16_t mv = 0;
    if (!parseMilliVolts(value, mv)) return false;
    return host_.setBatteryLowMv(mv);  // the host refuses a pair it would not accept
  }
  if (strcmp(name, kNames[kPowerCritMv]) == 0) {
    uint16_t mv = 0;
    if (!parseMilliVolts(value, mv)) return false;
    return host_.setBatteryCritMv(mv);
  }
  if (strcmp(name, kNames[kObsWake]) == 0) {
    // Both ways, unlike `walter.wake`: this one is a setting and not a request, so it can be
    // turned off again.
    const bool on = strcmp(value, "1") == 0;
    if (!on && strcmp(value, "0") != 0) return false;
    host_.setObsWake(on);
    return true;
  }
  return false;  // the other four are read-only
}

}  // namespace twinoak
