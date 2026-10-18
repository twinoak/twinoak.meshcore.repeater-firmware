// src/roles/repeater/app/Settings.{h,cpp}: our ten keys as upstream's CLI sees them - `sensor list`,
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
// once it has written 134 bytes into a 160-byte reply: with these ten keys in this order a
// fresh node prints task 05's five and `... next:5`, a node whose counters have grown wide
// prints four and `... next:4`, `sensor list 5` prints the five that are left - and the two
// cases that reach the end of the window (the tenth key's own `10 vars` header, and a release
// version string) are measured here rather than found on a bench. Those measurements rest on
// how long `fw.version` is, which is how long the MeshCore tag is, so the version strings in
// this file are built by the firmware's own formatVersion from the build's own defines and the
// **bound** they rest on is the last test: a tag long enough to change what a device prints is
// red here before it is a surprise on a bench (tasks/phase-3.2/reports/01.md).

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "common/version/Version.h"
#include "roles/repeater/app/Settings.h"

namespace {

// The version string a device of this build prints, built by the firmware's own
// formatVersion over the same defines src/common/version/Version.cpp uses
// (platformio.ini's [env:native] passes them). Nothing here restates a tag or a pin: this
// file's subject is the *arithmetic* of upstream's `sensor list` over our table, and that
// arithmetic counts the bytes of this string - so a MeshCore tag two characters longer than
// today's changes what a device prints, and the bound at the end of this file is where it is
// noticed (tasks/phase-3.2/reports/01.md left this as the one thing a bump could still move
// silently).
std::string deviceVersion(const char* fw_version) {
  char buf[96];  // src/common/version/Version.cpp's own buffer, so a truncation truncates here
  twinoak::formatVersion(buf, sizeof(buf), TWINOAK_ROLE, fw_version, TWINOAK_MESHCORE_TAG,
                         TWINOAK_MESHCORE_PIN);
  return std::string(buf);
}

class FakeHost : public twinoak::SettingsHost {
 public:
  std::string version = deviceVersion("0.0.0-dev");
  uint16_t errors = 0;
  bool seen = false;
  uint32_t seen_ago = 0;
  std::string clock_state = "unset";
  bool pending = false;
  int wakes = 0;
  bool wake_accepted = true;
  uint16_t low_mv = 3500;
  uint16_t crit_mv = 3300;
  // The board refuses a pair the thresholds would not accept (src/common/power/BatteryWatch.h); here
  // the window is the same one, written out so that the table's own job - parsing - is what this
  // suite is testing.
  bool accept_thresholds = true;
  int low_sets = 0;
  int crit_sets = 0;
  bool obs_wake = false;
  int obs_sets = 0;
  uint32_t refused = 0;
  std::string first_boot = "never";

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
  const char* firstBootState() const override { return first_boot.c_str(); }
};

}  // namespace

TEST(TheTable, TheOrderIsStableAndIsWhatSensorListPrints) {
  FakeHost host;
  twinoak::Settings s(host);
  ASSERT_EQ(10, twinoak::Settings::count());

  // Task 05's five, in task 05's order, then task 06's two, then phase 2's task 03's and 04's,
  // then phase 3's task 03's - every later task's key appended, so nothing already documented
  // moved.
  const char* const want[] = {"fw.version",     "walter.wake",   "link.errors",
                              "walter.seen",    "clock.state",   "power.low_mv",
                              "power.crit_mv",  "obs.wake",      "client.refused",
                              "firstboot.state"};
  for (int i = 0; i < twinoak::Settings::count(); i++) {
    EXPECT_STREQ(want[i], twinoak::Settings::name(i)) << "index " << i;
  }
  EXPECT_EQ(nullptr, twinoak::Settings::name(-1));
  EXPECT_EQ(nullptr, twinoak::Settings::name(10));
  EXPECT_EQ(nullptr, s.value(-1));
  EXPECT_EQ(nullptr, s.value(10));
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
  EXPECT_STREQ("never", s.value(9)) << "firstboot.state, on a boot that found its settings";

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
  host.first_boot = "partial: 2 refused";
  EXPECT_STREQ("partial: 2 refused", s.value(9)) << "firstboot.state is the host's own string";
}

TEST(TheTable, SixOfTheTenAreReadOnlyAndAnUnknownKeyIsNotOurs) {
  FakeHost host;
  twinoak::Settings s(host);

  // `client.refused` is read-only on purpose: the ceiling it counts is a build constant, because
  // a limit the Walter could raise would be no backstop against the Walter
  // (src/roles/repeater/meshclient/SendCeiling.h).
  // `firstboot.state` likewise: it says what a boot did, and a boot cannot be talked out of it.
  const char* const read_only[] = {"fw.version",     "link.errors", "walter.seen", "clock.state",
                                   "client.refused", "firstboot.state"};
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
  EXPECT_FALSE(s.set("firstboot.stat", "done"));
  EXPECT_FALSE(s.set("firstboot.state", "done"));
  EXPECT_FALSE(s.set("firstboot.state", "never"));
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
  // var", which is its only word for "no" (src/roles/repeater/app/Settings.h).
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
  // Walter every hour for adverts alone (src/roles/repeater/meshclient/LinkObservations.h). Unlike
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
  // 160 characters, where nine keys wrote 159: the tenth key made upstream's own header one
  // byte longer (`10 vars`, not `9 vars`) and nothing else about this page changed. The window
  // holds 159 characters and a NUL, so what the console prints and what the link sends is this
  // page without the digit of `... next:5`, and the 64 guard bytes behind the window are what
  // absorb the byte (src/roles/repeater/main.cpp, src/common/link/session/Session.h).
  EXPECT_EQ(160u, page1.size()) << page1;
  EXPECT_NE(std::string::npos, page1.find("walter.wake=0\n")) << page1;
  EXPECT_NE(std::string::npos, page1.find("walter.seen=42\n")) << page1;
  EXPECT_NE(std::string::npos, page1.find("clock.state=restored\n")) << page1;
  EXPECT_NE(std::string::npos, page1.substr(0, 159).find("... next:")) << page1;
  EXPECT_EQ(std::string::npos, page1.find("power.low_mv")) << page1;

  // `sensor list 5` is the rest of it - all five of them on one page, the pair of thresholds
  // together - and it is nowhere near the window.
  const std::string page2 = sensorList(s, 5, printed);
  EXPECT_EQ(5, printed) << page2;
  EXPECT_GT(160u, page2.size()) << page2;
  EXPECT_NE(std::string::npos, page2.find("power.low_mv=3500\n")) << page2;
  EXPECT_NE(std::string::npos, page2.find("power.crit_mv=3300\n")) << page2;
  EXPECT_NE(std::string::npos, page2.find("obs.wake=0\n")) << page2;
  EXPECT_NE(std::string::npos, page2.find("client.refused=0\n")) << page2;
  EXPECT_NE(std::string::npos, page2.find("firstboot.state=never")) << page2;
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

TEST(SensorList, TheReleaseVersionCaseIsKnownCountedAndCutRatherThanSpilled) {
  // A release image's version string is longer than `0.0.0-dev` (the release build passes
  // TWINOAK_FW_VERSION, src/common/version/Version.cpp), which pushes the fw.version line from 78 bytes to
  // 82 - and with a `restored` clock and both counters still narrow, upstream's own loop writes
  // 163 bytes into a 160-byte reply.
  FakeHost host;
  host.version = deviceVersion("0.1.0+abc1234");
  host.seen = true;
  host.seen_ago = 7;
  host.clock_state = "restored";
  twinoak::Settings s(host);

  int printed = 0;
  const std::string out = sensorList(s, 0, printed);
  EXPECT_EQ(5, printed) << out;
  EXPECT_EQ(163u, out.size()) << out;

  // What the firmware does with it: 64 guard bytes behind the window absorb the write - the
  // link's and the console's alike - the link counts the cut, and what goes out is the window's
  // own 159 characters (src/common/link/session/Session.cpp's cliHandler and
  // src/roles/repeater/main.cpp; test/test_link_session drives the real session). So the cost is
  // the tail of `... next:5` and nothing else - every key is still readable with `sensor get`,
  // and `sensor list 5` still prints the five that are left.
  EXPECT_LT(out.size(), 160u + 64u) << "and it stays well inside the guard";
  EXPECT_NE(std::string::npos, out.substr(0, 159).find("clock.state=restored")) << out;
}

TEST(SensorList, ThePagingRestsOnTheVersionStringsLengthAndSaysSo) {
  // The two pages above are measured, and what they are measured at is the length of
  // `fw.version`'s line: everything else on the first page is narrow and fixed. The bound is
  // **70 characters of version string**, and it is arithmetic and not taste - upstream adds a
  // line while it has written fewer than 134 bytes, so the first page holds five keys while
  //
  //     8 (the "10 vars" header and its newline)
  //   + 12 + len(version) + 1                     the fw.version line
  //   + 14 + 14                                   walter.wake=0, link.errors=0
  //   + 15                                        walter.seen=<two digits>
  //   < 134
  //
  // which is len(version) <= 70. One character more and `clock.state` falls to the second page:
  // a different first page, a different `... next:<i>`, and a different line in docs/bench.md
  // section 5 - without one byte of this repository having changed.
  //
  // So the bound is asserted both ways, and then this build's own strings are held to it. The
  // version string is "twinoak-<role> <version> on MeshCore <tag> (<pin8>)", so what can push it
  // over is a longer MeshCore tag or a longer release version, and a bump that does is red here
  // first - which is what tasks/phase-3.2/reports/01.md's last paragraph asked for.
  const size_t kVersionBound = 70;
  {
    FakeHost host;
    host.version = std::string(kVersionBound, 'v');
    host.seen = true;
    host.seen_ago = 42;
    host.clock_state = "restored";
    twinoak::Settings s(host);
    int printed = 0;
    const std::string out = sensorList(s, 0, printed);
    EXPECT_EQ(5, printed) << "at the bound the first page is still five keys: " << out;
  }
  {
    FakeHost host;
    host.version = std::string(kVersionBound + 1, 'v');
    host.seen = true;
    host.seen_ago = 42;
    host.clock_state = "restored";
    twinoak::Settings s(host);
    int printed = 0;
    const std::string out = sensorList(s, 0, printed);
    EXPECT_EQ(4, printed) << "one character over the bound is a different first page: " << out;
    EXPECT_NE(std::string::npos, out.find("... next:4")) << out;
  }

  // This build, as it is - the dev string the tests above use - and a release image's, which is
  // "<the tag without its v>+<sha7>" (.github/workflows/release.yml).
  const char* const kReMeasure =
      "): re-measure test/test_settings' SensorList pages, docs/provisioning.md section 3's"
      " session and docs/bench.md section 5 - what a device prints has changed, and the tests"
      " above are the measurement";
  EXPECT_GE(kVersionBound, deviceVersion("0.0.0-dev").size())
      << "this build's own version string is over the bound (" << deviceVersion("0.0.0-dev")
      << kReMeasure;
  EXPECT_GE(kVersionBound, deviceVersion("0.1.0+abc1234").size())
      << "a release image's version string is over the bound ("
      << deviceVersion("0.1.0+abc1234") << kReMeasure;

  // And how much room is left, which is the number worth knowing: **none**. The string is
  // "twinoak-<role> <version> on MeshCore <tag> (<pin8>)" - 41 characters of frame for this role
  // - so the bound is len(version) + len(tag) <= 29, and `0.1.0+abc1234` with
  // `repeater-v1.17.1` is exactly 29. One character more anywhere - a `0.10.0` release, a tag
  // `repeater-v1.17.10` - is the four-key page above. The two EXPECTs before this one are what
  // go red when it happens; this one says why.
  EXPECT_EQ(kVersionBound, deviceVersion("0.1.0+abc1234").size())
      << "the paging has room it did not have: raise kVersionBound here, with the arithmetic";
  EXPECT_LT(kVersionBound, deviceVersion("0.10.0+abc1234").size())
      << "and one character more is over it";
  // A *dry run* of the release is the one build of ours that does not fit: its version is
  // `0.0.0-dryrun+<sha7>`, twenty characters, which makes the string 77 and the first page four
  // keys. That image is not a release (spec/FIRMWARE-MANIFEST.md section 3) and is never on a
  // unit in the field, so it is recorded here rather than asserted away.
  EXPECT_LT(kVersionBound, deviceVersion("0.0.0-dryrun+abc1234").size())
      << "a dry run's version string now fits too, which only makes the bound safer";
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
