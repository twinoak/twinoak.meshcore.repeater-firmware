// src/common/version/Version.cpp: the string, its shape, and the defines platformio.ini feeds it.

#include <gtest/gtest.h>

#include <string.h>

#include "common/version/Version.h"

TEST(Version, FormatsTheThreeParts) {
  char buf[96];
  size_t n = twinoak::formatVersion(buf, sizeof(buf), "repeater", "1.2.3+abc1234",
                                    "repeater-v1.17.1",
                                    "d92964352441e53b93e8667b802e04f6e072b39e");
  EXPECT_STREQ("twinoak-repeater 1.2.3+abc1234 on MeshCore repeater-v1.17.1 (d9296435)", buf);
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
  size_t n = twinoak::formatVersion(buf, sizeof(buf), "repeater", "1.0.0", "repeater-v1.17.1",
                                    "d9296435");
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

TEST(Version, ThisBuildSaysThePinOfPlatformioIni) {
  // The role, tag and pin are platformio.ini's own, passed as defines to the native environment
  // exactly as to the board's; the version is the default because no release passed
  // TWINOAK_FW_VERSION here.
  EXPECT_STREQ("twinoak-repeater 0.0.0-dev on MeshCore repeater-v1.17.1 (d9296435)",
               twinoak::versionString());
  EXPECT_EQ(twinoak::versionString(), twinoak::versionString());  // one static buffer, built once
}

// The box link's `upstream` field (spec/LINK-PROTOCOL.md 5.1): the tag, a '+', eight digits of
// the pin. HELLO carries this, where `fw_version` carries the release's version alone.
TEST(Upstream, FormatsTheTagAndEightDigitsOfThePin) {
  char buf[48];
  size_t n = twinoak::formatUpstream(buf, sizeof(buf), "repeater-v1.17.1",
                                     "d92964352441e53b93e8667b802e04f6e072b39e");
  EXPECT_STREQ("repeater-v1.17.1+d9296435", buf);
  EXPECT_EQ(strlen(buf), n);

  twinoak::formatUpstream(buf, sizeof(buf), "t", "abc");
  EXPECT_STREQ("t+abc", buf);
}

TEST(Upstream, TruncatesAndSaysSo) {
  char buf[8];
  size_t n = twinoak::formatUpstream(buf, sizeof(buf), "repeater-v1.17.1", "d9296435");
  EXPECT_STREQ("repeate", buf);
  EXPECT_GT(n, sizeof(buf));
}

TEST(Upstream, ThisBuildSaysThePinOfPlatformioIni) {
  // The same two defines as the version string's, and the same reason a bump arrives red
  // (docs/upstream.md, "What a bump costs today").
  EXPECT_STREQ("repeater-v1.17.1+d9296435", twinoak::upstreamString());
  EXPECT_EQ(twinoak::upstreamString(), twinoak::upstreamString());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
