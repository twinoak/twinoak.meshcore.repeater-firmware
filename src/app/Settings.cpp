#include "Settings.h"

#include <stdio.h>
#include <string.h>

namespace twinoak {
namespace {

// The order `sensor list` prints, and the order upstream's paging counts in. It is chosen and
// not alphabetical, because upstream's `sensor list` stops adding lines once it has written
// **134 bytes** into a 160-byte reply (src/helpers/CommonCLI.cpp at the pin d9296435) and the
// `fw.version` line alone is 78 of them:
//
//   - `fw.version` first, where phase 1's task 01 put it and where a bench reads it;
//   - `walter.wake` next, because it is the one key here that *does* something, and task 03's
//     two keys keep the order they had;
//   - `clock.state` last, because it is the only one of the five whose value is one of four
//     words: the two counters above it are what can grow to ten characters, and the key a
//     long-running node pushes behind a page should be pushed by a width and not by a name.
//
// So a fresh node lists all five on one page (148 bytes of the 160) and a node whose counters
// have grown wide lists four and `... next:4`. test/test_settings runs that arithmetic over this
// table, so a reordering which pushes a key off the first page of a fresh node fails there and
// not on the bench.
enum Key : int {
  kFwVersion = 0,
  kWalterWake,
  kLinkErrors,
  kWalterSeen,
  kClockState,
  kKeyCount,
};

const char* const kNames[kKeyCount] = {
    "fw.version", "walter.wake", "link.errors", "walter.seen", "clock.state",
};

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
    case kLinkErrors:
      snprintf(buf_, sizeof(buf_), "%u", unsigned(host_.linkErrors()));
      return buf_;
    case kWalterSeen:
      if (!host_.walterEverSeen()) return "never";
      snprintf(buf_, sizeof(buf_), "%lu", (unsigned long)host_.walterSeenSecondsAgo());
      return buf_;
    case kClockState:
      return host_.clockStateName();
    default:
      return nullptr;
  }
}

bool Settings::set(const char* name, const char* value) {
  if (name == nullptr || value == nullptr) return false;
  if (strcmp(name, kNames[kWalterWake]) != 0) return false;  // the other four are read-only
  if (strcmp(value, "1") != 0) return false;                 // and 1 is the only value this takes
  host_.raiseWalterWake();
  return true;
}

}  // namespace twinoak
