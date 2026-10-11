// Our settings, as upstream's CLI sees them: `sensor list`, `sensor get <key>` and
// `sensor set <key> <value>` (docs/ARCHITECTURE.md section 4's table, plus the two battery
// thresholds of phase 1's task 06).
//
// Why they are here and not only on the box link: these are the two ways into a unit that do not
// need the Walter. The USB console at the field port is one; a **mesh admin** with a phone is the
// other, because upstream runs a logged-in admin's text message through the same interpreter
// (examples/simple_repeater/MyMesh.cpp:702 at the pin d9296435 gates it on
// `client->isAdmin()`). So `sensor set walter.wake 1` from a phone that can reach this repeater
// asks it to wake its own uplink - which is how a unit whose Walter is asleep is reached at all
// (spec/LINK-PROTOCOL.md section 8, reason 4).
//
// Host-portable: no Arduino, no MeshCore. Every value comes from the host interface below, which
// is what lets test/test_settings hold the table - its order, its keys, what a `set` of each one
// does - against fakes; src/board/target.cpp is the twenty lines that make it upstream's
// SensorManager seam.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace twinoak {

// What the settings read and write. One method per thing, so that a fake is a fake and not a
// second copy of the firmware.
struct SettingsHost {
  virtual ~SettingsHost() {}

  // "twinoak-repeater <version> on MeshCore <tag> (<pin8>)" (src/app/Version.h).
  virtual const char* fwVersion() const = 0;
  // Frames the box link's receiver dropped since boot, saturating.
  virtual uint16_t linkErrors() const = 0;
  // Whether a valid request has ever arrived on the box link, and how long ago.
  virtual bool walterEverSeen() const = 0;
  virtual uint32_t walterSeenSecondsAgo() const = 0;
  // "unset", "restored", "set" or "rtc" (spec/LINK-PROTOCOL.md 5.4's time_state, by name).
  virtual const char* clockStateName() const = 0;
  // Whether an attention is pending, which is what `walter.wake` reads back.
  virtual bool attentionPending() const = 0;
  // `sensor set walter.wake 1`: ask for the Walter's attention, reason 4. False when the machine
  // refused it - the hourly limit, or one of that reason already pending - which is not an error
  // and is not an answer the CLI has a word for (see set() below).
  virtual bool raiseWalterWake() = 0;

  // The battery thresholds of section 8's reasons 2 and 3, in millivolts: `power.low_mv` and
  // `power.crit_mv` (src/power/BatteryWatch.h). A `set` is refused - and nothing changed - when
  // the pair that would result is not one the firmware accepts; the board also writes the
  // preferences file here, so that the value survives a restart.
  virtual uint16_t batteryLowMv() const = 0;
  virtual uint16_t batteryCritMv() const = 0;
  virtual bool setBatteryLowMv(uint16_t mv) = 0;
  virtual bool setBatteryCritMv(uint16_t mv) = 0;
};

class Settings {
 public:
  explicit Settings(SettingsHost& host) : host_(host) {}

  // How many keys there are, and the key and the value of each. The order is fixed and is the
  // order `sensor list` prints: it is part of what a bench and a mesh admin read, and upstream's
  // own paging (`sensor list <start>`) counts in it.
  static int count();
  static const char* name(int i);
  // The value of key i, or nullptr out of range. Valid until the next call: four of the seven
  // are formatted into a buffer of this object, which is how upstream's CLI uses them (it prints
  // one and then asks for the next).
  const char* value(int i);

  // `sensor set <name> <value>`. True when this table took it - upstream's CLI then answers
  // "ok"; false when the key is not ours or the value is not one this key accepts, and upstream's
  // CLI then answers "can't find custom var" for both, which is its only other word.
  //
  // Four of the seven keys are read-only and refuse every value. `walter.wake` accepts "1" and
  // nothing else, and accepting it means "the machine was asked": whether an attention was
  // actually raised is the hourly limit's business, and `sensor get walter.wake` is where the
  // answer to that is read. `power.low_mv` and `power.crit_mv` take a decimal number of
  // millivolts and refuse anything the thresholds would not accept.
  bool set(const char* name, const char* value);

 private:
  SettingsHost& host_;
  char buf_[16] = {0};
};

}  // namespace twinoak
