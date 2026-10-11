// src/version.cpp: the string, its shape, and the defines platformio.ini feeds it.

#include <gtest/gtest.h>

#include <string.h>

#include "version.h"

TEST(Version, FormatsTheThreeParts) {
  char buf[96];
  size_t n = twinoak::formatVersion(buf, sizeof(buf), "1.2.3+abc1234", "repeater-v1.17.1",
                                    "d92964352441e53b93e8667b802e04f6e072b39e");
  EXPECT_STREQ("twinoak-repeater 1.2.3+abc1234 on MeshCore repeater-v1.17.1 (d9296435)", buf);
  EXPECT_EQ(strlen(buf), n);
}

TEST(Version, ShowsAtMostEightDigitsOfThePin) {
  char buf[96];
  twinoak::formatVersion(buf, sizeof(buf), "0.0.0-dev", "t", "0123456789abcdef");
  EXPECT_STREQ("twinoak-repeater 0.0.0-dev on MeshCore t (01234567)", buf);
  twinoak::formatVersion(buf, sizeof(buf), "0.0.0-dev", "t", "abc");
  EXPECT_STREQ("twinoak-repeater 0.0.0-dev on MeshCore t (abc)", buf);
}

TEST(Version, TruncatesAndSaysSo) {
  char buf[20];
  size_t n = twinoak::formatVersion(buf, sizeof(buf), "1.0.0", "repeater-v1.17.1", "d9296435");
  EXPECT_STREQ("twinoak-repeater 1.", buf);  // NUL-terminated inside the buffer
  EXPECT_GT(n, sizeof(buf));                 // and the caller can tell
}

TEST(Version, ThisBuildSaysThePinOfPlatformioIni) {
  // The tag and pin are platformio.ini's [meshcore] section, passed as defines to the native
  // environment exactly as to the board's; the version is the default because no release
  // passed TWINOAK_FW_VERSION here.
  EXPECT_STREQ("twinoak-repeater 0.0.0-dev on MeshCore repeater-v1.17.1 (d9296435)",
               twinoak::versionString());
  EXPECT_EQ(twinoak::versionString(), twinoak::versionString());  // one static buffer, built once
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
