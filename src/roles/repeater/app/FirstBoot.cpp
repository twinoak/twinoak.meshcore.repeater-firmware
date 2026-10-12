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

// The profile's command interface: upstream's interpreter with console privileges, which is the
// same call the USB console and the box link's `CLI` request make
// (src/roles/repeater/link/BoardLink.cpp has the link's).
class MeshCli : public link::CliAccess {
 public:
  explicit MeshCli(MyMesh& mesh) : mesh_(mesh) {}

  size_t runCommand(char* line, char* reply, size_t reply_cap) override {
    if (reply_cap == 0) return 0;
    reply[0] = 0;
    mesh_.handleCommand(0, line, reply);  // 0: "there is no sender_timestamp via serial"
    size_t n = 0;
    while (n < reply_cap && reply[n] != 0) n++;
    return n;
  }

 private:
  MyMesh& mesh_;
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

FirstBootOutcome firstBoot(TwinOakMesh& mesh, bool saved_prefs_found, char* line, size_t line_cap,
                           char* reply, size_t reply_cap) {
  FirstBootOutcome out;
  out.was_first_boot = !saved_prefs_found;

  // One write for everything below, or none at all (FirstBoot.h).
  mesh.holdPrefsSaves();

  out.password_set = ensureAdminPassword(mesh, saved_prefs_found);

  // Runs once, and the once-ness is this one boolean: with settings present no line runs and
  // `firstboot.state` says `never`. (The password may still have been redrawn above, which is
  // the belt-and-braces rail of S-13 and not a provisioning.)
  MeshCli cli(mesh);
  out.profile = firstboot::runFirstBootProfile(saved_prefs_found, firstboot::kProfile,
                                               firstboot::kProfileCount, cli, line, line_cap,
                                               reply, reply_cap);

  out.prefs_written = mesh.releasePrefsSaves();
  return out;
}

}  // namespace twinoak
