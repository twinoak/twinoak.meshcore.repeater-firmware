// The scanner's portable half, held to what docs/noisescope.md says it is: the command line, the
// NDJSON lines and the sweep's arithmetic (phase 3's task 02).
//
// Three subjects, in the order a session with the instrument goes through them:
//
//   1. **the commands** - src/roles/scanner/Commands.h. Every word of docs/noisescope.md's table
//      is a verb and nothing else is, the command word is case-insensitive and an argument is
//      not, and every default is the one the page documents. What is held here is what a line
//      *means*, which NoiseScope settled inside a function that also drove a radio.
//   2. **the lines** - src/roles/scanner/Lines.h. Every message, parsed back with ArduinoJson:
//      a line is valid JSON, it carries the fields the page lists, and **no number in it is
//      anything but finite** - a NaN or an infinity is written as `null`, because `nan` is not
//      JSON and a line a parser drops carries nothing at all. The `help` text is the one thing
//      that is not JSON, and it is prefixed so that a parser skips it.
//   3. **the sweep** - src/roles/scanner/Sweep.h. How many points a sweep has and where each one
//      is, which receiver bandwidth a step needs, what the other three activities clamp their
//      arguments to, and the window of RSSI samples that `stat.rssi` and `dwell` report.
//   4. **the scope** - the fast sweep of phase 3.1's task 01, which is one subject across all
//      three of those and is therefore kept together at the end: the command, the plan (the
//      span cut into segments, each with its own image calibration), the frame and its base64,
//      the three aggregates, and the pinned binary of the plugin that runs inside the radio.
//   5. **the loop watchdog's deadline** - src/roles/scanner/LoopWatch.h, phase 3.5's task 03.
//      The 8 s check NoiseScope was designed around, which used to be the hardware watchdog's
//      timeout and is now arithmetic: a heartbeat from the loop, a cadence from an interrupt, and
//      the one question of whether the loop has come round. The board half of it is registers and
//      a vector (LoopWdt.h) and is not testable here; *when* it fires is, and that is what the
//      hardware watchdog's own timeout no longer says.
//
// ArduinoJson is the native environment's fixture reader already (test/test_link_codec) and it
// is here for the same reason: a line is held against a real parser and not against a string
// this test also wrote. Its default build rejects `nan` and `infinity`, which is precisely the
// refusal rule 2 is about.

#include <gtest/gtest.h>

#include <ArduinoJson.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "roles/scanner/Commands.h"
#include "roles/scanner/Lines.h"
#include "roles/scanner/LoopWatch.h"
#include "roles/scanner/Sweep.h"
// The vendored sweep plugin's binary, pinned by the last test of this file: a program for the
// radio's own core, and the one file under src/roles/scanner/vendor/ that is host-portable (it
// is an array of words and a #define, and nothing else).
#include "roles/scanner/vendor/sx126x_spectrum/SX126xSpectrumPlugin.h"

namespace {

using namespace twinoak::scanner;  // NOLINT: this is that namespace's own test

// ----------------------------------------------------------------------------- the sink

// Everything written, and the lines it came to. A fragment is not a line: the writer sends a
// message in several calls and ends it with a newline of its own, so what a reader sees is what
// this splits.
class StringSink : public LineSink {
 public:
  void write(const char* text) override {
    // The longest fragment, because that is the whole of the buffer the writer is allowed: a
    // 2048-point `scope` frame is about 2.9 kB and no part of it may be held in one piece
    // (src/roles/scanner/Lines.h, kLineFragmentBytes).
    const size_t n = std::strlen(text);
    if (n > longest_) longest_ = n;
    all_ += text;
  }

  size_t longestFragment() const { return longest_; }

  const std::string& text() const { return all_; }

  std::vector<std::string> lines() const {
    std::vector<std::string> out;
    std::string current;
    for (char c : all_) {
      if (c == '\n') {
        out.push_back(current);
        current.clear();
      } else {
        current += c;
      }
    }
    if (!current.empty()) out.push_back(current);
    return out;
  }

  void clear() {
    all_.clear();
    longest_ = 0;
  }

 private:
  std::string all_;
  size_t longest_ = 0;
};

// The one line a message wrote, parsed. Fails the test when there is not exactly one, when it
// is not JSON, or when any number in it is not finite.
JsonDocument parseOne(const StringSink& sink) {
  JsonDocument doc;
  const std::vector<std::string> lines = sink.lines();
  EXPECT_EQ(1u, lines.size()) << "expected one line, got: " << sink.text();
  if (lines.size() != 1) return doc;
  const DeserializationError err = deserializeJson(doc, lines[0]);
  EXPECT_FALSE(err) << "not JSON (" << err.c_str() << "): " << lines[0];
  // ArduinoJson's default build has no NaN and no Infinity, so the line above is already the
  // check; this says the same thing of the text, which is what a Walter's own parser sees. The
  // words are looked for where a *value* begins - after a ':', a ',' or a '[', with or without a
  // sign - because "info" is a perfectly good `t`.
  for (const char* word : {"nan", "inf"}) {
    for (const char* lead : {":", ",", "[", ":-", ",-", "[-"}) {
      EXPECT_EQ(std::string::npos, lines[0].find(std::string(lead) + word))
          << "'" << lead << word << "' is not a JSON number: " << lines[0];
    }
  }
  return doc;
}

// A line of tokens, since tokenize() writes into its argument.
class Line {
 public:
  explicit Line(const std::string& text) : buf_(text.begin(), text.end()) { buf_.push_back('\0'); }
  Tokens tokens() { return tokenize(buf_.data()); }

 private:
  std::vector<char> buf_;
};

Tokens tokensOf(Line& line) { return line.tokens(); }

ScanDefaults defaults() {
  ScanDefaults d;
  d.f0 = 863.0;
  d.f1 = 870.0;
  d.step_khz = 25.0;
  d.sweep_dwell_ms = 20;
  d.hist_samples = 2048;
  return d;
}

Meta someMeta() {
  Meta m;
  m.seq = 17;
  m.ts = 1754820100;
  m.ms = 112000;
  return m;
}

}  // namespace

// ================================================================= 1. the commands

TEST(Commands, TheCommandWordIsCaseInsensitiveAndAnArgumentIsNot) {
  Line line("SwEeP 863 870 Serial");
  const Tokens tokens = tokensOf(line);
  ASSERT_EQ(4, tokens.count);
  EXPECT_STREQ("sweep", tokens.at[0]);
  EXPECT_STREQ("863", tokens.at[1]);
  EXPECT_STREQ("Serial", tokens.at[3]) << "an argument's case is the operator's";
  EXPECT_EQ(kVerbSweep, verbOf(tokens.at[0]));
}

TEST(Commands, RepeatedSeparatorsAreOneAndNineWordsAreEight) {
  Line line("  dwell \t\t 869.618   300  ");
  const Tokens tokens = tokensOf(line);
  ASSERT_EQ(3, tokens.count);
  EXPECT_STREQ("dwell", tokens.at[0]);
  EXPECT_STREQ("300", tokens.at[2]);

  Line many("sweep 1 2 3 4 5 6 7 8 9");
  const Tokens capped = tokensOf(many);
  EXPECT_EQ(kMaxTokens, capped.count) << "a longer line is read up to the cap and no further";
}

TEST(Commands, AnEmptyLineIsNoCommand) {
  Line line("   \t ");
  EXPECT_EQ(0, tokensOf(line).count);
}

TEST(Commands, EveryWordOfTheProtocolIsAVerbAndNothingElseIs) {
  const struct {
    const char* word;
    Verb verb;
  } kTable[] = {
      {"help", kVerbHelp},     {"info", kVerbInfo},   {"stat", kVerbStat},
      {"sweep", kVerbSweep},   {"scope", kVerbScope}, {"hist", kVerbHist},
      {"dwell", kVerbDwell},   {"cad", kVerbCad},     {"stop", kVerbStop},
      {"home", kVerbHome},     {"boost", kVerbBoost}, {"auto", kVerbAuto},
      {"time", kVerbTime},     {"reboot", kVerbReboot}, {"dfu", kVerbDfu},
      {"hang", kVerbHang},
  };
  for (const auto& row : kTable) {
    EXPECT_EQ(row.verb, verbOf(row.word)) << row.word;
    EXPECT_STREQ(row.word, nameOf(row.verb)) << "the ack says the word the page spells";
  }
  for (const char* word : {"", "swee", "sweeps", "status", "tx", "transmit", "send"}) {
    EXPECT_EQ(kVerbUnknown, verbOf(word)) << word;
  }
  EXPECT_STREQ("", nameOf(kVerbUnknown)) << "an unknown command's ack carries what was typed";
}

TEST(Commands, SweepWithNoArgumentsIsTheDocumentedDefaults) {
  Line line("sweep");
  const ScanArgs args = parseScan(tokensOf(line), false, defaults());
  EXPECT_DOUBLE_EQ(863.0, args.f0);
  EXPECT_DOUBLE_EQ(870.0, args.f1);
  EXPECT_DOUBLE_EQ(25.0, args.step_khz);
  EXPECT_EQ(20u, args.fourth) << "a sweep's fourth number is its dwell in milliseconds";
  EXPECT_EQ(1, args.passes);
}

TEST(Commands, HistWithNoArgumentsTakesTheSampleCountInstead) {
  Line line("hist");
  const ScanArgs args = parseScan(tokensOf(line), true, defaults());
  EXPECT_EQ(2048u, args.fourth);
}

TEST(Commands, BothEndsOfARangeAreReadTogetherOrNotAtAll) {
  Line one("sweep 865");
  const ScanArgs partial = parseScan(tokensOf(one), false, defaults());
  EXPECT_DOUBLE_EQ(863.0, partial.f0) << "one number is not a range, so the defaults stand";
  EXPECT_DOUBLE_EQ(870.0, partial.f1);

  Line both("sweep 869.4 869.8 25 40 0");
  const ScanArgs full = parseScan(tokensOf(both), false, defaults());
  EXPECT_DOUBLE_EQ(869.4, full.f0);
  EXPECT_DOUBLE_EQ(869.8, full.f1);
  EXPECT_DOUBLE_EQ(25.0, full.step_khz);
  EXPECT_EQ(40u, full.fourth);
  EXPECT_EQ(0, full.passes) << "passes 0 is until stop";
}

TEST(Commands, DwellNeedsAFrequencyAndDefaultsTheRest) {
  Line bare("dwell");
  DwellArgs args;
  EXPECT_FALSE(parseDwell(tokensOf(bare), args));

  Line one("dwell 869.618");
  ASSERT_TRUE(parseDwell(tokensOf(one), args));
  EXPECT_DOUBLE_EQ(869.618, args.f_mhz);
  EXPECT_EQ(60u, args.secs);
  EXPECT_EQ(1000u, args.win_ms);

  Line all("dwell 869.618 300 500");
  ASSERT_TRUE(parseDwell(tokensOf(all), args));
  EXPECT_EQ(300u, args.secs);
  EXPECT_EQ(500u, args.win_ms);
}

TEST(Commands, CadDefaultsToTheHomeChannelAndAHundredTries) {
  Line bare("cad");
  const CadArgs args = parseCad(tokensOf(bare), 869.618);
  EXPECT_DOUBLE_EQ(869.618, args.f_mhz);
  EXPECT_EQ(100u, args.count);

  Line given("cad 868.1 250");
  const CadArgs other = parseCad(tokensOf(given), 869.618);
  EXPECT_DOUBLE_EQ(868.1, other.f_mhz);
  EXPECT_EQ(250u, other.count);
}

TEST(Commands, HomeKeepsTheModemSettingsUnlessAllThreeAreGiven) {
  HomeArgs current;
  current.f_mhz = 869.618;
  current.bw_khz = 62.5;
  current.sf = 8;
  current.cr = 8;

  Line bare("home");
  HomeArgs out;
  EXPECT_FALSE(parseHome(tokensOf(bare), current, out));

  Line retune("home 868.4");
  ASSERT_TRUE(parseHome(tokensOf(retune), current, out));
  EXPECT_DOUBLE_EQ(868.4, out.f_mhz);
  EXPECT_DOUBLE_EQ(62.5, out.bw_khz) << "a frequency alone retunes and changes nothing else";
  EXPECT_EQ(8, out.sf);

  Line full("home 868.4 125 10 5");
  ASSERT_TRUE(parseHome(tokensOf(full), current, out));
  EXPECT_DOUBLE_EQ(125.0, out.bw_khz);
  EXPECT_EQ(10, out.sf);
  EXPECT_EQ(5, out.cr);

  Line partial("home 868.4 125");
  ASSERT_TRUE(parseHome(tokensOf(partial), current, out));
  EXPECT_DOUBLE_EQ(62.5, out.bw_khz) << "two of the three is not a modem setting";
}

TEST(Commands, BoostIsOnForOnAndForOneAndOffForEverythingElse) {
  bool on = false;
  Line bare("boost");
  EXPECT_FALSE(parseBoost(tokensOf(bare), on));

  for (const char* word : {"on", "1"}) {
    Line line(std::string("boost ") + word);
    ASSERT_TRUE(parseBoost(tokensOf(line), on)) << word;
    EXPECT_TRUE(on) << word;
  }
  for (const char* word : {"off", "0", "ON", "yes"}) {
    Line line(std::string("boost ") + word);
    ASSERT_TRUE(parseBoost(tokensOf(line), on)) << word;
    EXPECT_FALSE(on) << word << ": an argument is not lowercased, and this is NoiseScope's own"
                                " reading of it";
  }
}

TEST(Commands, AutoNeedsANumberAndZeroIsOff) {
  uint32_t minutes = 99;
  Line bare("auto");
  EXPECT_FALSE(parseAuto(tokensOf(bare), minutes));
  EXPECT_EQ(99u, minutes) << "a refused command changes nothing";

  Line off("auto 0");
  ASSERT_TRUE(parseAuto(tokensOf(off), minutes));
  EXPECT_EQ(0u, minutes);

  Line ten("auto 10");
  ASSERT_TRUE(parseAuto(tokensOf(ten), minutes));
  EXPECT_EQ(10u, minutes);
}

TEST(Commands, AnEpochBeforeTheFloorIsNotATime) {
  uint32_t epoch = 7;
  Line bare("time");
  EXPECT_FALSE(parseEpoch(tokensOf(bare), epoch));

  Line zero("time 0");
  EXPECT_FALSE(parseEpoch(tokensOf(zero), epoch));

  Line old("time 1000000000");
  EXPECT_FALSE(parseEpoch(tokensOf(old), epoch)) << "before November 2023 is a host's mistake";
  EXPECT_EQ(7u, epoch) << "and nothing is set";

  Line now("time 1754820000");
  ASSERT_TRUE(parseEpoch(tokensOf(now), epoch));
  EXPECT_EQ(1754820000u, epoch);
}

TEST(Commands, DfuIsTheDriveUnlessTheWordIsSerial) {
  Line bare("dfu");
  EXPECT_FALSE(parseDfuSerialOnly(tokensOf(bare)));
  Line serial("dfu serial");
  EXPECT_TRUE(parseDfuSerialOnly(tokensOf(serial)));
  Line other("dfu uf2");
  EXPECT_FALSE(parseDfuSerialOnly(tokensOf(other)));
}

TEST(Commands, HangDefaultsPastTheDeadlineAndIsClampedShortOfTheHardwareWatchdog) {
  // The one command of this firmware whose point is to stop the loop (docs/noisescope.md section
  // 5). What the numbers have to be: a default past the 8 s the loop check allows, so that `hang`
  // with no argument demonstrates the reset; and a clamp well short of WDT_TIMEOUT_S, so that no
  // argument can reach the *hardware* watchdog and leave a reader wondering which of the two
  // fired.
  Line bare("hang");
  EXPECT_EQ(kHangDefaultMs, parseHangMs(tokensOf(bare)));
  Line zero("hang 0");
  EXPECT_EQ(kHangDefaultMs, parseHangMs(tokensOf(zero))) << "0 is not a hang, it is no argument";
  Line word("hang now");
  EXPECT_EQ(kHangDefaultMs, parseHangMs(tokensOf(word))) << "a word that is not a number reads as"
                                                            " absent";
  Line short_one("hang 3000");
  EXPECT_EQ(3000u, parseHangMs(tokensOf(short_one))) << "a stall shorter than the deadline is"
                                                        " allowed: it shows the check *not*"
                                                        " firing";
  Line long_one("hang 600000");
  EXPECT_EQ(kHangMaxMs, parseHangMs(tokensOf(long_one)));
  EXPECT_GT(kHangDefaultMs, 8000u) << "the default must outlast the loop check's deadline";
  EXPECT_LT(kHangMaxMs, 60000u) << "and the clamp must not reach the hardware watchdog's timeout";
}

TEST(Commands, EveryCommandThatCanBeRefusedHasAUsageLine) {
  for (Verb verb : {kVerbScope, kVerbDwell, kVerbHome, kVerbBoost, kVerbAuto, kVerbTime}) {
    const std::string usage = usageOf(verb);
    EXPECT_EQ(0u, usage.find("usage: ")) << "verb " << int(verb) << ": " << usage;
    EXPECT_NE(std::string::npos, usage.find(nameOf(verb)))
        << "a usage line names its own command: " << usage;
  }
  for (Verb verb : {kVerbHelp, kVerbInfo, kVerbStat, kVerbStop, kVerbSweep, kVerbHist, kVerbCad,
                    kVerbReboot, kVerbDfu, kVerbHang}) {
    EXPECT_STREQ("", usageOf(verb)) << "verb " << int(verb) << " has no argument to be refused"
                                       " over";
  }
}

// ================================================================= 2. the lines

namespace {

BootFields bootFields() {
  BootFields f;
  f.fw = "noisescope";
  f.version = "1.2.3";
  f.role = "scanner";
  f.board = "rak4630-single";
  f.hw = "rak4630";
  f.radio = "sx1262";
  f.reset = "por";
  f.radio_ok = true;
  f.rtc = true;
  f.ina = false;
  f.ts = 1754820000;
  return f;
}

}  // namespace

TEST(Lines, BootSaysWhatTheImageIs) {
  StringSink sink;
  Lines lines(sink);
  lines.boot(bootFields());
  const JsonDocument doc = parseOne(sink);
  EXPECT_STREQ("boot", doc["t"].as<const char*>());
  EXPECT_STREQ("noisescope", doc["fw"].as<const char*>()) << "the firmware's name is its own and not the role's";
  EXPECT_STREQ("1.2.3", doc["ver"].as<const char*>()) << "and its version is this repository's";
  EXPECT_STREQ("scanner", doc["role"].as<const char*>()) << "the two fields phase 3's task 02 added";
  EXPECT_STREQ("rak4630-single", doc["board"].as<const char*>());
  EXPECT_STREQ("rak4630", doc["hw"].as<const char*>());
  EXPECT_STREQ("sx1262", doc["radio"].as<const char*>());
  EXPECT_STREQ("por", doc["reset"].as<const char*>());
  EXPECT_TRUE(doc["wdt"].is<const char*>()) << "the field is there even when no watchdog fired";
  EXPECT_STREQ("", doc["wdt"].as<const char*>());
  EXPECT_TRUE(doc["radio_ok"].is<bool>());
  EXPECT_TRUE(doc["radio_ok"].as<bool>());
  EXPECT_EQ(1, doc["rtc"].as<int>());
  EXPECT_EQ(0, doc["ina"].as<int>());
  EXPECT_EQ(1754820000u, doc["ts"].as<uint32_t>());
}

TEST(Lines, BootSaysWhichWatchdogWithoutANewResetWord) {
  // Both watchdogs mean *the loop stopped*, so both are the one `reset` word a reader already
  // enumerates (docs/noisescope.md section 5: por|pin|wdt|soft|lockup). Which of the two it was
  // is a field beside it and not a sixth word - phase 3.5's task 03, deliverable 3.
  for (const char* half : {"loop", "hw"}) {
    StringSink sink;
    Lines lines(sink);
    BootFields f = bootFields();
    f.reset = "wdt";
    f.wdt = half;
    lines.boot(f);
    const JsonDocument doc = parseOne(sink);
    EXPECT_STREQ("wdt", doc["reset"].as<const char*>()) << half;
    EXPECT_STREQ(half, doc["wdt"].as<const char*>());
  }
  // And a null, which is what a caller that has nothing to say should be able to pass.
  StringSink sink;
  Lines lines(sink);
  BootFields f = bootFields();
  f.wdt = nullptr;
  lines.boot(f);
  EXPECT_STREQ("", parseOne(sink)["wdt"].as<const char*>());
}

TEST(Lines, InfoSaysTheSameAndTheHomeChannel) {
  StringSink sink;
  Lines lines(sink);
  InfoFields f;
  f.boot = bootFields();
  f.build = "Oct 12 2026";
  f.devid_high = 0xDEADBEEF;
  f.devid_low = 0x01020304;
  f.boosted_gain = true;
  f.auto_minutes = 10;
  f.home_f_mhz = 869.618;
  f.home_bw_khz = 62.5;
  f.home_sf = 8;
  f.home_cr = 8;
  lines.info(f);
  const JsonDocument doc = parseOne(sink);
  EXPECT_STREQ("info", doc["t"].as<const char*>());
  EXPECT_STREQ("scanner", doc["role"].as<const char*>());
  EXPECT_STREQ("deadbeef01020304", doc["devid"].as<const char*>()) << "both halves, zero-padded";
  EXPECT_STREQ("por", doc["reset"].as<const char*>());
  EXPECT_STREQ("", doc["wdt"].as<const char*>()) << "`info` carries the pair `boot` carries";
  EXPECT_EQ(1, doc["boost"].as<int>());
  EXPECT_EQ(10u, doc["auto_min"].as<uint32_t>());
  EXPECT_FLOAT_EQ(869.618, doc["home"]["f"].as<double>());
  EXPECT_FLOAT_EQ(62.5, doc["home"]["bw"].as<double>());
  EXPECT_EQ(8, doc["home"]["sf"].as<int>());
  EXPECT_EQ(8, doc["home"]["cr"].as<int>());
}

TEST(Lines, StatCarriesTheWindowOnlyWhenThereIsOne) {
  StringSink sink;
  Lines lines(sink);
  StatFields f;
  f.meta.ts = 1754820010;
  f.meta.ms = 12345;
  f.uptime_s = 12;
  f.mode = "monitor";
  f.home_f_mhz = 869.618;
  f.have_rssi = false;
  lines.stat(f);
  {
    const JsonDocument doc = parseOne(sink);
    EXPECT_STREQ("stat", doc["t"].as<const char*>());
    EXPECT_EQ(12u, doc["up"].as<uint32_t>());
    EXPECT_TRUE(doc["rssi"].isNull()) << "no sample is no object, not an average of nothing";
    EXPECT_TRUE(doc["pwr"].isNull());
    EXPECT_TRUE(doc["seq"].isNull()) << "a heartbeat is not a measurement";
  }

  sink.clear();
  f.have_rssi = true;
  f.rssi.n = 9900;
  f.rssi.avg = -114.24;
  f.rssi.max = -99.5;
  f.rssi.min = -116.0;
  f.rssi.dc = 0.03124;
  f.rssi.th = -106.0;
  f.pwr.present = true;
  f.pwr.vpanel_mv = 18100;
  f.pwr.ipanel_ma = 120;
  f.pwr.vbat_mv = 4012;
  f.pwr.ibat_ma = -52;
  f.pwr.vload_mv = 3980;
  f.pwr.iload_ma = 85;
  lines.stat(f);
  const JsonDocument doc = parseOne(sink);
  EXPECT_EQ(9900u, doc["rssi"]["n"].as<uint32_t>());
  EXPECT_FLOAT_EQ(-114.2, doc["rssi"]["avg"].as<double>()) << "one decimal, as the page says";
  EXPECT_FLOAT_EQ(0.0312, doc["rssi"]["dc"].as<double>()) << "four decimals";
  EXPECT_FLOAT_EQ(18.10, doc["pwr"]["vpanel"].as<double>());
  EXPECT_EQ(120, doc["pwr"]["ipanel"].as<int>());
  EXPECT_FLOAT_EQ(4.012, doc["pwr"]["vbat"].as<double>());
  EXPECT_EQ(-52, doc["pwr"]["ibat"].as<int>()) << "negative is discharging";
  EXPECT_FLOAT_EQ(3.980, doc["pwr"]["vload"].as<double>());
  EXPECT_EQ(85, doc["pwr"]["iload"].as<int>());
}

TEST(Lines, ASweepLineHasOneValuePerPoint) {
  StringSink sink;
  Lines lines(sink);
  SweepFields f;
  f.meta = someMeta();
  ASSERT_TRUE(planSweep(869.4, 869.5, 25.0, f.plan));
  ASSERT_EQ(5, f.plan.points);
  const double avg[5] = {-115.2, -114.9, -113.0, -112.5, -111.1};
  const double max[5] = {-108.9, -107.0, -101.1, -100.0, -99.9};
  f.dwell_ms = 20;
  f.avg = avg;
  f.max = max;
  lines.sweep(f);
  const JsonDocument doc = parseOne(sink);
  EXPECT_STREQ("sweep", doc["t"].as<const char*>());
  EXPECT_EQ(17u, doc["seq"].as<uint32_t>());
  EXPECT_EQ(5, doc["n"].as<int>());
  EXPECT_STREQ("dBm", doc["unit"].as<const char*>());
  EXPECT_FLOAT_EQ(869.4, doc["f0"].as<double>());
  EXPECT_FLOAT_EQ(29.3, doc["rbw"].as<double>());
  ASSERT_EQ(5u, doc["avg"].as<JsonArrayConst>().size());
  ASSERT_EQ(5u, doc["max"].as<JsonArrayConst>().size());
  EXPECT_FLOAT_EQ(-115.2, doc["avg"][0].as<double>());
  EXPECT_FLOAT_EQ(-99.9, doc["max"][4].as<double>());
}

TEST(Lines, AHistLineHasTheEnginesThirtyThreeBins) {
  StringSink sink;
  Lines lines(sink);
  HistFields f;
  f.meta = someMeta();
  f.f_mhz = 869.618;
  f.rbw_khz = 29.3;
  f.samples = 2048;
  uint16_t bins[kHistBins] = {0};
  bins[27] = 1790;
  bins[26] = 210;
  f.bins = bins;
  lines.hist(f);
  const JsonDocument doc = parseOne(sink);
  EXPECT_STREQ("hist", doc["t"].as<const char*>());
  EXPECT_EQ(2048, doc["n"].as<int>());
  EXPECT_EQ(-11, doc["b0"].as<int>());
  EXPECT_EQ(4, doc["bw_db"].as<int>());
  ASSERT_EQ(size_t(kHistBins), doc["bins"].as<JsonArrayConst>().size());
  EXPECT_EQ(1790, doc["bins"][27].as<int>());
}

TEST(Lines, DwellAndCadCarryTheirOwnFields) {
  StringSink sink;
  Lines lines(sink);
  DwellFields d;
  d.meta = someMeta();
  d.f_mhz = 869.618;
  d.win_ms = 1000;
  d.stats.n = 990;
  d.stats.avg = -113.8;
  d.stats.max = -100.1;
  d.stats.min = -115.9;
  d.stats.dc = 0.0404;
  d.stats.th = -105.9;
  lines.dwell(d);
  {
    const JsonDocument doc = parseOne(sink);
    EXPECT_STREQ("dwell", doc["t"].as<const char*>());
    EXPECT_EQ(1000u, doc["win"].as<uint32_t>());
    EXPECT_EQ(990u, doc["n"].as<uint32_t>());
    EXPECT_FLOAT_EQ(-113.8, doc["avg"].as<double>());
  }

  sink.clear();
  CadFields c;
  c.meta = someMeta();
  c.f_mhz = 869.618;
  c.sf = 8;
  c.bw_khz = 62.5;
  c.n = 100;
  c.detections = 17;
  lines.cad(c);
  const JsonDocument doc = parseOne(sink);
  EXPECT_STREQ("cad", doc["t"].as<const char*>());
  EXPECT_EQ(8, doc["sf"].as<int>());
  EXPECT_EQ(100, doc["n"].as<int>());
  EXPECT_EQ(17, doc["det"].as<int>());
}

TEST(Lines, AckDoneAndErrAreTheCommandResponses) {
  StringSink sink;
  Lines lines(sink);
  lines.ack("sweep", true, "started");
  {
    const JsonDocument doc = parseOne(sink);
    EXPECT_STREQ("ack", doc["t"].as<const char*>());
    EXPECT_STREQ("sweep", doc["cmd"].as<const char*>());
    EXPECT_TRUE(doc["ok"].as<bool>());
    EXPECT_STREQ("started", doc["msg"].as<const char*>());
  }

  sink.clear();
  lines.ack("auto-sweep", true, "");
  {
    const JsonDocument doc = parseOne(sink);
    EXPECT_TRUE(doc["msg"].isNull()) << "an empty message is no field at all";
  }

  sink.clear();
  lines.done("sweep", someMeta());
  {
    const JsonDocument doc = parseOne(sink);
    EXPECT_STREQ("done", doc["t"].as<const char*>());
    EXPECT_STREQ("sweep", doc["cmd"].as<const char*>());
    EXPECT_EQ(17u, doc["seq"].as<uint32_t>());
  }

  sink.clear();
  lines.err("scan timeout at 869.618");
  const JsonDocument doc = parseOne(sink);
  EXPECT_STREQ("err", doc["t"].as<const char*>());
  EXPECT_STREQ("scan timeout at 869.618", doc["msg"].as<const char*>());
}

TEST(Lines, ANumberThatIsNotANumberIsNullAndTheLineIsStillJson) {
  StringSink sink;
  Lines lines(sink);
  StatFields f;
  f.mode = "monitor";
  f.home_f_mhz = NAN;
  f.have_rssi = true;
  f.rssi.n = 3;
  f.rssi.avg = NAN;
  f.rssi.max = INFINITY;
  f.rssi.min = -INFINITY;
  f.rssi.dc = 0.5;
  f.rssi.th = -100.0;
  lines.stat(f);
  const JsonDocument doc = parseOne(sink);  // valid JSON, no "nan", no "inf"
  EXPECT_TRUE(doc["home"].isNull());
  EXPECT_TRUE(doc["rssi"]["avg"].isNull());
  EXPECT_TRUE(doc["rssi"]["max"].isNull());
  EXPECT_TRUE(doc["rssi"]["min"].isNull());
  EXPECT_FLOAT_EQ(0.5, doc["rssi"]["dc"].as<double>()) << "and the finite ones are still there";
}

TEST(Lines, ASweepOfValuesThatAreNotNumbersIsStillJson) {
  StringSink sink;
  Lines lines(sink);
  SweepFields f;
  f.meta = someMeta();
  ASSERT_TRUE(planSweep(869.4, 869.5, 25.0, f.plan));
  const double avg[5] = {NAN, -114.9, INFINITY, -112.5, -INFINITY};
  f.avg = avg;
  f.max = nullptr;  // a pass that recorded nothing at all
  f.dwell_ms = 20;
  lines.sweep(f);
  const JsonDocument doc = parseOne(sink);
  EXPECT_TRUE(doc["avg"][0].isNull());
  EXPECT_FLOAT_EQ(-114.9, doc["avg"][1].as<double>());
  ASSERT_EQ(5u, doc["max"].as<JsonArrayConst>().size());
  EXPECT_TRUE(doc["max"][0].isNull());
}

TEST(Lines, TheHelpTextIsChatterAndEveryLineOfItSaysSo) {
  StringSink sink;
  Lines lines(sink);
  lines.help("1.2.3");
  const std::vector<std::string> lines_out = sink.lines();
  EXPECT_GT(lines_out.size(), 5u);
  for (const std::string& line : lines_out) {
    ASSERT_FALSE(line.empty());
    EXPECT_EQ('#', line[0]) << "a machine parser skips anything that does not begin with '{': "
                            << line;
    EXPECT_NE('{', line[0]);
  }
  EXPECT_NE(std::string::npos, sink.text().find("1.2.3"));
}

// ================================================================= 3. the sweep

TEST(Sweep, TheReceiverBandwidthIsTheSmallestOneWiderThanTheStep) {
  EXPECT_DOUBLE_EQ(kScopeRbwMaxKhz, kFskRbwKhz[kFskRbwCount - 1])
      << "the cap a `rbw_khz` is held to is the widest filter the part has";
  EXPECT_DOUBLE_EQ(4.8, narrowestRbwKhz(0.0)) << "a bandwidth lands on one of the part's filters";
  EXPECT_DOUBLE_EQ(23.4, narrowestRbwKhz(20.0));
  EXPECT_DOUBLE_EQ(23.4, narrowestRbwKhz(23.4));
  EXPECT_DOUBLE_EQ(kScopeRbwMaxKhz, narrowestRbwKhz(5000.0));
  EXPECT_DOUBLE_EQ(29.3, pickRbwKhz(25.0)) << "1.1 x 25 kHz is 27.5, and 29.3 is the next one up";
  EXPECT_DOUBLE_EQ(4.8, pickRbwKhz(1.0));
  EXPECT_DOUBLE_EQ(117.3, pickRbwKhz(100.0));
  EXPECT_DOUBLE_EQ(467.0, pickRbwKhz(1000.0)) << "nothing is wider, so the widest it is";
}

TEST(Sweep, APlanCountsThePointsWithBothEndsIncluded) {
  SweepPlan plan;
  ASSERT_TRUE(planSweep(863.0, 870.0, 25.0, plan));
  EXPECT_EQ(281, plan.points) << "7 MHz in 25 kHz steps, the ends included";
  EXPECT_DOUBLE_EQ(29.3, plan.rbw_khz);
  EXPECT_DOUBLE_EQ(863.0, pointFrequencyMhz(plan, 0));
  EXPECT_DOUBLE_EQ(870.0, pointFrequencyMhz(plan, 280));
  EXPECT_NEAR(863.025, pointFrequencyMhz(plan, 1), 1e-9);
}

TEST(Sweep, APlanStopsAtTheLastPointInsideARangeThatIsNotAWholeNumberOfSteps) {
  SweepPlan plan;
  ASSERT_TRUE(planSweep(863.0, 863.11, 25.0, plan));
  EXPECT_EQ(5, plan.points);
  EXPECT_NEAR(863.1, pointFrequencyMhz(plan, 4), 1e-9);
  EXPECT_LE(pointFrequencyMhz(plan, plan.points - 1), plan.f1)
      << "no point of a sweep is outside the range it was asked for";
}

TEST(Sweep, APlanIsRefusedWhenItIsNotASweep) {
  SweepPlan plan;
  EXPECT_FALSE(planSweep(870.0, 863.0, 25.0, plan)) << "the ends crossed";
  EXPECT_FALSE(planSweep(869.0, 869.0, 25.0, plan)) << "and a range of nothing";
  EXPECT_FALSE(planSweep(863.0, 870.0, 0.5, plan)) << "a step below a kilohertz";
  EXPECT_FALSE(planSweep(863.0, 870.0, 1001.0, plan)) << "and above a megahertz";
  EXPECT_FALSE(planSweep(863.0, 863.01, 25.0, plan)) << "fewer than two points";
  EXPECT_FALSE(planSweep(863.0, 890.0, 25.0, plan)) << "more points than there is room for";
  EXPECT_FALSE(planSweep(100.0, 200.0, 25.0, plan)) << "below the radio's range";
  EXPECT_FALSE(planSweep(900.0, 1000.0, 25.0, plan)) << "and above it";
  // The one thing that is not the band: a sweep may be pointed at a suspected blocker, which is
  // the point of the instrument (docs/noisescope.md).
  EXPECT_TRUE(planSweep(921.0, 925.0, 100.0, plan)) << "GSM900 downlink, through the filter";
}

TEST(Sweep, ASingleFrequencyIsTheRadiosRangeAndNotTheBands) {
  EXPECT_TRUE(frequencyOk(150.0));
  EXPECT_TRUE(frequencyOk(869.618));
  EXPECT_TRUE(frequencyOk(960.0));
  EXPECT_FALSE(frequencyOk(149.9));
  EXPECT_FALSE(frequencyOk(960.1));
}

TEST(Sweep, TheArgumentsOfTheOtherThreeActivitiesAreClamped) {
  EXPECT_TRUE(sweepDwellOk(5));
  EXPECT_TRUE(sweepDwellOk(500));
  EXPECT_FALSE(sweepDwellOk(4));
  EXPECT_FALSE(sweepDwellOk(501));

  EXPECT_EQ(kHistSamplesMin, clampHistSamples(0));
  EXPECT_EQ(kHistSamplesMin, clampHistSamples(255));
  EXPECT_EQ(2048, clampHistSamples(2048));

  EXPECT_EQ(1u, clampDwellSeconds(0));
  EXPECT_EQ(300u, clampDwellSeconds(300));
  EXPECT_EQ(kDwellSecondsMax, clampDwellSeconds(kDwellSecondsMax + 1));

  EXPECT_EQ(kDwellWindowMinMs, clampDwellWindowMs(0));
  EXPECT_EQ(1000u, clampDwellWindowMs(1000));
  EXPECT_EQ(kDwellWindowMaxMs, clampDwellWindowMs(kDwellWindowMaxMs + 1));

  EXPECT_EQ(1, clampCadCount(0));
  EXPECT_EQ(100, clampCadCount(100));
  EXPECT_EQ(kCadCountMax, clampCadCount(kCadCountMax + 1));
}

TEST(Sweep, TheHomeChannelsModemSettingsAreOnesTheRadioHas) {
  EXPECT_TRUE(homeSettingsOk(869.618, 8, 8));
  EXPECT_TRUE(homeSettingsOk(869.618, 5, 5));
  EXPECT_TRUE(homeSettingsOk(869.618, 12, 8));
  EXPECT_FALSE(homeSettingsOk(869.618, 4, 8)) << "spreading factors are 5 to 12";
  EXPECT_FALSE(homeSettingsOk(869.618, 13, 8));
  EXPECT_FALSE(homeSettingsOk(869.618, 8, 4)) << "and coding rates 5 to 8";
  EXPECT_FALSE(homeSettingsOk(869.618, 8, 9));
  EXPECT_FALSE(homeSettingsOk(70.0, 8, 8)) << "and the frequency still has to be one";
}

TEST(Window, AWindowWithNoSampleIsNotAnAverageOfNothing) {
  RssiWindow window;
  WindowStats stats;
  stats.n = 42;
  EXPECT_FALSE(window.stats(stats));
  EXPECT_EQ(42u, stats.n) << "and the caller's fields are untouched";
  EXPECT_EQ(0u, window.count());
  EXPECT_DOUBLE_EQ(kFirstThresholdDbm, window.threshold());
}

TEST(Window, AWindowAveragesAndCountsWhatWasAboveTheThreshold) {
  RssiWindow window;
  // The first window counts against kFirstThresholdDbm, -100 dBm.
  window.add(-116.0);
  window.add(-114.0);
  window.add(-99.0);  // above
  window.add(-90.0);  // above
  WindowStats stats;
  ASSERT_TRUE(window.stats(stats));
  EXPECT_EQ(4u, stats.n);
  EXPECT_DOUBLE_EQ((-116.0 - 114.0 - 99.0 - 90.0) / 4.0, stats.avg);
  EXPECT_DOUBLE_EQ(-90.0, stats.max);
  EXPECT_DOUBLE_EQ(-116.0, stats.min);
  EXPECT_DOUBLE_EQ(0.5, stats.dc) << "two of four above the threshold is a duty cycle of a half";
  EXPECT_DOUBLE_EQ(kFirstThresholdDbm, stats.th);
}

TEST(Window, TheNextWindowsThresholdIsThisOnesMinimumPlusTheDelta) {
  RssiWindow window;
  window.add(-116.0);
  window.add(-114.0);
  window.reset();
  EXPECT_DOUBLE_EQ(-116.0 + kBurstThresholdDeltaDb, window.threshold());

  // -106 is the threshold now, so only the burst counts.
  window.add(-115.0);
  window.add(-100.0);
  WindowStats stats;
  ASSERT_TRUE(window.stats(stats));
  EXPECT_DOUBLE_EQ(0.5, stats.dc);
  EXPECT_DOUBLE_EQ(-106.0, stats.th);
}

TEST(Window, AnEmptyWindowCarriesNothingOverAndARestartForgetsEverything) {
  RssiWindow window;
  window.add(-120.0);
  window.reset();
  const double carried = window.threshold();
  EXPECT_DOUBLE_EQ(-110.0, carried);

  window.reset();  // nothing was sampled in between
  EXPECT_DOUBLE_EQ(carried, window.threshold())
      << "a window with no sample has no minimum to carry over";

  window.restart();
  EXPECT_DOUBLE_EQ(kFirstThresholdDbm, window.threshold());
  EXPECT_EQ(0u, window.count());
}

// ================================================================= 4. the scope
//
// The fast sweep (phase 3.1's task 01), which is one subject across all three of the above and so
// is kept together: what `scope` means as a command, what its plan asks the radio for, and what
// a frame of it says. Its arithmetic is the part no bench can check afterwards - a segment that
// is a megahertz too wide is a reading through the wrong image calibration and looks like a
// signal - so it is all here.

namespace {

// The base64 of a frame, decoded the way a reader of the protocol decodes it (RFC 4648 section
// 4). Empty for text that is not base64, which is itself a failure worth seeing.
std::vector<uint8_t> fromBase64(const std::string& text) {
  static const char* kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::vector<uint8_t> out;
  if (text.size() % 4 != 0) return out;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : text) {
    if (c == '=') break;
    const char* at = std::strchr(kAlphabet, c);
    if (at == nullptr || c == '\0') return {};
    acc = (acc << 6) | uint32_t(at - kAlphabet);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(uint8_t((acc >> bits) & 0xFFu));
    }
  }
  return out;
}

ScopeRequest request(double f0, double f1) {
  ScopeRequest r;
  r.f0 = f0;
  r.f1 = f1;
  return r;
}

// Every segment of a plan, so that a test can say something about all of them at once.
std::vector<ScopeSegment> segmentsOf(const ScopePlan& plan) {
  std::vector<ScopeSegment> out;
  for (uint16_t i = 0; i < plan.segments; i++) {
    ScopeSegment seg;
    if (scopeSegment(plan, i, seg)) out.push_back(seg);
  }
  return out;
}

}  // namespace

// ----------------------------------------------------------------- the command

TEST(Scope, BothEndsAreRequiredAndTheRestHasDefaults) {
  Line line("scope 863 870");
  ScopeArgs args;
  ASSERT_TRUE(parseScope(tokensOf(line), args));
  EXPECT_FALSE(args.off);
  EXPECT_DOUBLE_EQ(863.0, args.f0);
  EXPECT_DOUBLE_EQ(870.0, args.f1);
  EXPECT_EQ(0u, args.points) << "0 is 'the plan's default' and not 'no points'";
  EXPECT_DOUBLE_EQ(0.0, args.rbw_khz);
  EXPECT_EQ(0u, args.interval_ms);
  EXPECT_EQ(kScopeAggMax, args.agg);

  // Unlike `sweep`, which has a band to fall back on: a scope's span is the operator's whole
  // point, so one end alone is not a command.
  Line one("scope 863");
  EXPECT_FALSE(parseScope(tokensOf(one), args));
  Line none("scope");
  EXPECT_FALSE(parseScope(tokensOf(none), args));
}

TEST(Scope, EveryArgumentIsReadWhereThePageSaysItIs) {
  Line line("scope 150 960 2048 467 500 mean");
  ScopeArgs args;
  ASSERT_TRUE(parseScope(tokensOf(line), args));
  EXPECT_DOUBLE_EQ(150.0, args.f0);
  EXPECT_DOUBLE_EQ(960.0, args.f1);
  EXPECT_EQ(2048u, args.points);
  EXPECT_DOUBLE_EQ(467.0, args.rbw_khz);
  EXPECT_EQ(500u, args.interval_ms);
  EXPECT_EQ(kScopeAggMean, args.agg);
}

TEST(Scope, OffIsTheOneWordAndAnUnknownAggregateIsNotACommand) {
  ScopeArgs args;
  Line off("scope off");
  ASSERT_TRUE(parseScope(tokensOf(off), args));
  EXPECT_TRUE(args.off);

  Line bad("scope 863 870 250 0 0 median");
  EXPECT_FALSE(parseScope(tokensOf(bad), args)) << "there are three aggregates, and that is not"
                                                  " one of them";

  for (const char* word : {"max", "mean", "last"}) {
    ScopeAgg agg = kScopeAggLast;
    ASSERT_TRUE(scopeAggOf(word, agg)) << word;
    EXPECT_STREQ(word, scopeAggName(agg)) << "the frame says the word the command took";
  }
  ScopeAgg agg = kScopeAggMax;
  EXPECT_FALSE(scopeAggOf("", agg));
  EXPECT_FALSE(scopeAggOf("MAX", agg)) << "an argument is case-sensitive (docs/noisescope.md)";
}

// ----------------------------------------------------------------- the ROM gate

TEST(Scope, ThePluginIsUploadedToOneRomAndNoOther) {
  EXPECT_TRUE(scopeRomSupported("SX1261 V2D 2D02"));
  EXPECT_TRUE(scopeRomSupported("SX1261 V2D 2D02 and whatever follows the fifteenth character"));
  EXPECT_FALSE(scopeRomSupported("SX1261 V1B 2D00")) << "an older ROM is a different core";
  EXPECT_FALSE(scopeRomSupported("SX1261 V2D 2D0"));
  EXPECT_FALSE(scopeRomSupported(""));
  EXPECT_FALSE(scopeRomSupported(nullptr));
  // Once the plugin has loaded, the fifteenth character is its tag and not the ROM's own - which
  // is why this is asked **before** anything is uploaded (SX126xSpectrumScanner::begin).
  EXPECT_FALSE(scopeRomSupported("SX1261 V2D 2D0P"));
}

// ----------------------------------------------------------------- the plan

TEST(Scope, AHomeBandScopeIsOnePluginCallAndCoversItsSpan) {
  ScopePlan plan;
  ASSERT_TRUE(planScope(request(863.0, 870.0), plan));
  EXPECT_EQ(kScopeDefaultPoints, plan.points) << "no point count given: one plugin call";
  EXPECT_EQ(1, plan.segments) << "7 MHz is inside one segment";
  EXPECT_NEAR(28.0, plan.step_khz, 1e-9) << "7 MHz over 250 points";
  EXPECT_DOUBLE_EQ(39.0, plan.rbw_khz) << "the narrowest bandwidth at least 1.1 x 28 kHz";
  EXPECT_GT(plan.cov, 1.0) << "no gaps, so a narrow carrier cannot hide between two points";
  EXPECT_NEAR(863.014, plan.first_mhz, 1e-9) << "the first point's centre is half a step in";

  const std::vector<ScopeSegment> segs = segmentsOf(plan);
  ASSERT_EQ(1u, segs.size());
  EXPECT_EQ(0, segs[0].first);
  EXPECT_EQ(250, segs[0].points);
  // What the one segment is calibrated for is what it measures: the first point's centre and the
  // last one's, half a 39 kHz filter either side.
  EXPECT_NEAR(863.014 - 0.0195, segs[0].cal_lo_mhz, 1e-9);
  EXPECT_NEAR(869.986 + 0.0195, segs[0].cal_hi_mhz, 1e-9);
}

TEST(Scope, ASpanWiderThanOneCallIsCutIntoSegmentsThatEachCalibrateForThemselves) {
  // 20 MHz at 2048 points: 9.766 kHz a step, so the plugin's 250 points a call is what bites and
  // the cut is nine calls - each of them well inside kScopeSegmentMhz.
  ScopeRequest r = request(860.0, 880.0);
  r.points = 2048;
  ScopePlan plan;
  ASSERT_TRUE(planScope(r, plan));
  EXPECT_EQ(2048, plan.points);
  EXPECT_EQ(9, plan.segments);

  const std::vector<ScopeSegment> segs = segmentsOf(plan);
  ASSERT_EQ(9u, segs.size());
  uint32_t total = 0;
  for (size_t i = 0; i < segs.size(); i++) {
    EXPECT_LE(segs[i].points, kScopePluginMaxPoints) << "segment " << i;
    EXPECT_LE(segs[i].cal_hi_mhz - segs[i].cal_lo_mhz, kScopeSegmentMhz) << "segment " << i;
    EXPECT_EQ(total, segs[i].first) << "the segments are consecutive and leave no point out";
    // A segment is calibrated for the frequencies it measures: its first point's centre, half a
    // receiver bandwidth below, to its last point's centre, half a bandwidth above.
    EXPECT_NEAR(segs[i].first_mhz - plan.rbw_khz / 2000.0, segs[i].cal_lo_mhz, 1e-9) << i;
    EXPECT_NEAR(segs[i].first_mhz + double(segs[i].points - 1) * plan.step_khz / 1000.0 +
                    plan.rbw_khz / 2000.0,
                segs[i].cal_hi_mhz, 1e-9)
        << i;
    total += segs[i].points;
  }
  EXPECT_EQ(2048u, total);
  EXPECT_NEAR(plan.first_mhz - plan.rbw_khz / 2000.0, segs.front().cal_lo_mhz, 1e-9);
  EXPECT_NEAR(860.0 + plan.step_khz / 2000.0, plan.first_mhz, 1e-9)
      << "and the frame starts half a step inside the span that was asked for";
  ScopeSegment past;
  EXPECT_FALSE(scopeSegment(plan, plan.segments, past)) << "and there is no tenth";
}

TEST(Scope, TheWholeTuningRangeIsSegmentedByBothBoundsAtOnce) {
  // 150-960 MHz at 2048 points: 395.5 kHz a step, so a segment of 32 MHz holds 80 points and
  // *that* is what bites, not the plugin's 250.
  ScopeRequest r = request(kRadioLowMhz, kRadioHighMhz);
  r.points = 2048;
  ScopePlan plan;
  ASSERT_TRUE(planScope(r, plan));
  EXPECT_EQ(26, plan.segments) << "810 MHz in segments of at most 32 MHz";

  const std::vector<ScopeSegment> segs = segmentsOf(plan);
  ASSERT_EQ(26u, segs.size());
  const double step_mhz = plan.step_khz / 1000.0;
  for (size_t i = 0; i < segs.size(); i++) {
    EXPECT_LE(segs[i].points, kScopePluginMaxPoints) << "segment " << i;
    EXPECT_LE(segs[i].cal_hi_mhz - segs[i].cal_lo_mhz, kScopeSegmentMhz) << "segment " << i;
    // The radio is only ever *tuned* to a point's centre, and every one of those is inside the
    // range the chip has. The calibration band reaches half a filter past the span's own edge,
    // which is a fact about what the receiver sees and not a frequency it is set to.
    const double last_mhz = segs[i].first_mhz + double(segs[i].points - 1) * step_mhz;
    EXPECT_GE(segs[i].first_mhz, kRadioLowMhz) << "segment " << i;
    EXPECT_LE(last_mhz, kRadioHighMhz) << "segment " << i;
  }
  // 32 MHz holds 80 points of 395.5 kHz, and 2048 points over 26 segments spread as evenly as
  // whole points can: twenty segments of 79 and six of 78, none of them over the bound.
  EXPECT_EQ(79, segs.front().points);
  EXPECT_EQ(78, segs.back().points);
  EXPECT_DOUBLE_EQ(467.0, plan.rbw_khz) << "nothing is wider than 467 kHz";
}

TEST(Scope, CoverageBelowOneIsReportedAndNotRefused) {
  // The acceptance criterion's own case: the whole tuning range at 1024 points is 791 kHz a step
  // through the widest filter there is, so more than two fifths of the span is not looked at at
  // all. That is a legitimate thing to ask for - it is how a blocker is found - and the frame
  // says so with `cov`, which the plotter draws as a grey band.
  ScopeRequest r = request(kRadioLowMhz, kRadioHighMhz);
  r.points = 1024;
  ScopePlan plan;
  ASSERT_TRUE(planScope(r, plan));
  EXPECT_NEAR(791.015625, plan.step_khz, 1e-6);
  EXPECT_DOUBLE_EQ(467.0, plan.rbw_khz);
  EXPECT_NEAR(467.0 / 791.015625, plan.cov, 1e-9);
  EXPECT_LT(plan.cov, 1.0);
  EXPECT_DOUBLE_EQ(plan.rbw_khz / plan.step_khz, plan.cov);
}

TEST(Scope, ABandwidthAskedForDecidesHowManyPointsHaveNoGaps) {
  // 150-960 MHz at 467 kHz needs 1735 points to leave no gap, which is what a `rbw` with no
  // point count asks for (tasks/phase-3.1/01-the-fast-sweep.md).
  ScopeRequest r = request(kRadioLowMhz, kRadioHighMhz);
  r.rbw_khz = 467.0;
  ScopePlan plan;
  ASSERT_TRUE(planScope(r, plan));
  EXPECT_EQ(1735, plan.points);
  EXPECT_DOUBLE_EQ(467.0, plan.rbw_khz);
  EXPECT_GE(plan.cov, 1.0) << "which is the whole point of that default";

  // A bandwidth lands on one of the radio's own filters, rounded up, and never above 467.
  ScopeRequest narrow = request(869.0, 869.5);
  narrow.rbw_khz = 20.0;
  ASSERT_TRUE(planScope(narrow, plan));
  EXPECT_DOUBLE_EQ(23.4, plan.rbw_khz) << "20 kHz is not a filter the part has; 23.4 is the next";
  ScopeRequest wide = request(kRadioLowMhz, kRadioHighMhz);
  wide.rbw_khz = 5000.0;
  ASSERT_TRUE(planScope(wide, plan));
  EXPECT_DOUBLE_EQ(kScopeRbwMaxKhz, plan.rbw_khz) << "nothing is wider than 467 kHz";
  EXPECT_EQ(1735, plan.points);

  // And where no gaps would need more points than a frame holds, the frame wins and `cov` says
  // what was given up: 810 MHz through a 100 kHz filter would be 8100 points.
  ScopeRequest fine = request(kRadioLowMhz, kRadioHighMhz);
  fine.rbw_khz = 100.0;
  ASSERT_TRUE(planScope(fine, plan));
  EXPECT_EQ(kScopeMaxPoints, plan.points);
  EXPECT_LT(plan.cov, 1.0);
}

TEST(Scope, ARequestThatIsNotAScopeIsRefused) {
  ScopePlan plan;
  EXPECT_FALSE(planScope(request(870.0, 863.0), plan)) << "the ends crossed";
  EXPECT_FALSE(planScope(request(869.0, 869.0), plan)) << "and a span of nothing";
  EXPECT_FALSE(planScope(request(149.0, 870.0), plan)) << "below the radio's range";
  EXPECT_FALSE(planScope(request(863.0, 961.0), plan)) << "and above it";

  ScopeRequest few = request(863.0, 870.0);
  few.points = kScopeMinPoints - 1;
  EXPECT_FALSE(planScope(few, plan)) << "fewer points than a spectrum is";
  ScopeRequest many = request(863.0, 870.0);
  many.points = kScopeMaxPoints + 1;
  EXPECT_FALSE(planScope(many, plan)) << "more points than a frame holds";

  ScopeRequest tiny = request(869.6, 869.6001);
  tiny.points = 2048;
  EXPECT_FALSE(planScope(tiny, plan)) << "a step below a hundred hertz, which is finer than the"
                                        " radio tunes";

  ScopeRequest slow = request(863.0, 870.0);
  slow.interval_ms = kScopeIntervalMaxMs + 1;
  EXPECT_FALSE(planScope(slow, plan)) << "longer than a minute between frames is not a stream";

  // And the one that is not a refusal: 150-960 is exactly what this mode is for.
  EXPECT_TRUE(planScope(request(kRadioLowMhz, kRadioHighMhz), plan));
}

TEST(Scope, NoLegalRequestNeedsMoreSegmentsThanThereIsRoomFor) {
  // kScopeMaxSegments is a claim about the arithmetic, so it is checked against the arithmetic
  // and not against a memory of it: every point count the command takes, over the widest span
  // there is, which is where both bounds on a segment are at their worst.
  uint32_t worst = 0;
  uint32_t worst_points = 0;
  for (uint32_t points = kScopeMinPoints; points <= kScopeMaxPoints; points++) {
    ScopeRequest r = request(kRadioLowMhz, kRadioHighMhz);
    r.points = points;
    ScopePlan plan;
    ASSERT_TRUE(planScope(r, plan)) << points << " points over the whole tuning range";
    ASSERT_LE(plan.segments, kScopeMaxSegments) << points << " points";
    const std::vector<ScopeSegment> segs = segmentsOf(plan);
    ASSERT_EQ(size_t(plan.segments), segs.size()) << points << " points";
    uint32_t total = 0;
    for (const ScopeSegment& seg : segs) {
      ASSERT_GE(seg.points, 1) << points << " points";
      ASSERT_LE(seg.points, kScopePluginMaxPoints) << points << " points";
      ASSERT_LE(seg.cal_hi_mhz - seg.cal_lo_mhz, kScopeSegmentMhz + 1e-9) << points << " points";
      total += seg.points;
    }
    ASSERT_EQ(points, total) << "every point of the frame is measured exactly once";
    if (plan.segments > worst) {
      worst = plan.segments;
      worst_points = points;
    }
  }
  EXPECT_EQ(26u, worst) << "the worst case of the two bounds together, over the widest span";
  EXPECT_EQ(51u, worst_points) << "810 MHz in 51 points: 15.9 MHz a step, two to a segment";
  EXPECT_LE(worst, kScopeMaxSegments);
}

TEST(Scope, ASweepStaysInsideWhatTheChipAndTheWatchdogAllow) {
  // One plugin call holds BUSY for its whole duration and nothing feeds the watchdog inside it,
  // so two bounds matter: RadioLib waits 1000 ms for BUSY (Module.h:224) and the hardware
  // watchdog is 8 s. The worst a request can be is the narrowest filter there is, which is where
  // an RSSI update takes longest.
  EXPECT_EQ(86u, scopeSegmentMs(kScopePluginMaxPoints, 4.8))
      << "250 points at 4.8 kHz: (250 + 8) x 1.6 / 4.8 ms";
  EXPECT_EQ(22u, scopeSegmentMs(250, 19.5)) << "about the fork's own number for 19.5 kHz";
  EXPECT_EQ(6u, scopeSegmentMs(250, 78.2)) << "and for 78 kHz";

  uint32_t worst_segment = 0;
  uint32_t worst_sweep = 0;
  for (uint32_t points = kScopeMinPoints; points <= kScopeMaxPoints; points++) {
    // Three spans: the whole tuning range (the widest step there is), one megahertz, and the
    // narrowest span this point count may have - which is the narrowest step, and so the
    // narrowest filter and the slowest RSSI update.
    const double spans[][2] = {{kRadioLowMhz, kRadioHighMhz},
                               {869.0, 870.0},
                               {869.6, 869.6 + double(points) * kScopeMinStepKhz / 1000.0}};
    for (const auto& span : spans) {
      ScopeRequest r = request(span[0], span[1]);
      r.points = points;
      ScopePlan plan;
      if (!planScope(r, plan)) continue;
      if (plan.segment_ms > worst_segment) worst_segment = plan.segment_ms;
      if (plan.sweep_ms > worst_sweep) worst_sweep = plan.sweep_ms;
    }
  }
  EXPECT_LT(worst_segment, 1000u) << "a plugin call must answer before RadioLib stops waiting"
                                     " for BUSY";
  EXPECT_LT(worst_sweep, 4000u) << "and a whole sweep inside half the watchdog - with every"
                                   " segment a separate pass of loop() on top of that";
}

TEST(Scope, ASlowSinkGetsAnIntervalItCanCarry) {
  // The Walter's UART is 115200 baud and a frame is bytes: a scope that asked for every sweep
  // gets the interval the port can actually carry, and the `ack` says which (docs/noisescope.md
  // section 5). USB CDC is handed 0 and keeps what was asked for.
  EXPECT_EQ(0u, scopeIntervalFloorMs(2048, 0)) << "a sink with no floor worth computing";

  const uint32_t floor_2048 = scopeIntervalFloorMs(2048, 115200);
  const uint32_t floor_250 = scopeIntervalFloorMs(250, 115200);
  EXPECT_GT(floor_2048, floor_250) << "a bigger frame needs longer";
  // A frame must fit into one part in kScopeSinkDutyDivisor of the interval it is sent at, so
  // that a scope can never block the UART writer or starve the watchdog.
  for (uint16_t points : {uint16_t(16), uint16_t(250), uint16_t(1024), uint16_t(2048)}) {
    const uint32_t floor_ms = scopeIntervalFloorMs(points, 115200);
    const double frame_ms = double(scopeFrameBytes(points)) * 10.0 * 1000.0 / 115200.0;
    EXPECT_GE(double(floor_ms), frame_ms * double(kScopeSinkDutyDivisor)) << points << " points";
    EXPECT_LE(floor_ms, kScopeIntervalMaxMs) << "and never longer than the longest interval";
  }

  ScopeRequest r = request(863.0, 870.0);
  r.points = 2048;
  r.sink_baud = 115200;
  ScopePlan uart;
  ASSERT_TRUE(planScope(r, uart));
  EXPECT_EQ(floor_2048, uart.interval_ms) << "'a frame per sweep' on the UART is the floor";

  r.interval_ms = kScopeIntervalMaxMs;
  ASSERT_TRUE(planScope(r, uart));
  EXPECT_EQ(kScopeIntervalMaxMs, uart.interval_ms) << "a slower interval is kept";

  r.interval_ms = 0;
  r.sink_baud = 0;
  ScopePlan usb;
  ASSERT_TRUE(planScope(r, usb));
  EXPECT_EQ(0u, usb.interval_ms) << "USB gets every sweep if it asked for one";
}

// ----------------------------------------------------------------- the frame

TEST(Scope, AFrameCarriesOneByteAPointAndSaysWhatWasTaken) {
  StringSink sink;
  Lines lines(sink);
  ScopeFields f;
  f.meta = someMeta();
  ScopeRequest r = request(863.0, 870.0);
  r.points = 32;
  r.agg = kScopeAggMean;
  ASSERT_TRUE(planScope(r, f.plan));
  std::vector<uint8_t> levels(32);
  for (size_t i = 0; i < levels.size(); i++) levels[i] = uint8_t(200 + i);
  levels[7] = kScopeNoReading;
  f.levels = levels.data();
  f.sweeps = 12;
  f.sps_x10 = 476;
  lines.scope(f);

  const JsonDocument doc = parseOne(sink);
  EXPECT_STREQ("scope", doc["t"].as<const char*>());
  EXPECT_EQ(17u, doc["seq"].as<uint32_t>());
  EXPECT_EQ(32, doc["n"].as<int>());
  EXPECT_STREQ("mean", doc["agg"].as<const char*>());
  EXPECT_EQ(12, doc["sweeps"].as<int>());
  EXPECT_EQ(476, doc["sps"].as<int>()) << "sweeps per second x10, as the page says";
  EXPECT_NEAR(863.109375, doc["f0"].as<double>(), 1e-6) << "the first point's centre";
  EXPECT_NEAR(218.75, doc["step"].as<double>(), 1e-6);
  EXPECT_NEAR(f.plan.rbw_khz, doc["rbw"].as<double>(), 1e-6);
  EXPECT_NEAR(f.plan.cov, doc["cov"].as<double>(), 1e-3);

  const std::vector<uint8_t> back = fromBase64(doc["lv"].as<std::string>());
  ASSERT_EQ(levels.size(), back.size()) << "one byte a point, and base64 is what carries them";
  EXPECT_EQ(levels, back);
  EXPECT_EQ(kScopeNoReading, back[7]) << "255 is 'no reading' and survives the trip";
}

TEST(Scope, ATwoThousandPointFrameIsWrittenInFragmentsAndStaysInsideItsBound) {
  StringSink sink;
  Lines lines(sink);
  ScopeFields f;
  f.meta.seq = 0xFFFFFFFFu;
  f.meta.ts = 0xFFFFFFFFu;
  f.meta.ms = 0xFFFFFFFFu;
  ScopeRequest r = request(kRadioLowMhz, kRadioHighMhz);
  r.points = kScopeMaxPoints;
  ASSERT_TRUE(planScope(r, f.plan));
  std::vector<uint8_t> levels(kScopeMaxPoints);
  for (size_t i = 0; i < levels.size(); i++) levels[i] = uint8_t(i & 0xFFu);
  f.levels = levels.data();
  f.sweeps = 0xFFFFu;
  f.sps_x10 = 0xFFFFu;
  lines.scope(f);

  const JsonDocument doc = parseOne(sink);
  EXPECT_EQ(kScopeMaxPoints, doc["n"].as<int>());
  const std::vector<uint8_t> back = fromBase64(doc["lv"].as<std::string>());
  ASSERT_EQ(size_t(kScopeMaxPoints), back.size());
  EXPECT_EQ(levels, back);

  // The two things that bound is for: no part of the line is ever held in one piece, and the
  // whole line is no longer than what the sink's interval floor was computed from.
  EXPECT_LE(sink.longestFragment(), kLineFragmentBytes)
      << "a fragment longer than the writer's buffer would have been truncated";
  EXPECT_LE(sink.text().size(), scopeFrameBytes(kScopeMaxPoints))
      << "the frame is longer than scopeIntervalFloorMs() was told it would be, so the UART's"
         " floor is too low";
  EXPECT_GT(sink.text().size(), scopeBase64Length(kScopeMaxPoints));
}

TEST(Scope, AFrameOfOnePointIsStillInsideItsBound) {
  StringSink sink;
  Lines lines(sink);
  ScopeFields f;
  f.meta = someMeta();
  f.plan.points = 1;
  f.plan.first_mhz = 869.618;
  f.plan.step_khz = 25.0;
  f.plan.rbw_khz = 29.3;
  f.plan.cov = 29.3 / 25.0;
  const uint8_t level = 230;
  f.levels = &level;
  lines.scope(f);
  const JsonDocument doc = parseOne(sink);
  EXPECT_EQ(1, doc["n"].as<int>());
  const std::vector<uint8_t> back = fromBase64(doc["lv"].as<std::string>());
  ASSERT_EQ(1u, back.size());
  EXPECT_EQ(230, back[0]);
  EXPECT_LE(sink.text().size(), scopeFrameBytes(1));
}

TEST(Scope, AFrameOfNumbersThatAreNotNumbersIsStillJson) {
  // The same rule as every other line: a number that is not one is written `null` and never
  // `nan`, because a line a parser drops carries nothing at all.
  StringSink sink;
  Lines lines(sink);
  ScopeFields f;
  f.meta = someMeta();
  f.plan.points = 2;
  f.plan.first_mhz = NAN;
  f.plan.step_khz = INFINITY;
  f.plan.rbw_khz = -INFINITY;
  f.plan.cov = NAN;
  f.levels = nullptr;
  lines.scope(f);
  const JsonDocument doc = parseOne(sink);
  EXPECT_TRUE(doc["f0"].isNull());
  EXPECT_TRUE(doc["step"].isNull());
  EXPECT_TRUE(doc["cov"].isNull());
  EXPECT_EQ(2, doc["n"].as<int>());
}

TEST(Scope, InfoCarriesTheRadiosRom) {
  StringSink sink;
  Lines lines(sink);
  InfoFields f;
  f.boot = bootFields();
  f.build = "Oct 13 2026";
  f.rom = kScopeRom;
  f.home_f_mhz = 869.618;
  f.home_bw_khz = 62.5;
  f.home_sf = 8;
  f.home_cr = 8;
  lines.info(f);
  const JsonDocument doc = parseOne(sink);
  EXPECT_STREQ(kScopeRom, doc["rom"].as<const char*>());
  // And the rest of `info` is what it was: phase 3's task 02 settled those fields and this task
  // adds exactly one.
  EXPECT_STREQ("info", doc["t"].as<const char*>());
  EXPECT_STREQ("scanner", doc["role"].as<const char*>());
  EXPECT_FLOAT_EQ(869.618, doc["home"]["f"].as<double>());
}

// ----------------------------------------------------------------- arming a segment

// A radio that records what it was asked, in order, and answers what the test tells it to.
struct FakeArmRadio : ScopeArmRadio {
  char log[512];
  int standby_code = 0;
  int frequency_code = 0;
  int calibrate_code = 0;
  int sweep_code = 0;
  int reload_code = 0;
  unsigned device_errors = 0;
  int refusals = 0;  // how many calibrations in a row the chip refuses (IMG_CALIB_ERR), then none

  FakeArmRadio() { log[0] = '\0'; }

  void note(const char* fmt, ...) {
    char piece[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(piece, sizeof(piece), fmt, ap);
    va_end(ap);
    strncat(log, piece, sizeof(log) - strlen(log) - 1);
  }
  int standby() override {
    note("standby;");
    return standby_code;
  }
  int setFrequency(double mhz) override {
    note("setFrequency(%.3f);", mhz);
    return frequency_code;
  }
  int calibrateImage(double lo, double hi) override {
    note("calibrateImage(%.3f,%.3f);", lo, hi);
    return calibrate_code;
  }
  unsigned deviceErrors() override {
    note("deviceErrors;");
    if (refusals > 0) {
      refusals--;
      return device_errors | kScopeImageCalibrationError;
    }
    return device_errors;
  }
  int reload() override {
    note("reload;");
    return reload_code;
  }
  int setSweep(double first, double step, uint16_t points, double rbw) override {
    note("setSweep(%.3f,%.3f,%u,%.1f);", first, step, unsigned(points), rbw);
    return sweep_code;
  }
};

static ScopePlan planOf(double f0, double f1, uint32_t points) {
  ScopeRequest r;
  r.f0 = f0;
  r.f1 = f1;
  r.points = points;
  ScopePlan plan;
  EXPECT_TRUE(planScope(r, plan));
  return plan;
}

TEST(Scope, ASegmentIsArmedInTheOneOrderTheBoardAccepts) {
  // `scope 150 250`: four segments of 63 points, 400 kHz apart, through the 467 kHz filter - the
  // span of p3.5-02's probe table, whose third segment the first board refused until the order
  // below. The frequency is set *before* anything is calibrated, exactly one calibration is
  // made and it is the segment's own range, the chip is asked whether it took, and only then
  // are the plugin's parameters loaded.
  const ScopePlan plan = planOf(150.0, 250.0, 250);
  ASSERT_EQ(4u, plan.segments);
  ScopeSegment seg;
  ASSERT_TRUE(scopeSegment(plan, 2, seg));
  EXPECT_NEAR(200.6, seg.first_mhz, 1e-9);

  EXPECT_EQ(62u, seg.points);  // 250 points over 4 segments: 63, 63, 62, 62
  EXPECT_NEAR(200.6 - 0.2335, seg.cal_lo_mhz, 1e-6);
  EXPECT_NEAR(200.6 + 61 * 0.4 + 0.2335, seg.cal_hi_mhz, 1e-6);

  FakeArmRadio radio;
  ScopeArmResult out;
  EXPECT_TRUE(armScopeSegment(radio, plan, seg, out));
  EXPECT_EQ(kScopeArmNone, out.failed);
  EXPECT_EQ(0, out.code);
  char expected[160];
  snprintf(expected, sizeof(expected),
           "standby;setFrequency(213.000);calibrateImage(%.3f,%.3f);deviceErrors;"
           "setSweep(200.600,0.400,62,467.0);",
           seg.cal_lo_mhz, seg.cal_hi_mhz);
  EXPECT_STREQ(expected, radio.log);
  EXPECT_FALSE(out.reloaded);

  // The centre is the plugin's own formula (first + step * points / 2, in float), so that
  // setSweep()'s setFrequency() of the same number is a move of zero and calibrates nothing.
  EXPECT_FLOAT_EQ(float(200.6) + float(0.4) * 62.0f / 2.0f, scopeSegmentCentreMhz(plan, seg));
}

TEST(Scope, AnArmThatFailsNamesTheCallAndStopsThere) {
  const ScopePlan plan = planOf(150.0, 250.0, 250);
  ScopeSegment seg;
  ASSERT_TRUE(scopeSegment(plan, 2, seg));
  char calibrated[160];  // the log up to and including the calibration
  snprintf(calibrated, sizeof(calibrated),
           "standby;setFrequency(213.000);calibrateImage(%.3f,%.3f);", seg.cal_lo_mhz,
           seg.cal_hi_mhz);
  char asked[200];  // and with the chip asked
  snprintf(asked, sizeof(asked), "%sdeviceErrors;", calibrated);

  {
    FakeArmRadio radio;
    radio.standby_code = -707;
    ScopeArmResult out;
    EXPECT_FALSE(armScopeSegment(radio, plan, seg, out));
    EXPECT_EQ(kScopeArmStandby, out.failed);
    EXPECT_EQ(-707, out.code);
    EXPECT_STREQ("standby;", radio.log);
  }
  {
    FakeArmRadio radio;
    radio.frequency_code = -705;
    ScopeArmResult out;
    EXPECT_FALSE(armScopeSegment(radio, plan, seg, out));
    EXPECT_EQ(kScopeArmSetFrequency, out.failed);
    EXPECT_EQ(-705, out.code);
    EXPECT_STREQ("standby;setFrequency(213.000);", radio.log);
  }
  {
    // A calibration call that returns a code is a refused calibration too: one reload, the
    // same arm again, and the err when that is refused as well.
    FakeArmRadio radio;
    radio.calibrate_code = -707;
    ScopeArmResult out;
    EXPECT_FALSE(armScopeSegment(radio, plan, seg, out));
    EXPECT_EQ(kScopeArmCalibrateImage, out.failed);
    EXPECT_EQ(-707, out.code);
    EXPECT_TRUE(out.reloaded);
    char expected[400];
    snprintf(expected, sizeof(expected), "%sreload;%s", calibrated, calibrated);
    EXPECT_STREQ(expected, radio.log);
  }
  {
    // The case the board showed: the command's own status says nothing (the driver reads the
    // status of the command before it) and only the device errors carry IMG_CALIB_ERR. That is
    // a calibration that failed, named as one, with the driver's own code for a chip's failure
    // to execute - and no sweep is loaded through a bad image.
    FakeArmRadio radio;
    radio.device_errors = kScopeImageCalibrationError | 0x0040;
    ScopeArmResult out;
    EXPECT_FALSE(armScopeSegment(radio, plan, seg, out));
    EXPECT_EQ(kScopeArmCalibrateImage, out.failed);
    EXPECT_EQ(kScopeArmCalibrationFailed, out.code);
    EXPECT_EQ(-707, out.code);
    EXPECT_EQ(0x0050u, out.device_errors);
    EXPECT_TRUE(out.reloaded);  // refused, reloaded, refused again: then the err
    char expected[512];
    snprintf(expected, sizeof(expected), "%sreload;%s", asked, asked);
    EXPECT_STREQ(expected, radio.log);
  }
  {
    // Any other device error alone is not a calibration failure.
    FakeArmRadio radio;
    radio.device_errors = 0x0040;
    ScopeArmResult out;
    EXPECT_TRUE(armScopeSegment(radio, plan, seg, out));
    EXPECT_EQ(0x0040u, out.device_errors);
    EXPECT_FALSE(out.reloaded);
  }
  {
    FakeArmRadio radio;
    radio.sweep_code = -14;
    ScopeArmResult out;
    EXPECT_FALSE(armScopeSegment(radio, plan, seg, out));
    EXPECT_EQ(kScopeArmSetSweep, out.failed);
    EXPECT_EQ(-14, out.code);
  }
}

TEST(Scope, ACalibrationTheChipRefusesIsAskedForOnceMoreAfterAReload) {
  // What the first board does at the second segment of every pass over 150-960 MHz (p3.5-02):
  // the calibration is refused with IMG_CALIB_ERR, and nothing but a reset of the chip and the
  // plugin uploaded again clears it. So: refused once, reload, the same arm again - and the
  // sweep loaded only after the chip accepted it.
  const ScopePlan plan = planOf(150.0, 960.0, 2048);
  ScopeSegment seg;
  ASSERT_TRUE(scopeSegment(plan, 1, seg));
  char arm[160];
  snprintf(arm, sizeof(arm), "standby;setFrequency(%.3f);calibrateImage(%.3f,%.3f);deviceErrors;",
           double(scopeSegmentCentreMhz(plan, seg)), seg.cal_lo_mhz, seg.cal_hi_mhz);
  char sweep[96];
  snprintf(sweep, sizeof(sweep), "setSweep(%.3f,%.3f,%u,%.1f);", seg.first_mhz,
           plan.step_khz / 1000.0, unsigned(seg.points), plan.rbw_khz);
  {
    FakeArmRadio radio;
    radio.refusals = 1;
    ScopeArmResult out;
    EXPECT_TRUE(armScopeSegment(radio, plan, seg, out));
    EXPECT_TRUE(out.reloaded);
    EXPECT_EQ(kScopeArmNone, out.failed);
    EXPECT_EQ(0u, out.device_errors & kScopeImageCalibrationError);
    char expected[512];
    snprintf(expected, sizeof(expected), "%sreload;%s%s", arm, arm, sweep);
    EXPECT_STREQ(expected, radio.log);
  }
  {
    // Refused twice: one reload, no third try, and the err names the calibration.
    FakeArmRadio radio;
    radio.refusals = 2;
    ScopeArmResult out;
    EXPECT_FALSE(armScopeSegment(radio, plan, seg, out));
    EXPECT_TRUE(out.reloaded);
    EXPECT_EQ(kScopeArmCalibrateImage, out.failed);
    EXPECT_EQ(kScopeArmCalibrationFailed, out.code);
    char expected[512];
    snprintf(expected, sizeof(expected), "%sreload;%s", arm, arm);
    EXPECT_STREQ(expected, radio.log);
  }
  {
    // A reload that fails is its own call in the err line.
    FakeArmRadio radio;
    radio.refusals = 1;
    radio.reload_code = -2;
    ScopeArmResult out;
    EXPECT_FALSE(armScopeSegment(radio, plan, seg, out));
    EXPECT_TRUE(out.reloaded);
    EXPECT_EQ(kScopeArmReload, out.failed);
    EXPECT_EQ(-2, out.code);
    char expected[512];
    snprintf(expected, sizeof(expected), "%sreload;", arm);
    EXPECT_STREQ(expected, radio.log);
  }
  {
    // A call that is not the calibration is not retried: a standby that fails is an err at
    // once, with no reload.
    FakeArmRadio radio;
    radio.standby_code = -707;
    ScopeArmResult out;
    EXPECT_FALSE(armScopeSegment(radio, plan, seg, out));
    EXPECT_FALSE(out.reloaded);
    EXPECT_STREQ("standby;", radio.log);
  }
}

TEST(Scope, EverySegmentOfTheWholeRangeIsCalibratedForWhereItsSweepRuns) {
  // The baseline, `scope 150 960 2048`: 26 segments, and each one's calibration begins at the
  // frequency its own first point is at - the frequency the sweep before it ended next to - and
  // never 20 MHz or more above it, which is what the chip refused.
  const ScopePlan plan = planOf(150.0, 960.0, 2048);
  ASSERT_EQ(26u, plan.segments);
  for (uint16_t i = 0; i < plan.segments; i++) {
    ScopeSegment seg;
    ASSERT_TRUE(scopeSegment(plan, i, seg));
    FakeArmRadio radio;
    ScopeArmResult out;
    ASSERT_TRUE(armScopeSegment(radio, plan, seg, out)) << "segment " << i;
    char expected[160];
    snprintf(expected, sizeof(expected),
             "standby;setFrequency(%.3f);calibrateImage(%.3f,%.3f);deviceErrors;"
             "setSweep(%.3f,%.3f,%u,%.1f);",
             double(scopeSegmentCentreMhz(plan, seg)), seg.cal_lo_mhz, seg.cal_hi_mhz,
             seg.first_mhz, plan.step_khz / 1000.0, unsigned(seg.points), plan.rbw_khz);
    EXPECT_STREQ(expected, radio.log) << "segment " << i;
    EXPECT_LT(seg.cal_lo_mhz, seg.first_mhz) << "segment " << i;
    if (i > 0) {
      ScopeSegment before;
      ASSERT_TRUE(scopeSegment(plan, i - 1, before));
      const double last_mhz = before.first_mhz + double(before.points - 1) * plan.step_khz / 1000.0;
      EXPECT_NEAR(last_mhz + plan.step_khz / 1000.0, seg.first_mhz, 1e-6) << "segment " << i;
    }
  }
}

// ----------------------------------------------------------------- the err line of a segment

TEST(Scope, ASegmentThatWouldNotArmNamesTheCallAndItsCode) {
  // The four calls of armSegment (src/roles/scanner/Radio.cpp), in the order they are made, and
  // the sweep itself: the frequency alone could not tell them apart on the first board
  // (tasks/phase-3.1/FOLLOWUPS.md, 22 October 2026), and a report from the field needs to.
  EXPECT_STREQ("standby", scopeArmCallName(kScopeArmStandby));
  EXPECT_STREQ("setFrequency", scopeArmCallName(kScopeArmSetFrequency));
  EXPECT_STREQ("calibrateImage", scopeArmCallName(kScopeArmCalibrateImage));
  EXPECT_STREQ("setSweep", scopeArmCallName(kScopeArmSetSweep));
  EXPECT_STREQ("sweep", scopeArmCallName(kScopeArmSweep));
  EXPECT_STREQ("reload", scopeArmCallName(kScopeArmReload));
  EXPECT_STREQ("", scopeArmCallName(kScopeArmNone));

  StringSink sink;
  Lines lines(sink);
  lines.err("scope setup failed (-707) in calibrateImage at 200.6000; device errors 0x0010",
            scopeArmCallName(kScopeArmCalibrateImage), -707);
  {
    const JsonDocument doc = parseOne(sink);
    EXPECT_STREQ("err", doc["t"].as<const char*>());
    EXPECT_STREQ("scope setup failed (-707) in calibrateImage at 200.6000; device errors 0x0010",
                 doc["msg"].as<const char*>());
    EXPECT_STREQ("calibrateImage", doc["call"].as<const char*>());
    EXPECT_EQ(-707, doc["code"].as<int>());
  }

  // Additive (docs/noisescope.md section 5): a reader that knows only `msg` reads the same line
  // as before, and an err with no call to name is the plain line with neither field.
  sink.clear();
  lines.err("scan timeout at 869.618", scopeArmCallName(kScopeArmNone), 0);
  const JsonDocument doc = parseOne(sink);
  EXPECT_STREQ("err", doc["t"].as<const char*>());
  EXPECT_STREQ("scan timeout at 869.618", doc["msg"].as<const char*>());
  EXPECT_TRUE(doc["call"].isNull());
  EXPECT_TRUE(doc["code"].isNull());
}

// ----------------------------------------------------------------- the aggregates

TEST(Scope, TheThreeAggregatesAreTheStrongestTheMeanAndTheLatest) {
  ScopeAccumulator acc;
  acc.reset(4);
  EXPECT_EQ(4, acc.points());
  EXPECT_EQ(0, acc.sweeps());

  // Three sweeps of four points. Point 0 gets one strong reading among weak ones, and point 3
  // never gets a reading at all. A level is -2 x dBm, so the *lowest* number is the strongest
  // signal: 200 is -100 dBm and 230 is -115 dBm.
  const uint8_t sweep1[4] = {230, 230, 220, kScopeNoReading};
  const uint8_t sweep2[4] = {200, 230, 220, kScopeNoReading};
  const uint8_t sweep3[4] = {230, 230, 210, kScopeNoReading};
  acc.add(0, sweep1, 4);
  acc.sweepDone();
  acc.add(0, sweep2, 4);
  acc.sweepDone();
  acc.add(0, sweep3, 4);
  acc.sweepDone();
  EXPECT_EQ(3, acc.sweeps());

  uint8_t out[4] = {0, 0, 0, 0};
  acc.take(kScopeAggMax, out);
  EXPECT_EQ(200, out[0]) << "a burst in one sweep of three still shows, which is what max is for";
  EXPECT_EQ(230, out[1]);
  EXPECT_EQ(210, out[2]);
  EXPECT_EQ(kScopeNoReading, out[3]) << "a point with no reading has no maximum";

  acc.take(kScopeAggLast, out);
  EXPECT_EQ(230, out[0]) << "the latest sweep, where the burst had gone";
  EXPECT_EQ(210, out[2]);
  EXPECT_EQ(kScopeNoReading, out[3]);

  acc.take(kScopeAggMean, out);
  // The mean is taken in the power domain, so one reading at -100 dBm among two at -115 lands
  // near -104.8 dBm and not near -110: level = -20 log10((10^-10 + 2 x 10^-11.5) / 3).
  const double expected = -20.0 * std::log10((std::pow(10.0, -200.0 / 20.0) +
                                              2.0 * std::pow(10.0, -230.0 / 20.0)) / 3.0);
  EXPECT_NEAR(expected, double(out[0]), 1.0);
  EXPECT_LT(out[0], 230) << "a mean in the power domain is pulled up by the burst";
  EXPECT_EQ(230, out[1]) << "three equal readings average to themselves";
  EXPECT_EQ(kScopeNoReading, out[3]) << "and a point with no reading has no mean either";
}

TEST(Scope, AMeanIsOfTheReadingsAPointGotAndNotOfTheSweepsThatHappened) {
  // A segment that failed must not pull its points down: the sweep still counted, but those
  // points got nothing, and the mean of the readings that did arrive is the honest answer.
  ScopeAccumulator acc;
  acc.reset(4);
  const uint8_t two[2] = {220, 220};
  acc.add(0, two, 2);
  acc.add(2, two, 2);
  acc.sweepDone();
  acc.add(0, two, 2);  // the second segment failed this time round
  acc.sweepDone();

  uint8_t out[4] = {0, 0, 0, 0};
  acc.take(kScopeAggMean, out);
  for (int i = 0; i < 4; i++) EXPECT_EQ(220, out[i]) << "point " << i;
  EXPECT_EQ(2, acc.sweeps());
}

TEST(Scope, ASegmentIsAddedWhereItBelongsAndNeverPastTheEnd) {
  ScopeAccumulator acc;
  acc.reset(5);
  const uint8_t two[2] = {100, 101};
  acc.add(3, two, 2);
  acc.add(4, two, 2);  // one point past the end: the second byte is dropped, not written
  uint8_t out[5] = {0, 0, 0, 0, 0};
  acc.take(kScopeAggLast, out);
  EXPECT_EQ(kScopeNoReading, out[0]);
  EXPECT_EQ(kScopeNoReading, out[2]);
  EXPECT_EQ(100, out[3]);
  EXPECT_EQ(100, out[4]) << "the point at the end got the second segment's first byte";
}

TEST(Scope, TheSweepRateIsTenTimesWhatItSays) {
  EXPECT_EQ(0, scopeSweepsPerSecondX10(10, 0)) << "no time has passed, so nothing is known yet";
  EXPECT_EQ(100, scopeSweepsPerSecondX10(1, 100)) << "one sweep in 100 ms is 10.0 a second";
  EXPECT_EQ(476, scopeSweepsPerSecondX10(476, 10000));
  EXPECT_EQ(0xFFFF, scopeSweepsPerSecondX10(1000000, 1)) << "and it never wraps";
}

// ----------------------------------------------------------------- the plugin

TEST(Scope, ThePluginBinaryIsTheOneTheForkAssembled) {
  // The 167 words of src/roles/scanner/vendor/sx126x_spectrum/SX126xSpectrumPlugin.h are a
  // program for the **radio's** core, assembled from spectrum.asm beside it with the sxplug
  // assembler of sleeply4cat/MeshCore (branch mesh-pocket-spectrum, commit
  // f40f469d1e00eb8ae1b8ca07390dfc96a33ab707); the header itself is that fork's branch
  // spectrum-stream at 41472f9734cecdfa5c795eddedd6b15e2553b6b3. The assembler is not vendored
  // and nothing here rebuilds the binary, so this is the pin: a word changed by hand fails it.
  //
  // FNV-1a/64 over the words big-endian, which is the order uploadPatch() writes them to the
  // chip (RadioLib 7.8.0, SX126x::uploadPatch). Reproduce it, and the SHA-256 below, with:
  //
  //   python -c "import re,struct,hashlib;w=[int(x,16) for x in re.findall(r'0x([0-9a-f]{6}),',
  //   open('src/roles/scanner/vendor/sx126x_spectrum/SX126xSpectrumPlugin.h').read())];
  //   b=b''.join(struct.pack('>I',v) for v in w);print(len(w),hashlib.sha256(b).hexdigest())"
  //
  //   167 c0030c0a8748d4312d2b8ae8d1c5482a5eb5773a8bfa239492dceb724b01435f
  const size_t words = sizeof(SX126X_SPECTRUM_PLUGIN) / sizeof(SX126X_SPECTRUM_PLUGIN[0]);
  ASSERT_EQ(167u, words) << "the plugin is 167 words of the radio's own instruction set";

  uint64_t hash = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < words; i++) {
    const uint32_t word = SX126X_SPECTRUM_PLUGIN[i];
    ASSERT_EQ(0u, word & 0xFF000000u) << "word " << i << " is wider than the core's 24 bits";
    for (int shift = 24; shift >= 0; shift -= 8) {
      hash ^= uint64_t((word >> shift) & 0xFFu);
      hash *= 0x100000001b3ull;
    }
  }
  EXPECT_EQ(0x5fbe6ee8bea43853ull, hash)
      << "the sweep plugin's binary is not the one the fork assembled from spectrum.asm";

  // And the tag it writes into the ROM string once it is loaded, which is what says it is still
  // there before every sweep (spectrum.asm: SX_TAG = 'P').
  EXPECT_EQ('P', SX126X_SPECTRUM_PLUGIN_TAG);
}

// ================================================================= 5. the loop watchdog

// src/roles/scanner/LoopWatch.h: the 8 s check, as arithmetic. On the board `beat()` is the top
// of loop() and `tick()` is an RTC interrupt every 125 ms whose `true` is a reset
// (src/roles/scanner/LoopWdt.h); here the cadence is a for-loop, which is the only way *when* it
// fires can be stated at all.

namespace {

// Ticks until the watch says the loop has stopped, or `limit` if it never does.
int ticksUntilFired(LoopWatch& watch, int limit) {
  for (int i = 1; i <= limit; i++) {
    if (watch.tick()) return i;
  }
  return limit;
}

}  // namespace

TEST(LoopWatch, AHeartbeatEveryTickIsNeverAReset) {
  LoopWatch watch(8000, 125);
  for (int i = 0; i < 1000; i++) {  // two minutes of a loop that is coming round
    watch.beat();
    ASSERT_FALSE(watch.tick()) << "tick " << i;
  }
  EXPECT_EQ(0u, watch.silentMs());
  EXPECT_FALSE(watch.fired());
}

TEST(LoopWatch, AHeartbeatEveryFewTicksIsNeverAResetEither) {
  // The loop is not expected at the cadence of the check: a 2048-sample `hist` step is tens of
  // milliseconds and a `scope` segment is one pass of the loop, so several ticks may pass between
  // two heartbeats. What matters is that one heartbeat clears the whole count.
  LoopWatch watch(8000, 125);
  for (int i = 0; i < 400; i++) {
    const bool fired = watch.tick();
    ASSERT_FALSE(fired) << "tick " << i;
    if (i % 60 == 0) watch.beat();  // a heartbeat every 7.5 s, inside the deadline
  }
}

TEST(LoopWatch, TheDeadlineIsReachedAfterTheDeadlineAndNotBefore) {
  LoopWatch watch(8000, 125);
  watch.beat();
  EXPECT_FALSE(watch.tick()) << "the heartbeat is taken by the first tick";
  // 8000 ms of silence at 125 ms a tick is 64 ticks, and the 64th is the one that says so.
  EXPECT_EQ(64, ticksUntilFired(watch, 200));
  EXPECT_EQ(8000u, watch.silentMs());
  EXPECT_TRUE(watch.fired());
}

TEST(LoopWatch, ACadenceThatDoesNotDivideTheDeadlineOvershootsAndNeverUndershoots) {
  // The resolution of the answer is one tick: a deadline is reached between `deadline` and
  // `deadline + tick` of silence. Never before - a reset that should not have happened is the one
  // failure a watchdog must not have.
  LoopWatch watch(8000, 300);
  EXPECT_EQ(27, ticksUntilFired(watch, 200)) << "26 ticks is 7800 ms, which is not yet 8000";
  EXPECT_EQ(8100u, watch.silentMs());
}

TEST(LoopWatch, ItSaysSoOnceAndThenKeepsQuiet) {
  // The reset is already on its way; a second `true` would only be a second reset, and on the
  // board it would be a second NVIC_SystemReset from inside an interrupt.
  LoopWatch watch(1000, 125);
  EXPECT_EQ(8, ticksUntilFired(watch, 100));
  for (int i = 0; i < 100; i++) {
    ASSERT_FALSE(watch.tick()) << "tick " << i << " after it had already fired";
  }
  EXPECT_TRUE(watch.fired());
}

TEST(LoopWatch, AHeartbeatAfterItHasFiredDoesNotUnfireIt) {
  LoopWatch watch(1000, 125);
  ticksUntilFired(watch, 100);
  watch.beat();
  EXPECT_FALSE(watch.tick());
  EXPECT_TRUE(watch.fired()) << "the part is going; nothing takes that back";
}

TEST(LoopWatch, ADeadlineOfZeroIsTheCheckTurnedOff) {
  // What a build that wanted the hardware watchdog alone would ask for
  // (src/roles/scanner/config.h, LOOP_WDT_TIMEOUT_MS).
  LoopWatch watch(0, 125);
  for (int i = 0; i < 10000; i++) {
    ASSERT_FALSE(watch.tick()) << "tick " << i;
  }
  EXPECT_FALSE(watch.fired());
  EXPECT_EQ(0u, watch.deadlineMs());
}

TEST(LoopWatch, ACadenceOfZeroIsOneMillisecondAndNotADeadlineThatNeverGrows) {
  // The constructor runs before main() on the board and has nowhere to refuse, so it clamps.
  LoopWatch watch(10, 0);
  EXPECT_EQ(1u, watch.tickMs());
  EXPECT_EQ(10, ticksUntilFired(watch, 100));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
