#include "roles/repeater/firstboot/Profile.h"

#include <stdio.h>
#include <string.h>

#include "common/bands/Bands.h"

// The band's own duty cycle, from platformio.ini's [band_*] section by way of the environment
// (and, for tools/linkloop/, read out of the same two sections by tools/linkloop/check.sh). It is
// a number here and not a string, because the line below is built from it: a build that cannot
// say what the band allows does not get to ship a profile that guesses.
#ifndef TWINOAK_BAND_DUTY_CYCLE
#error "TWINOAK_BAND_DUTY_CYCLE is the band's duty cycle, from platformio.ini's [band_*] section"
#endif
#ifndef TWINOAK_BAND
#error "TWINOAK_BAND is set by platformio.ini from the environment's [img_*] section"
#endif

// `set dutycycle 10`, with the 10 coming from the band and never from this file.
#define TWINOAK_PROFILE_TEXT_(x) #x
#define TWINOAK_PROFILE_TEXT(x) TWINOAK_PROFILE_TEXT_(x)

namespace twinoak {
namespace firstboot {

// The two halves of "built from the band" meet here: the number the line is made of is the
// number this band's row in src/common/bands/Bands.h carries. A [band_*] section and a row that
// disagreed would be a unit asking for a duty cycle no document of ours gives.
static_assert(TWINOAK_BAND_DUTY_CYCLE == bands::dutyCycleOf(bands::kBands, bands::kBandCount,
                                                            TWINOAK_BAND),
              "platformio.ini's [band_*].duty_cycle and this band's row in "
              "src/common/bands/Bands.h disagree: the first-boot profile's `set dutycycle` line "
              "is built from the first and the band is defined by the second");

// --- the table ---------------------------------------------------------------------------------
//
// Two lines, and the rule that keeps it two: **a line is here only where a document of this
// project gives the value** - spec/SYSTEM.md S-12 for the first, and the band itself for the
// second, which is a regulation and not a judgement. The reviewer fills the rest from the
// network's own standard (tasks/phase-3/reports/03.md lists every `set` key of the pinned CLI
// with upstream's default beside it, and meshguide.dk's recommendation where there is one); a
// session with nobody to ask does not invent a value for a radio nobody has measured or a site
// nobody has visited.
//
// What is deliberately *not* here:
//
//   - the **channel** (`set radio`): the build's default, so that a unit is on the right
//     frequency even if this profile never ran (platformio.ini's [band_868]). Its duty cycle is
//     the one thing of that block the build cannot set by itself - upstream keeps it in the
//     preferences and not in a define - which is why it is a line of this table instead;
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
    // The band's legal ceiling, and the one line of this table that is **generated**: the
    // percentage is platformio.ini's [band_868].duty_cycle by way of TWINOAK_BAND_DUTY_CYCLE,
    // held against src/common/bands/Bands.h's row by the static_assert above. 869.618 MHz is in
    // the 869.400-869.650 MHz sub-band of ERC Recommendation 70-03, Annex 1, row h1.7 - 500 mW
    // e.r.p. and a duty cycle of 10 % - and upstream starts a repeater at 50 %
    // (MyMesh's constructor sets `airtime_factor` to 1.0, examples/simple_repeater/MyMesh.cpp:890
    // at the pin d9296435, and `Dispatcher::begin` makes the budget 1/(1+1) of a one-hour window,
    // src/Dispatcher.cpp:25-27). `set dutycycle <dc>` is upstream's own way to say it: it writes
    // `airtime_factor = (100 / dc) - 1` and answers `OK - 10.0%`
    // (CommonCLI::handleSetCmd, src/helpers/CommonCLI.cpp:450-461), which the reply rule below
    // accepts, and `get dutycycle` reads the same number back out of the factor (`:809-814`).
    //
    // 10 % is also the lowest duty cycle this upstream can keep: `loadPrefs` clamps
    // `airtime_factor` to 9.0 (`:111`), which is exactly 100/10 - 1, so a site that wants less
    // than the band allows cannot have it from this preference. A line here asking for less would
    // be accepted, saved and then loaded back as 10 %.
    {"set dutycycle " TWINOAK_PROFILE_TEXT(TWINOAK_BAND_DUTY_CYCLE),
     "the duty cycle the home channel's band allows, which upstream starts at 50 % "
     "(ERC Recommendation 70-03, Annex 1, 869.400-869.650 MHz; platformio.ini's [band_868])"},
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
