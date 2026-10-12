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
//
// ArduinoJson is the native environment's fixture reader already (test/test_link_codec) and it
// is here for the same reason: a line is held against a real parser and not against a string
// this test also wrote. Its default build rejects `nan` and `infinity`, which is precisely the
// refusal rule 2 is about.

#include <gtest/gtest.h>

#include <ArduinoJson.h>

#include <cmath>
#include <string>
#include <vector>

#include "roles/scanner/Commands.h"
#include "roles/scanner/Lines.h"
#include "roles/scanner/Sweep.h"

namespace {

using namespace twinoak::scanner;  // NOLINT: this is that namespace's own test

// ----------------------------------------------------------------------------- the sink

// Everything written, and the lines it came to. A fragment is not a line: the writer sends a
// message in several calls and ends it with a newline of its own, so what a reader sees is what
// this splits.
class StringSink : public LineSink {
 public:
  void write(const char* text) override { all_ += text; }

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

  void clear() { all_.clear(); }

 private:
  std::string all_;
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
      {"sweep", kVerbSweep},   {"hist", kVerbHist},   {"dwell", kVerbDwell},
      {"cad", kVerbCad},       {"stop", kVerbStop},   {"home", kVerbHome},
      {"boost", kVerbBoost},   {"auto", kVerbAuto},   {"time", kVerbTime},
      {"reboot", kVerbReboot}, {"dfu", kVerbDfu},
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

TEST(Commands, EveryCommandThatCanBeRefusedHasAUsageLine) {
  for (Verb verb : {kVerbDwell, kVerbHome, kVerbBoost, kVerbAuto, kVerbTime}) {
    const std::string usage = usageOf(verb);
    EXPECT_EQ(0u, usage.find("usage: ")) << "verb " << int(verb) << ": " << usage;
    EXPECT_NE(std::string::npos, usage.find(nameOf(verb)))
        << "a usage line names its own command: " << usage;
  }
  for (Verb verb : {kVerbHelp, kVerbInfo, kVerbStat, kVerbStop, kVerbSweep, kVerbHist, kVerbCad,
                    kVerbReboot, kVerbDfu}) {
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
  EXPECT_TRUE(doc["radio_ok"].is<bool>());
  EXPECT_TRUE(doc["radio_ok"].as<bool>());
  EXPECT_EQ(1, doc["rtc"].as<int>());
  EXPECT_EQ(0, doc["ina"].as<int>());
  EXPECT_EQ(1754820000u, doc["ts"].as<uint32_t>());
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

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
