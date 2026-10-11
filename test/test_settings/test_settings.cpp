// src/app/Settings.{h,cpp}: our five keys as upstream's CLI sees them - `sensor list`,
// `sensor get <key>`, `sensor set <key> <value>` - against a fake host.
//
// What is checked, in the order tasks/phase-1/05-local-management.md asks for it: the order of
// `sensor list` is stable (it is what a bench reads and what upstream's paging counts in), an
// unknown key is not ours, a `set` of a read-only key is refused, and `walter.wake` takes 1 and
// nothing else. The last two tests are the arithmetic of upstream's own `sensor list`, which
// stops adding lines once it has written 134 bytes into a 160-byte reply: with these five keys
// in this order a fresh node prints all five, a node whose counters have grown wide prints four
// and `... next:4`, and neither ever fills the buffer.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "app/Settings.h"

namespace {

class FakeHost : public twinoak::SettingsHost {
 public:
  std::string version = "twinoak-repeater 0.0.0-dev on MeshCore repeater-v1.17.1 (d9296435)";
  uint16_t errors = 0;
  bool seen = false;
  uint32_t seen_ago = 0;
  std::string clock_state = "unset";
  bool pending = false;
  int wakes = 0;
  bool wake_accepted = true;

  const char* fwVersion() const override { return version.c_str(); }
  uint16_t linkErrors() const override { return errors; }
  bool walterEverSeen() const override { return seen; }
  uint32_t walterSeenSecondsAgo() const override { return seen_ago; }
  const char* clockStateName() const override { return clock_state.c_str(); }
  bool attentionPending() const override { return pending; }
  bool raiseWalterWake() override {
    wakes++;
    return wake_accepted;
  }
};

}  // namespace

TEST(TheTable, TheOrderIsStableAndIsWhatSensorListPrints) {
  FakeHost host;
  twinoak::Settings s(host);
  ASSERT_EQ(5, twinoak::Settings::count());

  const char* const want[] = {"fw.version", "walter.wake", "link.errors", "walter.seen",
                              "clock.state"};
  for (int i = 0; i < twinoak::Settings::count(); i++) {
    EXPECT_STREQ(want[i], twinoak::Settings::name(i)) << "index " << i;
  }
  EXPECT_EQ(nullptr, twinoak::Settings::name(-1));
  EXPECT_EQ(nullptr, twinoak::Settings::name(5));
  EXPECT_EQ(nullptr, s.value(-1));
  EXPECT_EQ(nullptr, s.value(5));
}

TEST(TheTable, EveryValueIsWhatTheHostSays) {
  FakeHost host;
  twinoak::Settings s(host);

  // A node that has just booted: nothing has been seen, the clock is unset, nothing is pending.
  EXPECT_STREQ(host.version.c_str(), s.value(0));
  EXPECT_STREQ("0", s.value(1)) << "walter.wake";
  EXPECT_STREQ("0", s.value(2)) << "link.errors";
  EXPECT_STREQ("never", s.value(3)) << "walter.seen, before the first valid request";
  EXPECT_STREQ("unset", s.value(4)) << "clock.state";

  // And one that has been running.
  host.errors = 65535;  // the counter saturates there
  host.seen = true;
  host.seen_ago = 4294967295u;
  host.clock_state = "restored";
  host.pending = true;
  EXPECT_STREQ("1", s.value(1)) << "walter.wake reads 1 while an attention is pending";
  EXPECT_STREQ("65535", s.value(2));
  EXPECT_STREQ("4294967295", s.value(3)) << "and the widest number it could hold still fits";
  EXPECT_STREQ("restored", s.value(4));
}

TEST(TheTable, FourOfTheFiveAreReadOnlyAndAnUnknownKeyIsNotOurs) {
  FakeHost host;
  twinoak::Settings s(host);

  const char* const read_only[] = {"fw.version", "link.errors", "walter.seen", "clock.state"};
  for (const char* key : read_only) {
    EXPECT_FALSE(s.set(key, "1")) << key;
    EXPECT_FALSE(s.set(key, "x")) << key;
  }
  EXPECT_EQ(0, host.wakes);

  // Not ours: upstream's CLI has already offered it to upstream's own manager and will answer
  // "can't find custom var" when nobody takes it.
  EXPECT_FALSE(s.set("gps", "1"));
  EXPECT_FALSE(s.set("walter.wak", "1"));
  EXPECT_FALSE(s.set("walter.wake1", "1"));
  EXPECT_FALSE(s.set("", "1"));
  EXPECT_FALSE(s.set(nullptr, "1"));
  EXPECT_FALSE(s.set("walter.wake", nullptr));
  EXPECT_EQ(0, host.wakes);
}

TEST(TheTable, WalterWakeTakesOneAndNothingElse) {
  FakeHost host;
  twinoak::Settings s(host);

  EXPECT_TRUE(s.set("walter.wake", "1"));
  EXPECT_EQ(1, host.wakes);

  // The machine's own rules - the hourly limit, a reason already pending - are
  // test/test_attention's. Whichever way it answers, the key was ours and the CLI says "ok":
  // `sensor get walter.wake` is where "did it actually raise one" is read.
  host.wake_accepted = false;
  EXPECT_TRUE(s.set("walter.wake", "1"));
  EXPECT_EQ(2, host.wakes);

  // And every other value is refused, without asking the machine.
  const char* const no[] = {"0", "", "2", "on", "true", "1 ", " 1", "01"};
  for (const char* value : no) {
    EXPECT_FALSE(s.set("walter.wake", value)) << "'" << value << "'";
  }
  EXPECT_EQ(2, host.wakes);
}

namespace {

// Upstream's `sensor list` (src/helpers/CommonCLI.cpp at the pin d9296435) writes "<n> vars\n"
// and then "<name>=<value>\n" per setting while it has written fewer than 134 bytes, and ends
// with "... next:<i>" when it stopped early. This is that loop over our table, so that what a
// console and a mesh admin actually see is a test and not an intention.
std::string sensorList(twinoak::Settings& s, int& printed) {
  std::string out = std::to_string(twinoak::Settings::count()) + " vars\n";
  int i = 0;
  const int end = twinoak::Settings::count();
  for (; i < end && out.size() < 134; i++) {
    out += std::string(twinoak::Settings::name(i)) + "=" + s.value(i) + "\n";
  }
  printed = i;
  if (i < end) {
    out += "... next:" + std::to_string(i);
  } else {
    out.resize(out.size() - 1);  // upstream removes the last newline
  }
  return out;
}

}  // namespace

TEST(SensorList, AllFiveAreOnTheFirstPageOfAFreshNodeAndTheReplyAlwaysFits) {
  FakeHost host;
  host.seen = true;
  host.seen_ago = 42;
  host.clock_state = "restored";
  twinoak::Settings s(host);

  int printed = 0;
  const std::string out = sensorList(s, printed);
  EXPECT_EQ(5, printed) << out;
  EXPECT_LT(out.size(), 160u) << out;
  EXPECT_NE(std::string::npos, out.find("walter.wake=0\n")) << out;
  EXPECT_NE(std::string::npos, out.find("walter.seen=42\n")) << out;
  // Last, and upstream drops the newline after the last line rather than leaving it.
  EXPECT_EQ(out.size() - 20, out.find("clock.state=restored")) << out;
}

TEST(SensorList, WithBothCountersAtTheirWidestOneKeyIsBehindAPageAndNothingOverruns) {
  FakeHost host;
  host.seen = true;
  // millis() wraps at 2^32 ms, so the most seconds `walter.seen` can really say is seven digits;
  // the value below is wider than the firmware can produce, which is the point of it.
  host.seen_ago = 4294967295u;
  host.errors = 65535;
  host.clock_state = "restored";
  host.pending = true;
  twinoak::Settings s(host);

  int printed = 0;
  const std::string out = sensorList(s, printed);
  EXPECT_EQ(4, printed) << out;
  EXPECT_NE(std::string::npos, out.find("... next:4")) << out;
  EXPECT_LT(out.size(), 160u) << "upstream's 160-byte reply buffer is never overrun: " << out;
  // The four that are on the first page whatever the counters say; `clock.state` is the one
  // behind the page, which `sensor list 4` then prints.
  EXPECT_NE(std::string::npos, out.find("fw.version=")) << out;
  EXPECT_NE(std::string::npos, out.find("walter.wake=1\n")) << out;
  EXPECT_NE(std::string::npos, out.find("link.errors=65535\n")) << out;
  EXPECT_NE(std::string::npos, out.find("walter.seen=4294967295\n")) << out;
  EXPECT_EQ(std::string::npos, out.find("clock.state")) << out;
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
