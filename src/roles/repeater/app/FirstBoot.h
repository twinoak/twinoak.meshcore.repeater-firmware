// First boot: the one test that says "this node has no settings yet", and what is done about it -
// the random admin password of spec/SYSTEM.md S-13 and the profile of phase 3's task 03
// (src/roles/repeater/firstboot/Profile.h), both decided by that one test and both done from the
// same place in setup().
//
// **Once is exact.** The two things happen inside one hold of upstream's preference saves
// (TwinOakMesh::savePrefs), so `/prefs.json` - the file the test below looks for - appears exactly
// once, when everything this boot does is already in it. A power cut before that moment leaves no
// file, and the next boot does the whole thing again; a power cut after it leaves a node that is
// done. There is no moment at which the file exists and the work is half finished, and no second
// marker anywhere to disagree with it.

#pragma once

#include <stddef.h>

#include "roles/repeater/app/TwinOakMesh.h"
#include "roles/repeater/app/UpstreamMyMesh.h"
#include "roles/repeater/firstboot/Profile.h"

namespace twinoak {

// Whether saved preferences exist, read the way CommonCLI::loadPrefs (src/helpers/CommonCLI.cpp:30
// at the pin) reads them: it loads "/prefs.json" if that exists, else converts the legacy
// "/com_prefs" if that exists, else leaves the constructor's defaults - ADMIN_PASSWORD among them.
// So "no saved preferences" is: neither file exists. Call it before the_mesh.begin(), which is
// what runs loadPrefs.
bool hasSavedPrefs(FILESYSTEM& fs);

// After the_mesh.begin(): when no preferences were saved, or the build's default password is in
// the preferences anyway, draws a random password from the radio's RNG straight into
// getNodePrefs()->password and asks for the preferences to be saved, so that the default never
// reaches flash and the next boot loads the random one. Prints nothing. Returns true when it drew
// one.
bool ensureAdminPassword(MyMesh& mesh, bool saved_prefs_found);

// What the boot did, for the one line setup() prints and for nothing else - `sensor get
// firstboot.state` is where the outcome is read afterwards (firstboot::stateText()).
struct FirstBootOutcome {
  bool was_first_boot = false;  // no saved preferences were found: this boot provisioned the node
  bool password_set = false;    // a random admin password was drawn (never printed)
  bool prefs_written = false;   // the one preferences write happened
  firstboot::ProfileResult profile;
};

// The whole of a first boot, in one call, after the_mesh.begin() and before the radio is used:
// the random admin password, then the profile's lines through upstream's own
// handleCommand(0, ...), then one preferences write for both.
//
// `line` and `reply` are the console's own two buffers (src/roles/repeater/main.cpp), lent to the
// profile while setup() runs - nothing else is using them yet, and sizing them once means the
// console and the first boot cannot disagree about how much room upstream's interpreter has.
FirstBootOutcome firstBoot(TwinOakMesh& mesh, bool saved_prefs_found, char* line, size_t line_cap,
                           char* reply, size_t reply_cap);

}  // namespace twinoak
