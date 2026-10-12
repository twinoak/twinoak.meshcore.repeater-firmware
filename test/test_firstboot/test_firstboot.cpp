// src/roles/repeater/firstboot/Profile.{h,cpp}: the first-boot profile's mechanism - a list of
// lines in, a command interface it is given, a result out - and the table as shipped.
//
// What is checked: the rule that decides whether a reply from upstream's interpreter means "the
// line was taken" (every acceptance and every refusal upstream can produce for a `set`, and the
// two replies that are neither); the runner with every line accepted, with one refused in the
// middle, with an empty table, with a line too long for the console's buffer and with a reply
// that writes into the guard; that it **runs once** - with settings present nothing runs at all
// and the state says `never`; and the table as shipped, which is held to the rails that make it
// safe to run unattended on a board nobody is watching.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "roles/repeater/firstboot/Profile.h"

namespace {

using twinoak::firstboot::kProfile;
using twinoak::firstboot::kProfileCount;
using twinoak::firstboot::ProfileLine;
using twinoak::firstboot::ProfileResult;
using twinoak::firstboot::replyAccepted;
using twinoak::firstboot::runFirstBootProfile;
using twinoak::firstboot::runProfile;
using twinoak::firstboot::State;
using twinoak::firstboot::stateText;

// The console's two buffers, as src/roles/repeater/main.cpp sizes them: the line upstream's
// interpreter may write into, and the 160-byte reply window with 64 bytes of guard behind it.
struct Buffers {
  char line[160] = {0};
  char reply[160 + 64] = {0};
};

// A command interface that answers from a script and remembers every line it was given. The
// firmware's own is upstream's handleCommand(0, ...) (src/roles/repeater/app/FirstBoot.cpp); this
// one is the only way a host can watch the runner work.
class FakeCli : public twinoak::link::CliAccess {
 public:
  std::vector<std::string> seen;
  std::vector<std::string> answers;  // one per call, in order; the last one repeats
  std::string default_answer = "OK";
  // When set, the answer is written at full length whatever the window is - which is how
  // upstream's own `sensor list` can write past it.
  size_t pad_to = 0;

  size_t runCommand(char* line, char* reply, size_t reply_cap) override {
    seen.push_back(std::string(line));
    std::string answer = default_answer;
    if (!answers.empty()) {
      answer = answers[seen.size() - 1 < answers.size() ? seen.size() - 1 : answers.size() - 1];
    }
    if (pad_to > answer.size()) answer.append(pad_to - answer.size(), 'x');
    // The interpreter is given reply_cap bytes and writes its string; a command that writes past
    // that writes into whatever the caller put behind it, which is what pad_to reproduces.
    const size_t n = answer.size();
    memcpy(reply, answer.data(), n);
    reply[n] = 0;
    return n < reply_cap ? n : reply_cap;
  }
};

}  // namespace

// ----------------------------------------------------------------------------- the reply rule

TEST(TheReplyRule, EveryAcceptanceUpstreamCanProduceIsOne) {
  // CommonCLI.cpp's `set` branches, MyMesh.cpp and the `sensor set` branch, at the pin d9296435.
  const char* const yes[] = {
      "OK",                                     // most of `set`
      "OK - reboot to apply",                   // `set radio`, `set freq`
      "OK - 10.0%",                             // `set dutycycle`
      "OK - interval rounded to 8",             // `set agc.reset.interval`
      "OK - repeat is now ON",                  // `set repeat`
      "OK - LoRa FEM RX gain on",               // `set radio.fem.rxgain`
      "OK, reboot to apply! New pubkey: aabb",  // `set prv.key`
      "OK - multiplier set to 1.500",           // `set adc.multiplier`
      "ok",                                     // `sensor set`
      "(OK - stats reset)",                     // `clear stats`
      "  OK",                                   // and with the leading spaces some replies carry
  };
  for (const char* reply : yes) EXPECT_TRUE(replyAccepted(reply)) << "'" << reply << "'";
}

TEST(TheReplyRule, EveryRefusalUpstreamCanProduceIsNotOne) {
  const char* const no[] = {
      "Error, must be 0,1, or 2",             // `set path.hash.mode 3`
      "ERROR: dutycycle must be 1-100",       // `set dutycycle 0`
      "Error: interval range is 3-168 hours",  // `set flood.advert.interval 1`
      "Error, bad chars",                     // `set name a:b`
      "Error: unsupported",                   // `set radio.fem.rxgain` on a board without one
      "Error, bad key",                       // `set prv.key xx`
      "Error, max 64",                        // `set flood.max 65`
      "Error, must be 0-20",                  // `set rxdelay 21`
      "unknown config: nonsense 1",           // a key this pin does not have
      "Unknown command",                      // not a `set` at all
      "can't find custom var",                // `sensor set` of a key that is not ours
  };
  for (const char* reply : no) EXPECT_FALSE(replyAccepted(reply)) << "'" << reply << "'";
}

TEST(TheReplyRule, TheTwoRepliesThatAreNeitherAreRefusedOnPurpose) {
  // A success that is not an acceptance: upstream's `password <new>` echoes the new password
  // back. No line of the profile may carry a password (the rail below), and one that did would
  // be refused rather than silently taken - and a refusal prints the command, never the reply.
  EXPECT_FALSE(replyAccepted("password now: hunter2"));
  // A `get` answers a value, which is not a line that changed anything.
  EXPECT_FALSE(replyAccepted("> 869.618,62.5,8,8"));
  EXPECT_FALSE(replyAccepted("??: nosuchkey"));
  EXPECT_FALSE(replyAccepted("null"));
  // Silence is not consent: `get acl` is one of upstream's own commands that answers nothing.
  EXPECT_FALSE(replyAccepted(""));
  EXPECT_FALSE(replyAccepted(nullptr));
  // And `ok` is a word, not a prefix.
  EXPECT_FALSE(replyAccepted("okay then"));
  EXPECT_FALSE(replyAccepted("okee"));
  EXPECT_FALSE(replyAccepted("o"));
}

// ----------------------------------------------------------------------------- the runner

TEST(TheRunner, EveryLineAcceptedIsEveryLineRun) {
  const ProfileLine lines[] = {
      {"set path.hash.mode 1", "why one"},
      {"set loop.detect moderate", "why two"},
      {"set flood.max.unscoped 15", "why three"},
  };
  FakeCli cli;
  Buffers b;
  const ProfileResult r = runProfile(lines, 3, cli, b.line, sizeof(b.line), b.reply,
                                     sizeof(b.reply));
  EXPECT_EQ(3, r.ran);
  EXPECT_EQ(0, r.refused);
  EXPECT_EQ(nullptr, r.first_refused);
  ASSERT_EQ(3u, cli.seen.size());
  EXPECT_EQ("set path.hash.mode 1", cli.seen[0]) << "and in the table's own order";
  EXPECT_EQ("set loop.detect moderate", cli.seen[1]);
  EXPECT_EQ("set flood.max.unscoped 15", cli.seen[2]);
}

TEST(TheRunner, OneRefusedInTheMiddleAndTheRestStillRun) {
  const ProfileLine lines[] = {
      {"set path.hash.mode 1", "taken"},
      {"set nonsense 1", "a key a pin bump removed"},
      {"set loop.detect moderate", "taken after the refusal"},
  };
  FakeCli cli;
  cli.answers = {"OK", "unknown config: nonsense 1", "OK"};
  Buffers b;
  const ProfileResult r = runProfile(lines, 3, cli, b.line, sizeof(b.line), b.reply,
                                     sizeof(b.reply));
  EXPECT_EQ(2, r.ran) << "a refusal does not stop the run: the lines are independent settings";
  EXPECT_EQ(1, r.refused);
  ASSERT_NE(nullptr, r.first_refused);
  EXPECT_STREQ("set nonsense 1", r.first_refused);
  ASSERT_EQ(3u, cli.seen.size()) << "and the line after it was still given to the interpreter";
}

TEST(TheRunner, AnEmptyTableRunsNothingAndRefusesNothing) {
  FakeCli cli;
  Buffers b;
  const ProfileResult r = runProfile(kProfile, 0, cli, b.line, sizeof(b.line), b.reply,
                                     sizeof(b.reply));
  EXPECT_EQ(0, r.ran);
  EXPECT_EQ(0, r.refused);
  EXPECT_EQ(nullptr, r.first_refused);
  EXPECT_TRUE(cli.seen.empty());
  // Which is `done`, and rightly: a table with nothing in it has nothing left undone.
  FakeCli cli2;
  Buffers b2;
  runFirstBootProfile(false, kProfile, 0, cli2, b2.line, sizeof(b2.line), b2.reply,
                      sizeof(b2.reply));
  EXPECT_STREQ("done", stateText());
}

TEST(TheRunner, ALineTooLongForTheConsolesBufferIsRefusedWithoutBeingRun) {
  // 160 characters, which does not fit the 160-byte buffer upstream's console gives a line: a
  // truncated `set` is a wrong value and not a missing one, so it is never handed over.
  const std::string too_long = "set owner.info " + std::string(145, 'x');
  ASSERT_EQ(160u, too_long.size());
  const std::string just_fits = too_long.substr(0, 159);
  const ProfileLine lines[] = {
      {too_long.c_str(), "one byte too many"},
      {just_fits.c_str(), "and the one that fits"},
      {"", "nothing at all"},
  };
  FakeCli cli;
  Buffers b;
  const ProfileResult r = runProfile(lines, 3, cli, b.line, sizeof(b.line), b.reply,
                                     sizeof(b.reply));
  EXPECT_EQ(1, r.ran);
  EXPECT_EQ(2, r.refused);
  EXPECT_STREQ(too_long.c_str(), r.first_refused);
  ASSERT_EQ(1u, cli.seen.size()) << "only the line that fits reached the interpreter";
  EXPECT_EQ(just_fits, cli.seen[0]);
}

TEST(TheRunner, AReplyThatWritesPastTheWindowGoesIntoTheGuardAndIsStillJudged) {
  // Upstream's own `sensor list` can write past the 160 bytes it is given (phase 1's report 05
  // has the arithmetic, and it is why the console keeps 64 bytes behind the window). A reply
  // that long is judged by the window's own bytes and nothing is written past the buffer.
  const ProfileLine lines[] = {{"set path.hash.mode 1", "a reply that overruns"}};
  FakeCli cli;
  cli.default_answer = "OK - ";
  cli.pad_to = 200;  // 40 bytes into the 64-byte guard
  Buffers b;
  const ProfileResult r = runProfile(lines, 1, cli, b.line, sizeof(b.line), b.reply,
                                     sizeof(b.reply));
  EXPECT_EQ(1, r.ran) << "the reply still begins with OK";
  EXPECT_EQ(0, r.refused);
  EXPECT_EQ(0, b.reply[159]) << "and what is left in the window is a 159-character string";
  EXPECT_EQ(0, b.reply[sizeof(b.reply) - 1]) << "with the end of the guard untouched";
}

TEST(TheRunner, NoBufferAtAllRunsNothing) {
  FakeCli cli;
  Buffers b;
  EXPECT_EQ(0, runProfile(kProfile, kProfileCount, cli, nullptr, 0, b.reply, sizeof(b.reply)).ran);
  EXPECT_EQ(0, runProfile(kProfile, kProfileCount, cli, b.line, sizeof(b.line), nullptr, 0).ran);
  EXPECT_EQ(0, runProfile(nullptr, 3, cli, b.line, sizeof(b.line), b.reply, sizeof(b.reply)).ran);
  EXPECT_TRUE(cli.seen.empty());
}

// ----------------------------------------------------------------------------- once, and only once

TEST(RunsOnce, WithSettingsPresentNothingRunsAndTheStateIsNever) {
  FakeCli cli;
  Buffers b;
  const ProfileResult r = runFirstBootProfile(/*saved_prefs_found=*/true, kProfile, kProfileCount,
                                              cli, b.line, sizeof(b.line), b.reply,
                                              sizeof(b.reply));
  EXPECT_EQ(0, r.ran);
  EXPECT_EQ(0, r.refused);
  EXPECT_TRUE(cli.seen.empty()) << "not one line of the profile reached the interpreter";
  EXPECT_STREQ("never", stateText());
}

TEST(RunsOnce, WithNoSettingsTheProfileRunsAndTheStateSaysWhatHappened) {
  FakeCli cli;
  Buffers b;
  const ProfileResult r = runFirstBootProfile(/*saved_prefs_found=*/false, kProfile,
                                              kProfileCount, cli, b.line, sizeof(b.line),
                                              b.reply, sizeof(b.reply));
  EXPECT_EQ(kProfileCount, r.ran);
  EXPECT_EQ(0, r.refused);
  EXPECT_EQ(size_t(kProfileCount), cli.seen.size());
  EXPECT_STREQ("done", stateText());

  // And with one of the shipped lines refused by a pin that does not know the key.
  FakeCli refusing;
  refusing.default_answer = "unknown config: path.hash.mode 1";
  Buffers b2;
  const ProfileResult r2 = runFirstBootProfile(false, kProfile, kProfileCount, refusing, b2.line,
                                               sizeof(b2.line), b2.reply, sizeof(b2.reply));
  EXPECT_EQ(0, r2.ran);
  EXPECT_EQ(kProfileCount, r2.refused);
  EXPECT_STREQ(("partial: " + std::to_string(kProfileCount) + " refused").c_str(), stateText());
}

TEST(TheState, IsTheThreeWordsTheSettingPromises) {
  twinoak::firstboot::noteState(State::kNever, 0);
  EXPECT_STREQ("never", stateText());
  twinoak::firstboot::noteState(State::kDone, 0);
  EXPECT_STREQ("done", stateText());
  twinoak::firstboot::noteState(State::kPartial, 1);
  EXPECT_STREQ("partial: 1 refused", stateText());
  twinoak::firstboot::noteState(State::kPartial, 12);
  EXPECT_STREQ("partial: 12 refused", stateText());
  // The longest thing it can ever say still fits a `sensor get` reply with room to spare.
  EXPECT_GT(40u, strlen(stateText()));
}

// --------------------------------------------------------------------- the table as shipped

TEST(TheTableAsShipped, IsTheOneLineThisTaskIsSureOf) {
  ASSERT_EQ(1, kProfileCount) << "a line is added only where a document of this project gives "
                                 "the value (tasks/phase-3/reports/03.md lists the rest)";
  EXPECT_STREQ("set path.hash.mode 1", kProfile[0].command)
      << "spec/SYSTEM.md S-12 and meshmaster.dk's D-33: two-byte path hashes in the "
         "adverts this node floods (MyMesh::sendSelfAdvertisement at the pin)";
}

TEST(TheTableAsShipped, EveryLineIsOneTheConsoleCouldHaveBeenTyped) {
  for (int i = 0; i < kProfileCount; i++) {
    const std::string command = kProfile[i].command == nullptr ? "" : kProfile[i].command;
    EXPECT_FALSE(command.empty()) << "line " << i;
    // 159 characters and the NUL is what upstream's `static char command[160]` holds.
    EXPECT_GE(159u, command.size()) << command;
    for (char c : command) {
      const unsigned char u = static_cast<unsigned char>(c);
      EXPECT_TRUE(u >= 0x20 && u < 0x7f) << "a control or non-ASCII byte in '" << command << "'";
    }
    // Every line is a setting, written the one way that answers `OK`. A `get` would be judged a
    // refusal (the reply rule above), and nothing else belongs in a table that runs unattended.
    EXPECT_TRUE(command.rfind("set ", 0) == 0 || command.rfind("sensor set ", 0) == 0)
        << command;
  }
}

TEST(TheTableAsShipped, CarriesNoPasswordAndNothingThatRestartsTheNode) {
  // S-13: no default password is ever in an image, so no line of a table compiled into one may
  // mention one - not `password <new>`, not `set guest.password <x>`. The admin password of a
  // first boot is the random one src/roles/repeater/app/AdminPassword.h draws, and the guest
  // password is a person's to set (docs/provisioning.md).
  //
  // And nothing that does not return: these lines run inside setup(), and upstream's `reboot`,
  // `poweroff`, `shutdown`, `clkreboot` and `erase` would take the board with them.
  // Upstream recognises a command by a memcmp against the start of the line, so the first word
  // is what decides - which is why these are tested as prefixes and `set advert.interval 0`,
  // were the reviewer to add it, would not be caught by them.
  const char* const forbidden_first[] = {"password", "reboot",    "poweroff", "shutdown",
                                         "clkreboot", "erase",    "advert",   "start",
                                         "log",       "tempradio"};
  for (int i = 0; i < kProfileCount; i++) {
    const std::string command = kProfile[i].command;
    EXPECT_EQ(std::string::npos, command.find("password"))
        << "a password in a table compiled into an image (S-13): " << command;
    for (const char* word : forbidden_first) {
      EXPECT_NE(0u, command.rfind(word, 0))  // 0 would mean the line begins with it
          << "'" << word << "' does not belong in the profile: " << command;
    }
  }
}

TEST(TheTableAsShipped, EveryLineSaysWhyItIsThere) {
  // The reason beside the line is what makes the table readable by somebody who does not read
  // C++, and it is what docs/provisioning.md prints as the comment above each line.
  for (int i = 0; i < kProfileCount; i++) {
    ASSERT_NE(nullptr, kProfile[i].why) << "line " << i;
    EXPECT_LT(10u, strlen(kProfile[i].why)) << "line " << i << " needs a reason, not a word";
  }
}

TEST(TheTableAsShipped, SetsNothingTheBuildAlreadyDecides) {
  // The channel is the build's default (platformio.ini's [band_*]), so that a unit is on the
  // right frequency even if this profile never ran. A `set radio` or `set freq` line here would
  // mean the frequency depended on a boot having gone well.
  const char* const the_builds_own[] = {"set radio ", "set freq ", "set tx "};
  for (int i = 0; i < kProfileCount; i++) {
    const std::string command = kProfile[i].command;
    for (const char* prefix : the_builds_own) {
      EXPECT_NE(0u, command.rfind(prefix, 0))  // 0 would mean it starts with it
          << "'" << prefix << "' is the build's, not the profile's: " << command;
    }
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
