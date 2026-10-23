// src/roles/repeater/firstboot/Profile.{h,cpp}: the first-boot profile's mechanism - a list of
// lines in, a command interface it is given, a result out - and the table as shipped.
//
// What is checked: the rule that decides whether a reply from upstream's interpreter means "the
// line was taken" (every acceptance and every refusal upstream can produce for a `set`, and the
// two replies that are neither); the runner with every line accepted, with one refused in the
// middle, with an empty table, with a line too long for the console's buffer and with a reply
// that writes into the guard; that it **runs once** - with settings present nothing runs at all
// and the state says `never`; the one line that is built from the node's own key (its bounds,
// its mapping, that it is the same for the same key, and the text it produces); and the table as
// shipped, which is held to the rails that make it safe to run unattended on a board nobody is
// watching - and, for the duty-cycle line, to the band's own row, because that line is built
// from platformio.ini's [band_*] block and not written (src/common/bands/Bands.h).

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "common/bands/Bands.h"
#include "roles/repeater/firstboot/Profile.h"

namespace {

using twinoak::firstboot::buildFloodAdvertIntervalLine;
using twinoak::firstboot::floodAdvertIntervalHours;
using twinoak::firstboot::kFloodAdvertHoursMin;
using twinoak::firstboot::kFloodAdvertHoursSpan;
using twinoak::firstboot::kProfile;
using twinoak::firstboot::kProfileCount;
using twinoak::firstboot::NodeFacts;
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

// A node's key, as the board hands it over (src/roles/repeater/app/FirstBoot.cpp passes
// `the_mesh.self_id.pub_key`): 32 bytes whose **first** one is all the derived line reads.
struct Key {
  uint8_t bytes[32];
  explicit Key(uint8_t first) {
    for (size_t i = 0; i < sizeof(bytes); i++) bytes[i] = uint8_t(first + i * 7 + 1);
    bytes[0] = first;
  }
  NodeFacts facts() const { return NodeFacts{bytes, sizeof(bytes)}; }
};

// No key at all, which is what a caller that cannot say who the node is passes.
const NodeFacts kNoKey = {nullptr, 0};

// The key every test that is not about the key itself hands over. 13 is the first byte, so the
// derived line asks for 60 + 13 = 73 hours.
const Key kKey(13);
const int kKeyHours = 73;

}  // namespace

// ----------------------------------------------------------------------------- the reply rule

TEST(TheReplyRule, EveryAcceptanceUpstreamCanProduceIsOne) {
  // CommonCLI.cpp's `set` branches, CommonRadioPrefs.cpp's (the radio keys, since
  // repeater-v1.18.0), MyMesh.cpp and the `sensor set` branch, at the pin 81dbae59.
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
      "ERROR: af must be 0-9",                // `set af 12`, refused since repeater-v1.18.0
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
      {"set path.hash.mode 1", "why one", nullptr},
      {"set loop.detect moderate", "why two", nullptr},
      {"set flood.max.unscoped 15", "why three", nullptr},
  };
  FakeCli cli;
  Buffers b;
  const ProfileResult r = runProfile(lines, 3, kKey.facts(), cli, b.line, sizeof(b.line), b.reply,
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
      {"set path.hash.mode 1", "taken", nullptr},
      {"set nonsense 1", "a key a pin bump removed", nullptr},
      {"set loop.detect moderate", "taken after the refusal", nullptr},
  };
  FakeCli cli;
  cli.answers = {"OK", "unknown config: nonsense 1", "OK"};
  Buffers b;
  const ProfileResult r = runProfile(lines, 3, kKey.facts(), cli, b.line, sizeof(b.line), b.reply,
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
  const ProfileResult r = runProfile(kProfile, 0, kKey.facts(), cli, b.line, sizeof(b.line), b.reply,
                                     sizeof(b.reply));
  EXPECT_EQ(0, r.ran);
  EXPECT_EQ(0, r.refused);
  EXPECT_EQ(nullptr, r.first_refused);
  EXPECT_TRUE(cli.seen.empty());
  // Which is `done`, and rightly: a table with nothing in it has nothing left undone.
  FakeCli cli2;
  Buffers b2;
  runFirstBootProfile(false, kProfile, 0, kKey.facts(), cli2, b2.line, sizeof(b2.line), b2.reply,
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
      {too_long.c_str(), "one byte too many", nullptr},
      {just_fits.c_str(), "and the one that fits", nullptr},
      {"", "nothing at all", nullptr},
  };
  FakeCli cli;
  Buffers b;
  const ProfileResult r = runProfile(lines, 3, kKey.facts(), cli, b.line, sizeof(b.line), b.reply,
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
  const ProfileLine lines[] = {{"set path.hash.mode 1", "a reply that overruns", nullptr}};
  FakeCli cli;
  cli.default_answer = "OK - ";
  cli.pad_to = 200;  // 40 bytes into the 64-byte guard
  Buffers b;
  const ProfileResult r = runProfile(lines, 1, kKey.facts(), cli, b.line, sizeof(b.line), b.reply,
                                     sizeof(b.reply));
  EXPECT_EQ(1, r.ran) << "the reply still begins with OK";
  EXPECT_EQ(0, r.refused);
  EXPECT_EQ(0, b.reply[159]) << "and what is left in the window is a 159-character string";
  EXPECT_EQ(0, b.reply[sizeof(b.reply) - 1]) << "with the end of the guard untouched";
}

TEST(TheRunner, NoBufferAtAllRunsNothing) {
  FakeCli cli;
  Buffers b;
  const NodeFacts facts = kKey.facts();
  EXPECT_EQ(0, runProfile(kProfile, kProfileCount, facts, cli, nullptr, 0, b.reply,
                          sizeof(b.reply)).ran);
  EXPECT_EQ(0, runProfile(kProfile, kProfileCount, facts, cli, b.line, sizeof(b.line), nullptr,
                          0).ran);
  EXPECT_EQ(0, runProfile(nullptr, 3, facts, cli, b.line, sizeof(b.line), b.reply,
                          sizeof(b.reply)).ran);
  EXPECT_TRUE(cli.seen.empty());
}

// ----------------------------------------------------------------------------- once, and only once

TEST(RunsOnce, WithSettingsPresentNothingRunsAndTheStateIsNever) {
  FakeCli cli;
  Buffers b;
  const ProfileResult r = runFirstBootProfile(/*saved_prefs_found=*/true, kProfile, kProfileCount,
                                              kKey.facts(), cli, b.line, sizeof(b.line), b.reply,
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
                                              kProfileCount, kKey.facts(), cli, b.line,
                                              sizeof(b.line), b.reply, sizeof(b.reply));
  EXPECT_EQ(kProfileCount, r.ran);
  EXPECT_EQ(0, r.refused);
  EXPECT_EQ(size_t(kProfileCount), cli.seen.size());
  EXPECT_STREQ("done", stateText());

  // And with one of the shipped lines refused by a pin that does not know the key.
  FakeCli refusing;
  refusing.default_answer = "unknown config: path.hash.mode 1";
  Buffers b2;
  const ProfileResult r2 = runFirstBootProfile(false, kProfile, kProfileCount, kKey.facts(),
                                               refusing, b2.line, sizeof(b2.line), b2.reply,
                                               sizeof(b2.reply));
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

// ------------------------------------------------- the line the node's own key gives it

TEST(TheDerivedLine, EveryKeyGivesAnHourInsideTheRangeThomasChose) {
  // 60-85 inclusive, for every one of the 256 values a key's first byte can hold - including the
  // 0x00 and 0xFF that src/roles/repeater/main.cpp redraws an identity for, because it gives up
  // after ten tries and a unit that got one anyway still has to ask for a legal interval.
  for (int first = 0; first < 256; first++) {
    const Key key{uint8_t(first)};
    const int hours = floodAdvertIntervalHours(key.facts());
    EXPECT_LE(kFloodAdvertHoursMin, hours) << "first byte " << first;
    EXPECT_GE(kFloodAdvertHoursMin + kFloodAdvertHoursSpan - 1, hours) << "first byte " << first;
    // And inside upstream's own range for the key, which is 0 or 3-168 hours
    // (CommonCLI::handleSetCmd's `flood.advert.interval` branch at the pin 81dbae59): 85 is well
    // inside it, and a value outside it would be refused on the board and nowhere else.
    EXPECT_LE(3, hours);
    EXPECT_GE(168, hours);
  }
}

TEST(TheDerivedLine, TheMappingIsTheRuleTheDocumentStates) {
  // h = 60 + (the key's first byte mod 26), and every hour of 60-85 is reached by some key, so
  // that a batch of units spreads over the whole range rather than over part of it.
  bool reached[kFloodAdvertHoursSpan] = {false};
  for (int first = 0; first < 256; first++) {
    const Key key{uint8_t(first)};
    const int hours = floodAdvertIntervalHours(key.facts());
    ASSERT_EQ(kFloodAdvertHoursMin + (first % kFloodAdvertHoursSpan), hours);
    reached[hours - kFloodAdvertHoursMin] = true;
  }
  for (int i = 0; i < kFloodAdvertHoursSpan; i++) {
    EXPECT_TRUE(reached[i]) << "no key gives " << (kFloodAdvertHoursMin + i) << " hours";
  }
}

TEST(TheDerivedLine, TheSameKeyAlwaysGivesTheSameHour) {
  // A unit that is rebooted, or erased and provisioned again with the same identity, asks for the
  // same interval: nothing here is random and nothing here is a clock.
  const Key key(0x7f);
  const int once = floodAdvertIntervalHours(key.facts());
  for (int i = 0; i < 5; i++) EXPECT_EQ(once, floodAdvertIntervalHours(key.facts()));
  // Only the first byte decides, which is the byte a `Repeater ID:` line starts with and the one
  // tools/bench/walk.py recomputes the expected hour from.
  Key other(0x7f);
  other.bytes[1] ^= 0xff;
  other.bytes[31] ^= 0xff;
  EXPECT_EQ(once, floodAdvertIntervalHours(other.facts()));
  // A different first byte is a different hour whenever the two differ mod 26.
  EXPECT_NE(once, floodAdvertIntervalHours(Key(uint8_t(0x7f + 1)).facts()));
}

TEST(TheDerivedLine, WithNoKeyThereIsNoHourAndNoLine) {
  char out[64] = "untouched";
  EXPECT_EQ(0, floodAdvertIntervalHours(kNoKey));
  EXPECT_FALSE(buildFloodAdvertIntervalLine(kNoKey, out, sizeof(out)));
  const NodeFacts empty = {reinterpret_cast<const uint8_t*>("x"), 0};
  EXPECT_EQ(0, floodAdvertIntervalHours(empty));
  EXPECT_FALSE(buildFloodAdvertIntervalLine(empty, out, sizeof(out)));
}

TEST(TheDerivedLine, TheTextIsTheConsoleLineUpstreamTakes) {
  char out[64] = {0};
  ASSERT_TRUE(buildFloodAdvertIntervalLine(Key(13).facts(), out, sizeof(out)));
  EXPECT_STREQ("set flood.advert.interval 73", out) << "60 + (13 mod 26)";
  ASSERT_TRUE(buildFloodAdvertIntervalLine(Key(0).facts(), out, sizeof(out)));
  EXPECT_STREQ("set flood.advert.interval 60", out) << "the bottom of the range";
  ASSERT_TRUE(buildFloodAdvertIntervalLine(Key(25).facts(), out, sizeof(out)));
  EXPECT_STREQ("set flood.advert.interval 85", out) << "and the top of it";
  // Upstream's own branch is `memcmp(config, "flood.advert.interval ", 22)` after the `set `, so
  // the one space and the exact key spelling are what make the line a line at all.
  EXPECT_EQ(0, strncmp(out, "set flood.advert.interval ", 26));
}

TEST(TheDerivedLine, ABufferTooSmallForItIsNoLineAtAll) {
  // "set flood.advert.interval 73" is 28 characters, so 29 bytes is the smallest buffer that
  // holds it. One less writes nothing and refuses, rather than sending a truncated `set`.
  char out[29] = {0};
  EXPECT_TRUE(buildFloodAdvertIntervalLine(Key(13).facts(), out, sizeof(out)));
  EXPECT_EQ(28u, strlen(out));
  char tight[28] = {0};
  EXPECT_FALSE(buildFloodAdvertIntervalLine(Key(13).facts(), tight, sizeof(tight)));
  EXPECT_STREQ("", tight);
  EXPECT_FALSE(buildFloodAdvertIntervalLine(Key(13).facts(), nullptr, 8));
}

TEST(TheRunner, ABuiltLineReachesTheInterpreterAsTheLineAndNotAsTheTablesText) {
  const ProfileLine lines[] = {
      {"set path.hash.mode 1", "a line that is text", nullptr},
      {"set flood.advert.interval <h>", "a line that is built", buildFloodAdvertIntervalLine},
  };
  FakeCli cli;
  Buffers b;
  const ProfileResult r = runProfile(lines, 2, kKey.facts(), cli, b.line, sizeof(b.line), b.reply,
                                     sizeof(b.reply));
  EXPECT_EQ(2, r.ran);
  EXPECT_EQ(0, r.refused);
  ASSERT_EQ(2u, cli.seen.size());
  EXPECT_EQ("set path.hash.mode 1", cli.seen[0]);
  EXPECT_EQ("set flood.advert.interval " + std::to_string(kKeyHours), cli.seen[1])
      << "the table's `<h>` is what a person reads; what upstream is given is the hour";
}

TEST(TheRunner, ABuiltLineThatCannotBeBuiltIsRefusedWithoutBeingRunAndTheRestStillRuns) {
  const ProfileLine lines[] = {
      {"set flood.advert.interval <h>", "a line that is built", buildFloodAdvertIntervalLine},
      {"set path.hash.mode 1", "and the line after it", nullptr},
  };
  FakeCli cli;
  Buffers b;
  const ProfileResult r = runProfile(lines, 2, kNoKey, cli, b.line, sizeof(b.line), b.reply,
                                     sizeof(b.reply));
  EXPECT_EQ(1, r.ran);
  EXPECT_EQ(1, r.refused);
  ASSERT_NE(nullptr, r.first_refused);
  // The table's own text, which is what the console prints - and it carries no value of this
  // node, which is the reason a built line has a text at all.
  EXPECT_STREQ("set flood.advert.interval <h>", r.first_refused);
  ASSERT_EQ(1u, cli.seen.size()) << "the line that could not be built was never handed over";
  EXPECT_EQ("set path.hash.mode 1", cli.seen[0]);
}

// --------------------------------------------------------------------- the table as shipped

TEST(TheTableAsShipped, IsTheSixLinesThisProjectHasADocumentFor) {
  ASSERT_EQ(6, kProfileCount) << "a line is added only where a document of this project gives "
                                 "the value (tasks/phase-3/reports/03.md lists the rest, and "
                                 "the three delays are still nobody's to guess)";
  EXPECT_STREQ("set path.hash.mode 1", kProfile[0].command)
      << "spec/SYSTEM.md S-12 and meshmaster.dk's D-33: two-byte path hashes in the "
         "adverts this node floods (MyMesh::sendSelfAdvertisement at the pin)";
  // The order is the order docs/provisioning.md section 1 prints and docs/bench.md section 10
  // reads back: the specification's line, the band's, then the Danish standard's four.
  EXPECT_STREQ("set agc.reset.interval 4", kProfile[2].command);
  EXPECT_STREQ("set loop.detect moderate", kProfile[3].command);
  EXPECT_STREQ("set flood.max.unscoped 15", kProfile[4].command);
  EXPECT_STREQ("set flood.advert.interval <h>", kProfile[5].command);
}

TEST(TheTableAsShipped, TheDanishStandardsThreeStaticLinesAreTheValuesThomasChose) {
  // Thomas's decision of 22 October 2026 (tasks/phase-3/FOLLOWUPS.md, *The rest of the Danish
  // standard*), each against upstream's own default at the pin 81dbae59 - so that a bump that
  // changed a default would not quietly turn one of these lines into a no-op nobody noticed.
  //
  //   agc.reset.interval   0 (off; src/Dispatcher.h:172, MyMesh.h:153-155)        -> 4 seconds
  //   loop.detect          minimal since repeater-v1.18.0 (MyMesh.cpp:911; off before) -> moderate
  //   flood.max.unscoped   64 (MyMesh.cpp:907)                                    -> 15 hops
  //
  // The reply each of them gets is one the rule above accepts: `OK - interval rounded to 4` for
  // the first (CommonRadioPrefs.cpp:167-173: the value is stored as seconds/4 and echoed back
  // times 4) and a plain `OK` for the other two (CommonCLI.cpp:587-606, :550-558).
  EXPECT_TRUE(replyAccepted("OK - interval rounded to 4"));
  EXPECT_TRUE(replyAccepted("OK"));
  EXPECT_FALSE(replyAccepted("Error, must be: off, minimal, moderate, or strict"));
  EXPECT_FALSE(replyAccepted("Error, max 64"));
  // And no line of the table asks for a value outside what the pinned CLI takes.
  for (int i = 0; i < kProfileCount; i++) {
    const std::string command = kProfile[i].command;
    if (command.rfind("set flood.max.unscoped ", 0) == 0) {
      EXPECT_GE(64, std::stoi(command.substr(23))) << "upstream answers `Error, max 64`";
    }
  }
}

TEST(TheTableAsShipped, TheAdvertIntervalLineIsBuiltFromTheKeyAndNothingElse) {
  // The sixth line is the only built one, and the only one whose `command` is not what is sent.
  for (int i = 0; i < kProfileCount; i++) {
    if (i == 5) {
      EXPECT_NE(nullptr, kProfile[i].build) << "line " << i << " is the derived one";
    } else {
      EXPECT_EQ(nullptr, kProfile[i].build)
          << "line " << i << " is text, and a table of text is the whole design (Profile.h)";
    }
  }
  // Run the shipped table against a key and read what the interpreter was given: the derived
  // line is the hour that key gives, in upstream's own spelling.
  FakeCli cli;
  Buffers b;
  const ProfileResult r = runProfile(kProfile, kProfileCount, kKey.facts(), cli, b.line,
                                     sizeof(b.line), b.reply, sizeof(b.reply));
  EXPECT_EQ(kProfileCount, r.ran);
  ASSERT_EQ(size_t(kProfileCount), cli.seen.size());
  EXPECT_EQ("set flood.advert.interval " + std::to_string(kKeyHours), cli.seen[5]);
  // And no line of the table carries the hour as text, which is the thing that would rot.
  for (int i = 0; i < kProfileCount; i++) {
    EXPECT_EQ(std::string::npos, std::string(kProfile[i].command).find(std::to_string(kKeyHours)))
        << "a derived value written into the table: " << kProfile[i].command;
  }
}

TEST(TheTableAsShipped, TheDutyCycleLineIsTheBandsOwnNumber) {
  // The second line is **generated** from the band: platformio.ini's [band_868].duty_cycle
  // reaches src/roles/repeater/firstboot/Profile.cpp as TWINOAK_BAND_DUTY_CYCLE and the file's
  // own static_assert holds that number against this band's row in src/common/bands/Bands.h.
  // What is left for a test is the line itself - that the number in the text is the row's, so
  // that a band whose regulation changes changes what a fresh unit asks for and nothing else
  // has to be edited.
  const int dc = twinoak::bands::dutyCycleOf(twinoak::bands::kBands, twinoak::bands::kBandCount,
                                             TWINOAK_BAND);
  ASSERT_LE(1, dc) << "band " << TWINOAK_BAND << " has no duty cycle in src/common/bands/Bands.h";
  ASSERT_GE(100, dc) << "upstream's `set dutycycle` takes 1-100 and would refuse " << dc;
  const std::string expected = "set dutycycle " + std::to_string(dc);
  EXPECT_EQ(expected, std::string(kProfile[1].command))
      << "the profile's duty-cycle line is not the band's own number: a fresh unit would start"
         " outside what the band allows, or at a value this repository does not say";
  // And the reply upstream gives that line is one the rule above accepts: `set dutycycle 10`
  // answers "OK - 10.0%" (CommonRadioPrefs::handleCommand, src/helpers/CommonRadioPrefs.cpp:88-100
  // at the pin 81dbae59), where every refusal of it is "ERROR: dutycycle must be 1-100".
  EXPECT_TRUE(replyAccepted("OK - 10.0%"));
  EXPECT_FALSE(replyAccepted("ERROR: dutycycle must be 1-100"));
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
    // A built line's text is the only one that may carry a placeholder, and it may carry it only
    // because what is sent is what `build` writes (which this file also builds, above).
    if (kProfile[i].build == nullptr) {
      EXPECT_EQ(std::string::npos, command.find('<')) << command;
      EXPECT_EQ(std::string::npos, command.find('>')) << command;
    }
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
