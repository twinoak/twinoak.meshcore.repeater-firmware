// The repeater's half of src/common/board/RoleHooks.h: the six functions src/common/ calls into
// whichever role an environment builds.
//
// Nothing here is new. The settings host below stood in src/common/board/target.cpp until the
// tree was split by role (phase 3's task 01) - it is the board's half of the ten keys of
// docs/ARCHITECTURE.md section 4, whose table and rules are portable and tested
// (src/roles/repeater/app/Settings.{h,cpp}). The keys are on upstream's CLI and not only on the
// box link so that they can be read from the USB console and, over the mesh, by an admin: that
// is how a unit whose Walter is silent is diagnosed without opening the box, and `walter.wake`
// is how such a unit's uplink is woken from a phone.

#include "common/board/RoleHooks.h"

#include "common/power/BoardPower.h"
#include "common/version/Version.h"
#include "roles/repeater/app/Settings.h"
#include "roles/repeater/clock/BoardClock.h"
#include "roles/repeater/firstboot/Profile.h"
#include "roles/repeater/link/BoardLink.h"

namespace {

class BoardSettingsHost : public twinoak::SettingsHost {
 public:
  const char* fwVersion() const override { return twinoak::versionString(); }
  uint16_t linkErrors() const override { return twinoak::link::linkErrors(); }
  bool walterEverSeen() const override { return twinoak::link::walterEverSeen(); }
  uint32_t walterSeenSecondsAgo() const override {
    return twinoak::link::walterSeenSecondsAgo();
  }
  const char* clockStateName() const override { return twinoak::clock::stateName(); }
  bool attentionPending() const override { return twinoak::link::attentionPending(); }
  bool raiseWalterWake() override { return twinoak::link::raiseWalterWake(); }
  uint16_t batteryLowMv() const override { return twinoak::power::batteryLowMilliVolts(); }
  uint16_t batteryCritMv() const override { return twinoak::power::batteryCritMilliVolts(); }
  bool setBatteryLowMv(uint16_t mv) override {
    return twinoak::power::setBatteryLowMilliVolts(mv);
  }
  bool setBatteryCritMv(uint16_t mv) override {
    return twinoak::power::setBatteryCritMilliVolts(mv);
  }
  bool obsWake() const override { return twinoak::link::obsWakeEnabled(); }
  void setObsWake(bool on) override { twinoak::link::setObsWakeEnabled(on); }
  uint32_t clientSendsRefused() const override { return twinoak::link::clientRefused(); }
  const char* firstBootState() const override { return twinoak::firstboot::stateText(); }
};

BoardSettingsHost g_settings_host;
twinoak::Settings g_settings(g_settings_host);

}  // namespace

namespace twinoak {
namespace role {

// The clock's lower bound, so that the next boot carries on from this second rather than from an
// hour ahead of it (src/roles/repeater/clock/ClockKeeper.h, spec/LINK-PROTOCOL.md 5.4).
void beforeRestart() { twinoak::clock::saveBeforeRestart(); }

bool raiseAttention(uint8_t reason, uint16_t detail) {
  return twinoak::link::raiseAttention(reason, detail);
}

int settingsCount() { return twinoak::Settings::count(); }

const char* settingName(int i) { return twinoak::Settings::name(i); }

const char* settingValue(int i) { return g_settings.value(i); }

// `walter.wake 1`, `power.low_mv <mv>`, `power.crit_mv <mv>` and `obs.wake 0|1`; the other six
// are read-only.
bool settingSet(const char* name, const char* value) { return g_settings.set(name, value); }

}  // namespace role
}  // namespace twinoak
