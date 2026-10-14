// src/common/version/Version.cpp: the string, its shape, and the defines platformio.ini feeds it.
//
// No test in this file names the MeshCore release this firmware is pinned to. A bump moves two
// lines of platformio.ini ([meshcore].pin and .tag) and nothing else, and the point of tier one
// (docs/upstream.md) is that a bump which changes nothing we rely on arrives green: so the two
// ThisBuildSaysThePinOfPlatformioIni tests build what they expect out of the same two defines the
// code reads, and assert that the defines arrived at all - which is what they were for. The shape
// of the string is pinned with a made-up tag and pin, kTag and kPin below, which no bump touches.

#include <gtest/gtest.h>

#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include <string>

#include "common/version/Version.h"

namespace {

// A tag and a pin that are plainly not a release: the shape is what these hold, not the version.
constexpr const char* kTag = "repeater-vX.Y.Z";
constexpr const char* kPin = "0123abcdef0123456789abcdef0123456789abcd";  // 40 hex, as a commit is
constexpr const char* kPin8 = "0123abcd";                                 // and what is shown

// A pin is a git commit: forty hex digits. A build whose -D TWINOAK_MESHCORE_PIN did not arrive,
// or arrived as something else, fails this.
bool isCommit(const char* s) {
  size_t n = 0;
  for (; s[n] != '\0'; ++n) {
    if (!isxdigit(static_cast<unsigned char>(s[n]))) return false;
  }
  return n == 40;
}

std::string firstEight(const char* s) { return std::string(s).substr(0, 8); }

}  // namespace

TEST(Version, FormatsTheThreeParts) {
  char buf[96];
  size_t n = twinoak::formatVersion(buf, sizeof(buf), "repeater", "1.2.3+abc1234", kTag, kPin);
  EXPECT_STREQ("twinoak-repeater 1.2.3+abc1234 on MeshCore repeater-vX.Y.Z (0123abcd)", buf);
  EXPECT_EQ(strlen(buf), n);
}

TEST(Version, ShowsAtMostEightDigitsOfThePin) {
  char buf[96];
  twinoak::formatVersion(buf, sizeof(buf), "repeater", "0.0.0-dev", "t", "0123456789abcdef");
  EXPECT_STREQ("twinoak-repeater 0.0.0-dev on MeshCore t (01234567)", buf);
  twinoak::formatVersion(buf, sizeof(buf), "repeater", "0.0.0-dev", "t", "abc");
  EXPECT_STREQ("twinoak-repeater 0.0.0-dev on MeshCore t (abc)", buf);
}

TEST(Version, TruncatesAndSaysSo) {
  char buf[20];
  size_t n = twinoak::formatVersion(buf, sizeof(buf), "repeater", "1.0.0", kTag, kPin8);
  EXPECT_STREQ("twinoak-repeater 1.", buf);  // NUL-terminated inside the buffer
  EXPECT_GT(n, sizeof(buf));                 // and the caller can tell
}

TEST(Version, SaysTheRoleItWasBuiltFor) {
  // The role is a name in one place (platformio.ini's [img_*] section) and this string is one
  // of its consumers: a scanner image says "twinoak-scanner".
  char buf[96];
  twinoak::formatVersion(buf, sizeof(buf), "scanner", "1.0.0", "t", "abc");
  EXPECT_STREQ("twinoak-scanner 1.0.0 on MeshCore t (abc)", buf);
}

// The shape both strings have, held with literals no bump can change. versionString() and
// upstreamString() are formatVersion() and formatUpstream() over the four defines
// (src/common/version/Version.cpp), so those two functions are the seam the build's inputs come
// in through, and feeding them a made-up tag and pin pins the shape without pinning a release:
// "twinoak-<role> <version> on MeshCore <tag> (<8 hex>)" and "<tag>+<8 hex>", the same eight
// digits in both.
TEST(Version, TheShapeDoesNotDependOnTheRelease) {
  char version[96];
  twinoak::formatVersion(version, sizeof(version), "repeater", "1.0.0", kTag, kPin);
  EXPECT_STREQ("twinoak-repeater 1.0.0 on MeshCore repeater-vX.Y.Z (0123abcd)", version);

  char upstream[48];
  twinoak::formatUpstream(upstream, sizeof(upstream), kTag, kPin);
  EXPECT_STREQ("repeater-vX.Y.Z+0123abcd", upstream);

  // The parenthesised digits of the one are the suffix of the other.
  EXPECT_NE(nullptr, strstr(version, (std::string("(") + kPin8 + ")").c_str()));
  EXPECT_NE(nullptr, strstr(upstream, (std::string("+") + kPin8).c_str()));
}

TEST(Version, ThisBuildSaysThePinOfPlatformioIni) {
  // The role, tag and pin are platformio.ini's own, passed as defines to the native environment
  // exactly as to the board's; the version is the default because no release passed
  // TWINOAK_FW_VERSION here. What is asserted is that those defines reach the string - not which
  // release they name, which is why a MeshCore bump does not make this red.
  ASSERT_GT(strlen(TWINOAK_MESHCORE_TAG), 0u) << "TWINOAK_MESHCORE_TAG did not reach this build";
  ASSERT_TRUE(isCommit(TWINOAK_MESHCORE_PIN))
      << "TWINOAK_MESHCORE_PIN is not a forty-digit commit: " << TWINOAK_MESHCORE_PIN;

  const std::string expected = std::string("twinoak-repeater 0.0.0-dev on MeshCore ") +
                               TWINOAK_MESHCORE_TAG + " (" + firstEight(TWINOAK_MESHCORE_PIN) + ")";
  EXPECT_EQ(expected, twinoak::versionString());
  EXPECT_EQ(twinoak::versionString(), twinoak::versionString());  // one static buffer, built once
}

// The box link's `upstream` field (spec/LINK-PROTOCOL.md 5.1): the tag, a '+', eight digits of
// the pin. HELLO carries this, where `fw_version` carries the release's version alone.
TEST(Upstream, FormatsTheTagAndEightDigitsOfThePin) {
  char buf[48];
  size_t n = twinoak::formatUpstream(buf, sizeof(buf), kTag, kPin);
  EXPECT_STREQ("repeater-vX.Y.Z+0123abcd", buf);
  EXPECT_EQ(strlen(buf), n);

  twinoak::formatUpstream(buf, sizeof(buf), "t", "abc");
  EXPECT_STREQ("t+abc", buf);
}

TEST(Upstream, TruncatesAndSaysSo) {
  char buf[8];
  size_t n = twinoak::formatUpstream(buf, sizeof(buf), kTag, kPin8);
  EXPECT_STREQ("repeate", buf);
  EXPECT_GT(n, sizeof(buf));
}

TEST(Upstream, ThisBuildSaysThePinOfPlatformioIni) {
  // The same two defines as the version string's, and the same reason neither test names the
  // release: a bump that changes nothing we rely on is green (docs/upstream.md, "What a bump
  // costs today").
  ASSERT_GT(strlen(TWINOAK_MESHCORE_TAG), 0u) << "TWINOAK_MESHCORE_TAG did not reach this build";
  ASSERT_TRUE(isCommit(TWINOAK_MESHCORE_PIN))
      << "TWINOAK_MESHCORE_PIN is not a forty-digit commit: " << TWINOAK_MESHCORE_PIN;

  const std::string expected =
      std::string(TWINOAK_MESHCORE_TAG) + "+" + firstEight(TWINOAK_MESHCORE_PIN);
  EXPECT_EQ(expected, twinoak::upstreamString());
  EXPECT_EQ(twinoak::upstreamString(), twinoak::upstreamString());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
