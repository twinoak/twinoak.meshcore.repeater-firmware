// src/app/Settings.{h,cpp}: our nine keys as upstream's CLI sees them - `sensor list`,
// `sensor get <key>`, `sensor set <key> <value>` - against a fake host.
//
// What is checked: the order of `sensor list` is stable (it is what a bench reads and what
// upstream's paging counts in, and every key a later task adds is **appended** so that nothing
// task 05 documented moved), an unknown key is not ours, a `set` of a read-only key is refused,
// `walter.wake` takes 1 and nothing else, the two battery thresholds take a decimal number of
// millivolts and nothing else, `obs.wake` takes 0 and 1 both ways, and `client.refused` is a
// counter that takes nothing at all.
//
// The last tests are the arithmetic of upstream's own `sensor list`, which stops adding lines
// once it has written 134 bytes into a 160-byte reply: with these nine keys in this order a
// fresh node prints task 05's five and `... next:5`, a node whose counters have grown wide
// prints four and `... next:4`, `sensor list 5` prints the four that are left - and
// the one case that would be longer than the window (a release version string together with a
// `restored` clock) is measured here rather than found on a bench.

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
  uint16_t low_mv = 3500;
  uint16_t crit_mv = 3300;
  // The board refuses a pair the thresholds would not accept (src/power/BatteryWatch.h); here
  // the window is the same one, written out so that the table's own job - parsing - is what this
  // suite is testing.
  bool accept_thresholds = true;
  int low_sets = 0;
  int crit_sets = 0;
  bool obs_wake = false;
  int obs_sets = 0;
  uint32_t refused = 0;

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
  uint16_t batteryLowMv() const override { return low_mv; }
  uint16_t batteryCritMv() const override { return crit_mv; }
  bool setBatteryLowMv(uint16_t mv) override {
    low_sets++;
    if (!accept_thresholds) return false;
    low_mv = mv;
    return true;
  }
  bool setBatteryCritMv(uint16_t mv) override {
    crit_sets++;
    if (!accept_thresholds) return false;
    crit_mv = mv;
    return true;
  }
  bool obsWake() const override { return obs_wake; }
  void setObsWake(bool on) override {
    obs_sets++;
    obs_wake = on;
  }
  uint32_t clientSendsRefused() const override { return refused; }
};

}  // namespace

TEST(TheTable, TheOrderIsStableAndIsWhatSensorListPrints) {
  FakeHost host;
  twinoak::Settings s(host);
  ASSERT_EQ(9, twinoak::Settings::count());

  // Task 05's five, in task 05's order, then task 06's two, then phase 2's task 03's and 04's.
  const char* const want[] = {"fw.version",    "walter.wake",   "link.errors",
                              "walter.seen",   "clock.state",   "power.low_mv",
                              "power.crit_mv", "obs.wake",      "client.refused"};
  for (int i = 0; i < twinoak::Settings::count(); i++) {
    EXPECT_STREQ(want[i], twinoak::Settings::name(i)) << "index " << i;
  }
  EXPECT_EQ(nullptr, twinoak::Settings::name(-1));
  EXPECT_EQ(nullptr, twinoak::Settings::name(9));
  EXPECT_EQ(nullptr, s.value(-1));
  EXPECT_EQ(nullptr, s.value(9));
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
  EXPECT_STREQ("3500", s.value(5)) << "power.low_mv, the default";
  EXPECT_STREQ("3300", s.value(6)) << "power.crit_mv, the default";
  EXPECT_STREQ("0", s.value(7)) << "obs.wake, off at boot";
  EXPECT_STREQ("0", s.value(8)) << "client.refused, on a node whose Walter paces itself";

  // And one that has been running, with thresholds somebody set.
  host.errors = 65535;  // the counter saturates there
  host.seen = true;
  host.seen_ago = 4294967295u;
  host.clock_state = "restored";
  host.pending = true;
  host.low_mv = 3600;
  host.crit_mv = 3350;
  EXPECT_STREQ("1", s.value(1)) << "walter.wake reads 1 while an attention is pending";
  EXPECT_STREQ("65535", s.value(2));
  EXPECT_STREQ("4294967295", s.value(3)) << "and the widest number it could hold still fits";
  EXPECT_STREQ("restored", s.value(4));
  EXPECT_STREQ("3600", s.value(5));
  EXPECT_STREQ("3350", s.value(6));
  host.obs_wake = true;
  EXPECT_STREQ("1", s.value(7));
  host.refused = 4294967295u;
  EXPECT_STREQ("4294967295", s.value(8)) << "and the widest the counter can say still fits";
}

TEST(TheTable, FiveOfTheNineAreReadOnlyAndAnUnknownKeyIsNotOurs) {
  FakeHost host;
  twinoak::Settings s(host);

  // `client.refused` is read-only on purpose: the ceiling it counts is a build constant, because
  // a limit the Walter could raise would be no backstop against the Walter
  // (src/meshclient/SendCeiling.h).
  const char* const read_only[] = {"fw.version", "link.errors", "walter.seen", "clock.state",
                                   "client.refused"};
  for (const char* key : read_only) {
    EXPECT_FALSE(s.set(key, "1")) << key;
    EXPECT_FALSE(s.set(key, "x")) << key;
  }
  EXPECT_EQ(0, host.wakes);
  EXPECT_EQ(0, host.low_sets);
  EXPECT_EQ(0, host.crit_sets);
  EXPECT_EQ(0, host.obs_sets);

  // Not ours: upstream's CLI has already offered it to upstream's own manager and will answer
  // "can't find custom var" when nobody takes it.
  EXPECT_FALSE(s.set("gps", "1"));
  EXPECT_FALSE(s.set("walter.wak", "1"));
  EXPECT_FALSE(s.set("walter.wake1", "1"));
  EXPECT_FALSE(s.set("power.low", "3500"));
  EXPECT_FALSE(s.set("power.low_mv2", "3500"));
  EXPECT_FALSE(s.set("", "1"));
  EXPECT_FALSE(s.set(nullptr, "1"));
  EXPECT_FALSE(s.set("walter.wake", nullptr));
  EXPECT_FALSE(s.set("power.low_mv", nullptr));
  EXPECT_FALSE(s.set("obs.wak", "1"));
  EXPECT_FALSE(s.set("obs.wake1", "1"));
  EXPECT_FALSE(s.set("obs.wake", nullptr));
  EXPECT_FALSE(s.set("client.refuse", "0"));
  EXPECT_FALSE(s.set("client.refused", "0"));
  EXPECT_FALSE(s.set("client.refused", "1"));
  EXPECT_EQ(0, host.wakes);
  EXPECT_EQ(0, host.low_sets);
  EXPECT_EQ(0, host.obs_sets);
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

TEST(TheTable, TheTwoThresholdsTakeADecimalNumberOfMillivolts) {
  FakeHost host;
  twinoak::Settings s(host);

  EXPECT_TRUE(s.set("power.low_mv", "3600"));
  EXPECT_EQ(3600, host.low_mv);
  EXPECT_EQ(1, host.low_sets);
  EXPECT_TRUE(s.set("power.crit_mv", "3250"));
  EXPECT_EQ(3250, host.crit_mv);
  EXPECT_EQ(1, host.crit_sets);

  // Nothing but digits, and nothing a u16 cannot hold. None of these reaches the host.
  const char* const no[] = {"",   " 3500", "3500 ", "3.5", "+3500", "-3500",
                            "3500mv", "0x3500", "65536", "99999999999", "abc"};
  for (const char* value : no) {
    EXPECT_FALSE(s.set("power.low_mv", value)) << "'" << value << "'";
    EXPECT_FALSE(s.set("power.crit_mv", value)) << "'" << value << "'";
  }
  EXPECT_EQ(1, host.low_sets) << "a value the table cannot parse is never offered to the board";
  EXPECT_EQ(1, host.crit_sets);

  // A pair the board refuses is refused here too - upstream's CLI then says "can't find custom
  // var", which is its only word for "no" (src/app/Settings.h).
  host.accept_thresholds = false;
  EXPECT_FALSE(s.set("power.low_mv", "4000"));
  EXPECT_EQ(2, host.low_sets);
  EXPECT_EQ(3600, host.low_mv) << "and nothing changed";

  // "0" parses, and is the board's to refuse rather than the table's: the table knows what a
  // number is and the thresholds know what a battery is.
  host.accept_thresholds = true;
  EXPECT_TRUE(s.set("power.low_mv", "0"));
  EXPECT_EQ(3, host.low_sets);
}

TEST(TheTable, ObsWakeTakesZeroAndOneAndGoesBothWays) {
  // Section 8's reason 5 is off by default, because a unit on a busy mesh must not wake its
  // Walter every hour for adverts alone (src/meshclient/LinkObservations.h). Unlike
  // `walter.wake`, which is a request, this one is a setting: it can be turned off again.
  FakeHost host;
  twinoak::Settings s(host);

  EXPECT_STREQ("0", s.value(7));
  EXPECT_TRUE(s.set("obs.wake", "1"));
  EXPECT_TRUE(host.obs_wake);
  EXPECT_STREQ("1", s.value(7));
  EXPECT_TRUE(s.set("obs.wake", "0"));
  EXPECT_FALSE(host.obs_wake);
  EXPECT_STREQ("0", s.value(7));
  EXPECT_EQ(2, host.obs_sets);

  const char* const no[] = {"", "2", "on", "off", "true", "false", "1 ", " 0", "01"};
  for (const char* value : no) {
    EXPECT_FALSE(s.set("obs.wake", value)) << "'" << value << "'";
  }
  EXPECT_EQ(2, host.obs_sets) << "a value the table cannot read never reaches the board";
}

namespace {

// Upstream's `sensor list` (src/helpers/CommonCLI.cpp at the pin d9296435) writes "<n> vars\n"
// and then "<name>=<value>\n" per setting while it has written fewer than 134 bytes, and ends
// with "... next:<i>" when it stopped early. This is that loop over our table, so that what a
// console and a mesh admin actually see is a test and not an intention.
std::string sensorList(twinoak::Settings& s, int start, int& printed) {
  std::string out = std::to_string(twinoak::Settings::count()) + " vars\n";
  int i = start;
  const int end = twinoak::Settings::count();
  for (; i < end && out.size() < 134; i++) {
    out += std::string(twinoak::Settings::name(i)) + "=" + s.value(i) + "\n";
  }
  printed = i - start;
  if (i < end) {
    out += "... next:" + std::to_string(i);
  } else {
    out.resize(out.size() - 1);  // upstream removes the last newline
  }
  return out;
}

}  // namespace

TEST(SensorList, AFreshNodeListsTaskFivesFiveAndPagesTheRest) {
  FakeHost host;
  host.seen = true;
  host.seen_ago = 42;
  host.clock_state = "restored";
  twinoak::Settings s(host);

  int printed = 0;
  const std::string page1 = sensorList(s, 0, printed);
  EXPECT_EQ(5, printed) << page1;
  EXPECT_EQ(159u, page1.size()) << page1;
  EXPECT_LT(page1.size(), 160u) << page1;
  EXPECT_NE(std::string::npos, page1.find("walter.wake=0\n")) << page1;
  EXPECT_NE(std::string::npos, page1.find("walter.seen=42\n")) << page1;
  EXPECT_NE(std::string::npos, page1.find("clock.state=restored\n")) << page1;
  EXPECT_NE(std::string::npos, page1.find("... next:5")) << page1;
  EXPECT_EQ(std::string::npos, page1.find("power.low_mv")) << page1;

  // `sensor list 5` is the rest of it, and the pair of thresholds is together.
  const std::string page2 = sensorList(s, 5, printed);
  EXPECT_EQ(4, printed) << page2;
  EXPECT_LT(page2.size(), 160u) << page2;
  EXPECT_NE(std::string::npos, page2.find("power.low_mv=3500\n")) << page2;
  EXPECT_NE(std::string::npos, page2.find("power.crit_mv=3300\n")) << page2;
  EXPECT_NE(std::string::npos, page2.find("obs.wake=0\n")) << page2;
  EXPECT_NE(std::string::npos, page2.find("client.refused=0")) << page2;
  EXPECT_EQ(std::string::npos, page2.find("next:")) << page2;
}

TEST(SensorList, WithBothCountersAtTheirWidestOnlyFourFitAndNothingOverruns) {
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
  const std::string out = sensorList(s, 0, printed);
  EXPECT_EQ(4, printed) << out;
  EXPECT_NE(std::string::npos, out.find("... next:4")) << out;
  EXPECT_LT(out.size(), 160u) << "upstream's 160-byte reply buffer is never overrun: " << out;
  EXPECT_NE(std::string::npos, out.find("fw.version=")) << out;
  EXPECT_NE(std::string::npos, out.find("walter.wake=1\n")) << out;
  EXPECT_NE(std::string::npos, out.find("link.errors=65535\n")) << out;
  EXPECT_NE(std::string::npos, out.find("walter.seen=4294967295\n")) << out;
  EXPECT_EQ(std::string::npos, out.find("clock.state")) << out;
}

TEST(SensorList, TheOneCaseThatOutgrowsTheWindowIsKnownCountedAndCutRatherThanSpilled) {
  // A release image's version string is longer than `0.0.0-dev` (the release build passes
  // TWINOAK_FW_VERSION, src/app/Version.cpp), which pushes the fw.version line from 78 bytes to
  // 82 - and with a `restored` clock and both counters still narrow, upstream's own loop writes
  // 162 bytes into a 160-byte reply.
  FakeHost host;
  host.version = "twinoak-repeater 0.1.0+abc1234 on MeshCore repeater-v1.17.1 (d9296435)";
  host.seen = true;
  host.seen_ago = 7;
  host.clock_state = "restored";
  twinoak::Settings s(host);

  int printed = 0;
  const std::string out = sensorList(s, 0, printed);
  EXPECT_EQ(5, printed) << out;
  EXPECT_EQ(162u, out.size()) << out;

  // What the firmware does with it: the session's 64 guard bytes absorb the write, the cut is
  // counted, and what leaves the link is the window's own 159 characters
  // (src/link/session/Session.cpp's cliHandler; test/test_link_session drives the real thing).
  // So the cost is the tail of `... next:5` and nothing else - every key is still readable with
  // `sensor get`, and `sensor list 5` still prints the pair.
  EXPECT_LT(out.size(), 160u + 64u) << "and it stays well inside the guard";
  EXPECT_NE(std::string::npos, out.substr(0, 159).find("clock.state=restored")) << out;
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
