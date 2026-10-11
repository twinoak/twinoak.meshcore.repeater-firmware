// First boot: the one test that says "this node has no settings yet", and what is done about the
// admin password when it is true (spec/SYSTEM.md S-13). Phase 3's first-boot profile runs on the
// same test, from the same place in setup().

#pragma once

#include "UpstreamMyMesh.h"

namespace twinoak {

// Whether saved preferences exist, read the way CommonCLI::loadPrefs (src/helpers/CommonCLI.cpp:30
// at the pin) reads them: it loads "/prefs.json" if that exists, else converts the legacy
// "/com_prefs" if that exists, else leaves the constructor's defaults - ADMIN_PASSWORD among them.
// So "no saved preferences" is: neither file exists. Call it before the_mesh.begin(), which is
// what runs loadPrefs.
bool hasSavedPrefs(FILESYSTEM& fs);

// After the_mesh.begin(): when no preferences were saved, or the build's default password is in
// the preferences anyway, draws a random password from the radio's RNG straight into
// getNodePrefs()->password and saves the preferences (savePrefs()), so the default never reaches
// flash and the next boot loads the random one. Prints nothing. Returns true when it drew one.
bool ensureAdminPassword(MyMesh& mesh, bool saved_prefs_found);

}  // namespace twinoak
