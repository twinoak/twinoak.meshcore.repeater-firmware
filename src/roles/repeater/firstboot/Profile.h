// The first-boot profile: the console command lines a unit runs **once**, on the boot that finds
// no saved preferences, so that a board fresh from the flasher is already a well-behaved member
// of the Danish mesh (spec/SYSTEM.md S-12, S-13; docs/provisioning.md).
//
// **It is a table of text on purpose.** Three reasons, and they are the whole design:
//
//   1. it uses nothing but upstream's public command interface - the same `handleCommand(0, ...)`
//      a person at the USB port and the Walter over the link use - so no preference field, no
//      private member and no save path of upstream's is touched by name;
//   2. a pin bump that renames a key, narrows its range or drops it does not break the build and
//      does not corrupt a preference: it comes back as a reply that is not `OK`, which is counted
//      and readable afterwards (`sensor get firstboot.state`);
//   3. Thomas can read the table and change it without reading C++.
//
// Host-portable: no Arduino, no MeshCore. The board side (src/roles/repeater/app/FirstBoot.cpp)
// decides *when* - the same test the random admin password of phase 1's task 01 is decided by,
// in the same place - and hands this file the command interface and the console's own two
// buffers. What a line means is upstream's; what this file does is run the lines in order, judge
// each reply, and remember the outcome. test/test_firstboot holds all of it against a fake.
//
// **The radio's channel is not in the table.** It is the build's default (platformio.ini's
// [band_*] block, phase 3's task 01), because a unit must be on the right frequency even if this
// profile never ran at all. The band's **duty cycle** is the one part of that block that cannot
// be a build default - upstream keeps it in the preferences, as `airtime_factor` - so it is a
// line of the table instead, and that line is *built* from the band rather than written
// (TWINOAK_BAND_DUTY_CYCLE, held against src/common/bands/Bands.h's row where the table is).
//
// **One line is built from the node itself**, for the opposite reason: the flood advert's
// interval must *not* be the same number on every unit, or a batch flashed in one evening floods
// the mesh in step for as long as it runs. It is derived from the node's own public key
// (`floodAdvertIntervalHours` below), which is the only thing a unit has at its first boot that
// its neighbours do not share. Everything else in the table is text.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "common/link/session/Interfaces.h"  // CliAccess: one command line in, one reply out

namespace twinoak {
namespace firstboot {

// What a line that cannot be *written* is built from: this node's own identity, and nothing
// else. One line of the table needs it - the flood advert's interval, which is derived from the
// key so that units flashed together do not advertise in step (docs/provisioning.md section 1).
// The board passes the key upstream has already made or loaded by then
// (src/roles/repeater/main.cpp creates `the_mesh.self_id` a dozen lines above the firstBoot()
// call, which is why no order had to move); a host test passes any bytes, or none.
struct NodeFacts {
  const uint8_t* pub_key;
  size_t pub_key_len;
};

// What builds such a line: it writes the whole command line into `out`, which holds `cap` bytes
// including the NUL, and answers whether it could. A false is a line the runner refuses without
// running - the same outcome as a line upstream does not take - so a unit whose key could not be
// read keeps every other line of the table.
typedef bool (*BuildLine)(const NodeFacts& facts, char* out, size_t cap);

// One line of the profile: what is run, and why it is there. `why` is for a person reading the
// table and for docs/provisioning.md; nothing of the firmware reads it.
//
// `build` is null for a line that is text, which is all but one of them. Where it is set,
// `command` is how the table *shows* the line - `set flood.advert.interval <h>` - and is what a
// refusal prints; the line really sent is the one `build` writes. So the table still reads as a
// table of console lines, and the one value no document can state in advance is still a value
// this file decides and nothing else does.
struct ProfileLine {
  const char* command;
  const char* why;
  BuildLine build;
};

// --- the one line that is derived from the node ------------------------------------------------

// The flood advert interval a node's own key gives it, in hours: **60 + (the key's first byte
// mod 26)**, so 60-85 inclusive, every hour of that range reachable, and the same key always
// giving the same hour (docs/provisioning.md section 1; Thomas's decision of 22 October 2026,
// tasks/phase-3/FOLLOWUPS.md). Upstream's own default is 47 hours for every unit, so a batch
// flashed together floods its adverts in step; a byte of the key is the one thing a unit has at
// first boot that its neighbours do not share.
//
// The first byte is the one upstream itself gives meaning to - it is the node's hash, the byte
// main.cpp redraws an identity for when it is 0x00 or 0xFF - so it is the byte a reader can
// check against a `Repeater ID` printed on a console. 0 when there is no key: no line is built
// from a node that cannot say who it is.
//
// The two numbers of that rule, so that nothing has to repeat them: the lowest hour and how many
// hours the range holds. 60 and 26, which is 60-85.
const int kFloodAdvertHoursMin = 60;
const int kFloodAdvertHoursSpan = 26;

int floodAdvertIntervalHours(const NodeFacts& facts);

// `set flood.advert.interval <h>` with the hours above, written into `out`. False, and nothing
// written, when there is no key or `cap` cannot hold the line.
bool buildFloodAdvertIntervalLine(const NodeFacts& facts, char* out, size_t cap);

// The profile as shipped. Short on purpose: a line is here only where a document of this project
// gives the value - spec/SYSTEM.md S-12 (with meshmaster.dk's D-33 behind it) for the path
// hashes, the band's own regulation for the duty cycle, and the Danish standard as Thomas
// settled it on 22 October 2026 for the last three and for the advert interval's 60-85 hours
// (tasks/phase-3/FOLLOWUPS.md, tasks/phase-3.5/05-the-danish-standard.md). Everything else a
// unit needs is per-site and belongs to provisioning (docs/provisioning.md), or is already
// upstream's default.
extern const ProfileLine kProfile[];
extern const int kProfileCount;

// What a run came to. `refused` counts the lines whose reply was not an acceptance; the rest ran.
// `first_refused` is the command of the first of them, or nullptr - the console prints it, which
// is safe because no line of the profile may carry a password (test/test_firstboot holds that).
struct ProfileResult {
  int ran = 0;
  int refused = 0;
  const char* first_refused = nullptr;
};

// Whether a reply from upstream's interpreter means "the line was taken".
//
// Upstream has no status code: a CLI line's outcome is the English in its reply. Across the whole
// of CommonCLI.cpp, CommonRadioPrefs.cpp (the radio keys, since repeater-v1.18.0) and MyMesh.cpp
// at the pin 81dbae59, every acceptance starts with `OK` or `ok`,
// optionally wrapped in brackets, and no refusal does:
//
//   accepted   "OK" (most of `set`)            "OK - reboot to apply" (`set radio`, `set freq`)
//              "OK - 10.0%" (`set dutycycle`)  "OK, reboot to apply! ..." (`set prv.key`)
//              "ok" (`sensor set`)             "(OK - stats reset)" (`clear stats`)
//   refused    "Error, must be 0,1, or 2"   "ERROR: dutycycle must be 1-100"
//              "Error: unsupported"         "unknown config: <key>"   "Unknown command"
//              "can't find custom var" (`sensor set` of a key that is not ours)
//
// Two replies are neither, and both are **refused** here deliberately:
//
//   - `password <new>` answers `password now: <the password>`. A password line may not be in the
//     profile at all (that is a test), and a profile that grew one would be refused rather than
//     silently accepted - and its value would never be printed, because a refusal prints the
//     command and not the reply.
//   - a `get` line answers `> <value>`, and an empty reply means the interpreter said nothing.
//     Neither is a line that changed anything, so neither counts as done.
bool replyAccepted(const char* reply);

// Runs `count` lines in order through `cli`, counting what was refused. A refused line does not
// stop the run: the lines are independent settings and a unit with four of five is better than a
// unit with one of five, and the one that was refused is on the console and in the state below.
//
// `line` is a mutable copy of each command, because upstream's interpreter terminates a parameter
// in place (MyMesh::handleCommand, examples/simple_repeater/MyMesh.cpp:1234 at the pin); a line
// that does not fit it is refused without being run. `reply` is the console's reply buffer: the
// first link::kCliReplyMax bytes of it are the window upstream's own console gives its
// interpreter, and anything beyond that is the guard behind it (src/common/link/session/Session.h
// keeps the same shape for the link). Both buffers are the caller's, so that the board can lend
// the console's own - they are idle while setup() runs - and a test can put them on the stack.
//
// `facts` is the whole of the interface's growth for a line built at run time: a line whose
// `build` is set is written into the same `line` buffer the text lines are copied into, from
// these facts and from nothing else, and is refused without being run when it cannot be
// (`first_refused` is then the table's own text for it, which carries no value of this node).
ProfileResult runProfile(const ProfileLine* lines, int count, const NodeFacts& facts,
                         link::CliAccess& cli, char* line, size_t line_cap, char* reply,
                         size_t reply_cap);

// A whole boot's worth of the above: **runs once**. `saved_prefs_found` is the one test of
// src/roles/repeater/app/FirstBoot.h, taken before upstream loaded the preferences; when it is
// true this boot runs no line at all and the state below says `never`. Otherwise the lines run
// and the state says `done` or `partial: <n> refused`. The state is set either way, so the
// setting answers for every boot and not only for a first one.
ProfileResult runFirstBootProfile(bool saved_prefs_found, const ProfileLine* lines, int count,
                                  const NodeFacts& facts, link::CliAccess& cli, char* line,
                                  size_t line_cap, char* reply, size_t reply_cap);

// --- what `sensor get firstboot.state` answers ------------------------------------------------
//
// Per boot, in RAM, and never saved: the question it answers is "what did *this* boot do", and a
// unit that was provisioned weeks ago says `never` because this boot found its settings and ran
// nothing. It is on upstream's `sensor` list rather than on the box link so that it can be read
// from the USB port, over the link, and by a mesh admin with a phone - the three ways in that a
// unit has before anybody has been to the site (docs/ARCHITECTURE.md section 4).

enum class State {
  kNever,    // settings existed already: this boot ran no line
  kDone,     // every line of the profile was taken
  kPartial,  // some line was refused; `refused` says how many
};

// The outcome of this boot, remembered for the setting below. Called once, by the board side.
void noteState(State state, int refused);

// `never`, `done` or `partial: <n> refused`. Valid for the life of the program; `never` until
// noteState() has been called, which is what a role that never runs a profile reads.
const char* stateText();

}  // namespace firstboot
}  // namespace twinoak
