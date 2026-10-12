#include "roles/repeater/firstboot/Profile.h"

#include <stdio.h>
#include <string.h>

namespace twinoak {
namespace firstboot {

// --- the table ---------------------------------------------------------------------------------
//
// One line, and the rule that keeps it one line: **a line is here only where a document of this
// project gives the value.** The reviewer fills the rest from the network's own standard
// (tasks/phase-3/reports/03.md lists every `set` key of the pinned CLI with upstream's default
// beside it, and meshguide.dk's recommendation where there is one); a session with nobody to ask
// does not invent a value for a radio nobody has measured or a site nobody has visited.
//
// What is deliberately *not* here:
//
//   - the **channel** (`set radio`): the build's default, so that a unit is on the right
//     frequency even if this profile never ran (platformio.ini's [band_868]);
//   - the **name**, the **position**, the **owner info**, the **regions**: per site, and a unit
//     with somebody else's name on the air is worse than a unit called `repeater`;
//   - **any password**: a password is never in an image (spec/SYSTEM.md S-13). The admin password
//     of this boot is the random one src/roles/repeater/app/AdminPassword.h draws, and the guest
//     password is upstream's empty default until a person sets it;
//   - the **delays** (`rxdelay`, `txdelay`, `direct.txdelay`) and `flood.max.unscoped`: the
//     Danish standard tunes them to how many neighbours a site hears, which a unit that has
//     never been switched on cannot know.
const ProfileLine kProfile[] = {
    // spec/SYSTEM.md S-12: "Path hashes this firmware chooses are two bytes or more
    // (meshmaster.dk D-33)". Upstream floods this node's own adverts at `path_hash_mode + 1`
    // bytes (MyMesh::sendSelfAdvertisement, examples/simple_repeater/MyMesh.cpp:1035 at the pin
    // d9296435, and the periodic flood advert at :1297), so the default of 0 is one byte - and
    // one byte makes two repeaters that share a first hash byte both match a packet's next hop.
    // D-33 names this key for repeaters in so many words: `path.hash.mode` "at least 1". The
    // mesh client's own floods already pass 2 in code (meshclient::kFloodPathHashSize); this is
    // the half of S-12 that upstream decides from a preference.
    {"set path.hash.mode 1",
     "flooded adverts carry two-byte path hashes (S-12, meshmaster.dk D-33)"},
};

const int kProfileCount = int(sizeof(kProfile) / sizeof(kProfile[0]));

// --- the reply rule ----------------------------------------------------------------------------

bool replyAccepted(const char* reply) {
  if (reply == nullptr) return false;
  const char* p = reply;
  while (*p == ' ') p++;
  if (*p == '(') {  // `clear stats` answers "(OK - stats reset)"
    p++;
    while (*p == ' ') p++;
  }
  if ((p[0] != 'O' && p[0] != 'o') || (p[1] != 'K' && p[1] != 'k')) return false;
  // And `ok` is a word, not a prefix: nothing of upstream's answers "okay", and a reply that did
  // would not be one this file has read.
  const char c = p[2];
  return c == 0 || c == ' ' || c == ',' || c == '-' || c == ':' || c == '.' || c == '!' ||
         c == ')';
}

// --- the runner --------------------------------------------------------------------------------

ProfileResult runProfile(const ProfileLine* lines, int count, link::CliAccess& cli, char* line,
                         size_t line_cap, char* reply, size_t reply_cap) {
  ProfileResult out;
  if (lines == nullptr || line == nullptr || reply == nullptr || line_cap == 0 ||
      reply_cap == 0) {
    return out;
  }
  // The window upstream's own console gives its interpreter, and never more than the caller has:
  // what is behind the window is the guard, zeroed with the rest and read by nothing here.
  const size_t window = reply_cap < size_t(link::kCliReplyMax) ? reply_cap
                                                               : size_t(link::kCliReplyMax);

  for (int i = 0; i < count; i++) {
    const char* command = lines[i].command;
    const size_t len = command == nullptr ? 0 : strlen(command);
    // A line that is empty, or longer than the buffer the console would have given it, is
    // refused without being run: a truncated `set` is a wrong value, not a missing one.
    if (len == 0 || len + 1 > line_cap) {
      out.refused++;
      if (out.first_refused == nullptr) out.first_refused = command;
      continue;
    }
    memcpy(line, command, len);
    line[len] = 0;
    memset(reply, 0, reply_cap);
    cli.runCommand(line, reply, window);
    // The bytes decide, not the length the interpreter reports, and never past the window.
    reply[window - 1] = 0;
    if (replyAccepted(reply)) {
      out.ran++;
    } else {
      out.refused++;
      if (out.first_refused == nullptr) out.first_refused = command;
    }
  }
  return out;
}

ProfileResult runFirstBootProfile(bool saved_prefs_found, const ProfileLine* lines, int count,
                                  link::CliAccess& cli, char* line, size_t line_cap, char* reply,
                                  size_t reply_cap) {
  if (saved_prefs_found) {
    // Settings existed already: upstream loaded them and they are what the node is. Nothing of
    // the profile runs, not even a line that would be harmless - a unit a person configured is
    // not corrected by its own firmware behind their back.
    noteState(State::kNever, 0);
    return ProfileResult();
  }
  const ProfileResult result =
      runProfile(lines, count, cli, line, line_cap, reply, reply_cap);
  noteState(result.refused == 0 ? State::kDone : State::kPartial, result.refused);
  return result;
}

// --- the state ---------------------------------------------------------------------------------

namespace {
// "partial: 4294967295 refused" is 27 characters and its NUL; the counter cannot reach that (the
// table is a handful of lines), and the buffer is sized so that nothing has to think about it.
char g_state[32] = "never";
}  // namespace

void noteState(State state, int refused) {
  switch (state) {
    case State::kDone:
      snprintf(g_state, sizeof(g_state), "done");
      break;
    case State::kPartial:
      snprintf(g_state, sizeof(g_state), "partial: %d refused", refused);
      break;
    case State::kNever:
    default:
      snprintf(g_state, sizeof(g_state), "never");
      break;
  }
}

const char* stateText() { return g_state; }

}  // namespace firstboot
}  // namespace twinoak
