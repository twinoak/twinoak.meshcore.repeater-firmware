#include "roles/repeater/app/FirstBoot.h"

#include "roles/repeater/app/AdminPassword.h"

// The build must say what upstream's constructor puts in the password field before this code
// replaces it (MyMesh.cpp:897 at the pin copies ADMIN_PASSWORD into _prefs.password). Without the
// define upstream's MyMesh.cpp:32 would default it to "password" - the thing S-13 forbids - so a
// board environment that forgets it does not build; test/test_rails holds the same line against
// platformio.ini.
#ifndef ADMIN_PASSWORD
#error "ADMIN_PASSWORD must be defined by the board environment in platformio.ini (spec/SYSTEM.md S-13)"
#endif

namespace twinoak {

namespace {
class RadioRandom : public RandomSource {
 public:
  void fill(uint8_t* dest, size_t len) override { radio_random(dest, len); }
};
}  // namespace

bool hasSavedPrefs(FILESYSTEM& fs) {
  return fs.exists("/prefs.json") || fs.exists("/com_prefs");
}

bool ensureAdminPassword(MyMesh& mesh, bool saved_prefs_found) {
  NodePrefs* prefs = mesh.getNodePrefs();
  if (!needsFreshPassword(saved_prefs_found, prefs->password, ADMIN_PASSWORD)) return false;
  RadioRandom rng;
  // Written straight into the preferences - NodePrefs::password is char[16], so fifteen
  // characters and the NUL - with no copy anywhere else to wipe.
  generatePassword(prefs->password, sizeof(prefs->password), rng);
  mesh.savePrefs();
  return true;
}

}  // namespace twinoak
