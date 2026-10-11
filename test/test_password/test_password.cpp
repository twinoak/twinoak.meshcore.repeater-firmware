// src/app/AdminPassword.cpp: the first-boot password's length, alphabet and randomness, and the
// rule that decides when one is drawn (tasks/phase-1/01, spec/SYSTEM.md S-13).

#include <gtest/gtest.h>

#include <string.h>

#include <string>
#include <vector>

#include "app/AdminPassword.h"

namespace {

// Replays the bytes it was given, then zeros - so a test can steer the generator exactly.
class ReplaySource : public twinoak::RandomSource {
 public:
  explicit ReplaySource(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
  void fill(uint8_t* dest, size_t len) override {
    for (size_t i = 0; i < len; i++) dest[i] = pos_ < bytes_.size() ? bytes_[pos_++] : 0;
  }

 private:
  std::vector<uint8_t> bytes_;
  size_t pos_ = 0;
};

// A small deterministic generator (xorshift32) seeded by the test: different seeds, different
// streams, and a stream that visits every byte value.
class SeededSource : public twinoak::RandomSource {
 public:
  explicit SeededSource(uint32_t seed) : state_(seed ? seed : 1) {}
  void fill(uint8_t* dest, size_t len) override {
    for (size_t i = 0; i < len; i++) {
      state_ ^= state_ << 13;
      state_ ^= state_ >> 17;
      state_ ^= state_ << 5;
      dest[i] = static_cast<uint8_t>(state_);
    }
  }

 private:
  uint32_t state_;
};

bool inAlphabet(char c) { return strchr(twinoak::kPasswordAlphabet, c) != nullptr && c != 0; }

}  // namespace

TEST(Password, TheAlphabetIsSixtyTwoDistinctSafeCharacters) {
  const std::string alphabet(twinoak::kPasswordAlphabet);
  EXPECT_EQ(twinoak::kPasswordAlphabetSize, alphabet.size());
  for (size_t i = 0; i < alphabet.size(); i++) {
    EXPECT_EQ(i, alphabet.find(alphabet[i])) << "duplicate: " << alphabet[i];
    EXPECT_TRUE(isalnum(static_cast<unsigned char>(alphabet[i]))) << "not alphanumeric: " << alphabet[i];
  }
}

TEST(Password, FillsTheBufferButForTheNul) {
  // NodePrefs::password is char[16] at the pin: fifteen characters and the terminator.
  char buf[16];
  memset(buf, 'x', sizeof(buf));
  SeededSource rng(42);
  EXPECT_EQ(15u, twinoak::generatePassword(buf, sizeof(buf), rng));
  EXPECT_EQ(15u, strlen(buf));
  EXPECT_EQ(0, buf[15]);
}

TEST(Password, EveryCharacterIsFromTheAlphabet) {
  char buf[64];
  for (uint32_t seed = 1; seed <= 50; seed++) {
    SeededSource rng(seed);
    twinoak::generatePassword(buf, sizeof(buf), rng);
    for (size_t i = 0; buf[i]; i++) EXPECT_TRUE(inAlphabet(buf[i])) << "seed " << seed << " char " << i;
  }
}

TEST(Password, TwoDrawsDiffer) {
  char a[16], b[16];
  SeededSource first(1), second(2);
  twinoak::generatePassword(a, sizeof(a), first);
  twinoak::generatePassword(b, sizeof(b), second);
  EXPECT_STRNE(a, b);
  // And one source drawn twice gives two different passwords too.
  SeededSource again(3);
  twinoak::generatePassword(a, sizeof(a), again);
  twinoak::generatePassword(b, sizeof(b), again);
  EXPECT_STRNE(a, b);
}

TEST(Password, BytesAtOrAboveTwoFortyEightAreThrownAway) {
  // 255, 250 and 248 are rejected; 0 -> 'A', 247 -> 247 % 62 = 61 -> '9', 61 -> '9', 62 -> 'A',
  // 124 -> 'A' (the reduction is modulo 62, and every byte below 248 is used).
  ReplaySource rng({255, 250, 248, 0, 247, 61, 62, 124});
  char buf[6];
  EXPECT_EQ(5u, twinoak::generatePassword(buf, sizeof(buf), rng));
  EXPECT_STREQ("A99AA", buf);
}

TEST(Password, KeepsAskingUntilItHasEnough) {
  // Thirty rejected bytes in a row, then good ones: the generator draws again rather than giving
  // up or writing short.
  std::vector<uint8_t> bytes(30, 255);
  for (int i = 0; i < 20; i++) bytes.push_back(static_cast<uint8_t>(i));
  ReplaySource rng(bytes);
  char buf[16];
  EXPECT_EQ(15u, twinoak::generatePassword(buf, sizeof(buf), rng));
  EXPECT_STREQ("ABCDEFGHIJKLMNO", buf);
}

TEST(Password, AnEmptyOrOneByteBufferGetsNothingButANul) {
  SeededSource rng(7);
  char one[1] = {'x'};
  EXPECT_EQ(0u, twinoak::generatePassword(one, sizeof(one), rng));
  EXPECT_EQ(0, one[0]);
  char untouched[1] = {'x'};
  EXPECT_EQ(0u, twinoak::generatePassword(untouched, 0, rng));
  EXPECT_EQ('x', untouched[0]);
}

TEST(Password, IsDrawnWhenNoPrefsWereSavedOrTheBuildDefaultSurvived) {
  EXPECT_TRUE(twinoak::needsFreshPassword(false, "whatever", "not-a-password"));
  EXPECT_TRUE(twinoak::needsFreshPassword(false, "not-a-password", "not-a-password"));
  EXPECT_TRUE(twinoak::needsFreshPassword(true, "not-a-password", "not-a-password"));
  EXPECT_FALSE(twinoak::needsFreshPassword(true, "Kq8ZmP2vXr4tYw9", "not-a-password"));
  EXPECT_FALSE(twinoak::needsFreshPassword(true, "", "not-a-password"));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
