// src/roles/repeater/app/IdentityGuard.{h,cpp}: the decision a boot takes about the node's key,
// and the mode the node runs in when it refuses to invent one (pile 3.9, task 01).
//
// What is checked: the blank test; every row of the decision table - the two first boots (a
// blank region, a mounted volume with nothing of a node in it), the saved key, and the four
// refusals (a torn volume, a blank region that would not format, a key file that does not read,
// settings without a key); that a refusal never says "use" or "make"; the lost mode's console -
// `erase` through the one action and its reply, `reboot` through the other, every other line
// answered with the explanation, the explanation changing once the erase is done, and the reply
// always inside the window the session gives it; and the two constants the board reads the
// region with, held to the core's values.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "roles/repeater/app/IdentityGuard.h"

namespace {

using twinoak::identity::decide;
using twinoak::identity::Decision;
using twinoak::identity::explainLost;
using twinoak::identity::KeyFile;
using twinoak::identity::LostIdentityActions;
using twinoak::identity::LostIdentityCli;
using twinoak::identity::regionIsBlank;
using twinoak::identity::Verdict;
using twinoak::identity::Volume;

// ----------------------------------------------------------------------------- the region

TEST(TheRegion, IsWhereTheCoreKeepsIt) {
  // InternalFileSystem.cpp at the pinned core d541301: LFS_FLASH_ADDR 0xED000 on an nRF52840,
  // LFS_FLASH_TOTAL_SIZE 7 * FLASH_NRF52_PAGE_SIZE. docs/ARCHITECTURE.md section 9 says 0xED000
  // to 0xF4000, and the bootloader starts at 0xF4000 (boards/base/rak4630/README.md).
  EXPECT_EQ(0xED000u, twinoak::identity::kRegionStart);
  EXPECT_EQ(7u * 4096u, twinoak::identity::kRegionBytes);
  EXPECT_EQ(0xF4000u, twinoak::identity::kRegionStart + twinoak::identity::kRegionBytes);
  EXPECT_EQ(4096u, twinoak::identity::kPageBytes);
}

TEST(TheBlankTest, EveryByteErasedIsBlank) {
  std::vector<uint8_t> region(twinoak::identity::kRegionBytes, 0xFF);
  EXPECT_TRUE(regionIsBlank(region.data(), region.size()));
}

TEST(TheBlankTest, OneByteAnywhereIsNotBlank) {
  std::vector<uint8_t> region(twinoak::identity::kRegionBytes, 0xFF);
  region[0] = 0x00;
  EXPECT_FALSE(regionIsBlank(region.data(), region.size()));
  region[0] = 0xFF;
  region[region.size() - 1] = 0x7F;
  EXPECT_FALSE(regionIsBlank(region.data(), region.size()));
  // A formatted volume has a superblock in its first page.
  region[region.size() - 1] = 0xFF;
  memcpy(region.data() + 8, "littlefs", 8);
  EXPECT_FALSE(regionIsBlank(region.data(), region.size()));
}

TEST(TheBlankTest, NoBytesIsNotBlank) {
  // A null region is not evidence of anything, and must never read as "a first boot".
  EXPECT_FALSE(regionIsBlank(nullptr, 100));
  // An empty one is trivially all-0xFF; the board never asks with len 0, and the answer is kept
  // the mathematical one so that the function has one rule.
  uint8_t none = 0;
  EXPECT_TRUE(regionIsBlank(&none, 0));
}

// ----------------------------------------------------------------------------- the table

TEST(TheDecision, ABlankRegionIsAFirstBoot) {
  const Verdict v = decide(Volume::kBlank, KeyFile::kAbsent, false);
  EXPECT_EQ(Decision::kMakeNew, v.decision);
  EXPECT_NE(nullptr, strstr(v.why, "blank"));
  // Whatever the other two say: a blank region has no file and no settings in it.
  EXPECT_EQ(Decision::kMakeNew, decide(Volume::kBlank, KeyFile::kLoaded, true).decision);
}

TEST(TheDecision, ATornVolumeIsARefusal) {
  const Verdict v = decide(Volume::kUnmountable, KeyFile::kAbsent, false);
  EXPECT_EQ(Decision::kRefuse, v.decision);
  EXPECT_NE(nullptr, strstr(v.why, "would not mount"));
  EXPECT_NE(nullptr, strstr(v.why, "0xED000"));
  EXPECT_EQ(Decision::kRefuse, decide(Volume::kUnmountable, KeyFile::kLoaded, true).decision);
}

TEST(TheDecision, ABlankRegionThatWouldNotFormatIsARefusalToo) {
  const Verdict v = decide(Volume::kFormatFailed, KeyFile::kAbsent, false);
  EXPECT_EQ(Decision::kRefuse, v.decision);
  EXPECT_NE(nullptr, strstr(v.why, "could not be formatted"));
}

TEST(TheDecision, TheSavedKeyIsUsed) {
  const Verdict v = decide(Volume::kMounted, KeyFile::kLoaded, true);
  EXPECT_EQ(Decision::kUseSaved, v.decision);
  // With or without settings: the key is the node (spec/SYSTEM.md S-1); settings that are gone
  // are the first boot's business, as before.
  EXPECT_EQ(Decision::kUseSaved, decide(Volume::kMounted, KeyFile::kLoaded, false).decision);
}

TEST(TheDecision, NoKeyAndNoSettingsIsAFirstBoot) {
  // Upstream's `erase` formats the volume and leaves nothing; so does a format by hand. The next
  // boot is a first boot, exactly as it has always been.
  const Verdict v = decide(Volume::kMounted, KeyFile::kAbsent, false);
  EXPECT_EQ(Decision::kMakeNew, v.decision);
  EXPECT_NE(nullptr, strstr(v.why, "first boot"));
}

TEST(TheDecision, SettingsWithoutAKeyIsARefusal) {
  // The one case upstream turns into a new node wearing the old node's name and password.
  const Verdict v = decide(Volume::kMounted, KeyFile::kAbsent, true);
  EXPECT_EQ(Decision::kRefuse, v.decision);
  EXPECT_NE(nullptr, strstr(v.why, "settings are there"));
  EXPECT_NE(nullptr, strstr(v.why, "/_main.id"));
}

TEST(TheDecision, AKeyFileThatDoesNotReadIsARefusal) {
  for (bool prefs : {false, true}) {
    const Verdict v = decide(Volume::kMounted, KeyFile::kUnreadable, prefs);
    EXPECT_EQ(Decision::kRefuse, v.decision) << "prefs " << prefs;
    EXPECT_NE(nullptr, strstr(v.why, "does not read as a key"));
  }
}

TEST(TheDecision, EveryVerdictHasAReason) {
  const Volume volumes[] = {Volume::kMounted, Volume::kBlank, Volume::kUnmountable,
                            Volume::kFormatFailed};
  const KeyFile keys[] = {KeyFile::kLoaded, KeyFile::kAbsent, KeyFile::kUnreadable};
  for (Volume vol : volumes) {
    for (KeyFile key : keys) {
      for (bool prefs : {false, true}) {
        const Verdict v = decide(vol, key, prefs);
        ASSERT_NE(nullptr, v.why);
        EXPECT_GT(strlen(v.why), 8u);
      }
    }
  }
}

TEST(TheDecision, AKeyIsMadeOnlyWhereNoNodeWas) {
  // The rule of the file, as a property over the whole table: kMakeNew only for the blank region
  // and for a mounted volume with neither a key file nor settings.
  const Volume volumes[] = {Volume::kMounted, Volume::kBlank, Volume::kUnmountable,
                            Volume::kFormatFailed};
  const KeyFile keys[] = {KeyFile::kLoaded, KeyFile::kAbsent, KeyFile::kUnreadable};
  for (Volume vol : volumes) {
    for (KeyFile key : keys) {
      for (bool prefs : {false, true}) {
        const bool nothing_was_here =
            vol == Volume::kBlank || (vol == Volume::kMounted && key == KeyFile::kAbsent && !prefs);
        const Decision d = decide(vol, key, prefs).decision;
        EXPECT_EQ(nothing_was_here, d == Decision::kMakeNew)
            << "volume " << int(vol) << " key " << int(key) << " prefs " << prefs;
      }
    }
  }
}

// ----------------------------------------------------------------------------- the lost mode

class FakeActions : public LostIdentityActions {
 public:
  int erases = 0;
  int restarts = 0;
  bool erase_ok = true;

  bool eraseFileSystem() override {
    erases++;
    return erase_ok;
  }
  void restart() override { restarts++; }
};

// The verdict a handler is built over must outlive it (IdentityGuard.h): one for the whole suite.
const Verdict kTorn = decide(Volume::kUnmountable, KeyFile::kAbsent, false);
const Verdict& torn() { return kTorn; }

// The session's window: 160 bytes for the reply, as upstream's console gives its interpreter
// (src/roles/repeater/main.cpp, src/common/link/session/Session.h).
constexpr size_t kWindow = 160;

std::string run(LostIdentityCli& cli, const char* text, size_t cap = kWindow) {
  char line[160];
  snprintf(line, sizeof(line), "%s", text);
  std::vector<char> reply(cap + 64, 'X');  // a guard behind the window, as the session keeps one
  const size_t n = cli.runCommand(line, reply.data(), cap);
  EXPECT_EQ(strlen(reply.data()), n);
  EXPECT_LT(n, cap);
  for (size_t i = cap; i < reply.size(); i++) EXPECT_EQ('X', reply[i]) << "wrote past the window";
  return std::string(reply.data(), n);
}

TEST(TheLostMode, TheExplanationNamesTheConditionAndTheWayOut) {
  char out[256];
  const size_t n = explainLost(torn(), false, out, sizeof(out));
  EXPECT_EQ(strlen(out), n);
  const std::string text(out);
  EXPECT_EQ(0u, text.find("IDENTITY LOST"));
  EXPECT_NE(std::string::npos, text.find("would not mount"));
  EXPECT_NE(std::string::npos, text.find("No key was made"));
  EXPECT_NE(std::string::npos, text.find("'erase'"));
  EXPECT_NE(std::string::npos, text.find("'reboot'"));
  EXPECT_NE(std::string::npos, text.find("cannot be recovered"));
  EXPECT_NE(std::string::npos, text.find("docs/flashing.md 3a"));
}

TEST(TheLostMode, TheExplanationIsCutToTheWindowAndTerminated) {
  char out[40];
  memset(out, 'X', sizeof(out));
  const size_t n = explainLost(torn(), false, out, sizeof(out));
  EXPECT_EQ(sizeof(out) - 1, n);
  EXPECT_EQ(0, out[sizeof(out) - 1]);
  EXPECT_EQ(0u, std::string(out).find("IDENTITY LOST"));
  EXPECT_EQ(0u, explainLost(torn(), false, nullptr, 10));
  EXPECT_EQ(0u, explainLost(torn(), false, out, 0));
}

TEST(TheLostMode, EveryOtherLineIsAnsweredWithTheExplanation) {
  FakeActions actions;
  LostIdentityCli cli(actions, torn());
  for (const char* line : {"ver", "get public.key", "sensor list", "password hunter2", "",
                           "advert", "erase now", "reboot 2", "clkreboot"}) {
    const std::string reply = run(cli, line);
    EXPECT_EQ(0u, reply.find("IDENTITY LOST")) << "line '" << line << "' answered: " << reply;
  }
  EXPECT_EQ(0, actions.erases);
  EXPECT_EQ(0, actions.restarts);
}

TEST(TheLostMode, EraseRunsTheOneActionAndSaysWhatIsLeft) {
  FakeActions actions;
  LostIdentityCli cli(actions, torn());
  EXPECT_FALSE(cli.erased());
  const std::string reply = run(cli, "erase");
  EXPECT_EQ(1, actions.erases);
  EXPECT_EQ(0, actions.restarts);
  EXPECT_EQ(0u, reply.find("File system erase: OK"));  // upstream's own words, then ours
  EXPECT_NE(std::string::npos, reply.find("'reboot'"));
  EXPECT_TRUE(cli.erased());
  // And the explanation now says the one step that is left.
  const std::string after = run(cli, "ver");
  EXPECT_EQ(0u, after.find("IDENTITY LOST"));
  EXPECT_NE(std::string::npos, after.find("has been erased"));
  EXPECT_NE(std::string::npos, after.find("'reboot'"));
  EXPECT_EQ(std::string::npos, after.find("'erase'"));
}

TEST(TheLostMode, AnEraseThatFailsSaysSoAndChangesNothing) {
  FakeActions actions;
  actions.erase_ok = false;
  LostIdentityCli cli(actions, torn());
  const std::string reply = run(cli, "erase");
  EXPECT_EQ("File system erase: Err", reply);
  EXPECT_FALSE(cli.erased());
  EXPECT_NE(std::string::npos, run(cli, "ver").find("'erase'"));
}

TEST(TheLostMode, SpacesAroundTheWordAreForgiven) {
  // A Walter's `rak cli  erase ` or a terminal that sends a trailing CR.
  FakeActions actions;
  LostIdentityCli cli(actions, torn());
  run(cli, "  erase ");
  run(cli, "erase\r");
  EXPECT_EQ(2, actions.erases);
  run(cli, "\treboot");
  EXPECT_EQ(1, actions.restarts);
}

TEST(TheLostMode, RebootRunsTheOtherAction) {
  FakeActions actions;
  LostIdentityCli cli(actions, torn());
  const std::string reply = run(cli, "reboot");
  EXPECT_EQ(1, actions.restarts);
  EXPECT_EQ(0, actions.erases);
  EXPECT_EQ("rebooting", reply);
}

TEST(TheLostMode, TheReplyNeverLeavesTheWindow) {
  FakeActions actions;
  LostIdentityCli cli(actions, torn());
  for (size_t cap : {size_t(1), size_t(2), size_t(16), size_t(64), size_t(160)}) {
    run(cli, "anything", cap);
    run(cli, "erase", cap);
  }
  char reply[8] = "stale";
  EXPECT_EQ(0u, cli.runCommand(nullptr, reply, 0));
  EXPECT_EQ(0u, cli.runCommand(nullptr, nullptr, 8));
  // A null line is "no command" and gets the explanation, not a crash.
  EXPECT_LT(0u, cli.runCommand(nullptr, reply, sizeof(reply)));
  EXPECT_EQ(0, reply[sizeof(reply) - 1]);
}

TEST(TheLostMode, TheFaultCodeIsTheOneTheDocsList) {
  // docs/link.md's attention table: reason 6, detail 1 - the identity is lost.
  EXPECT_EQ(1u, twinoak::identity::kFaultCodeIdentityLost);
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
