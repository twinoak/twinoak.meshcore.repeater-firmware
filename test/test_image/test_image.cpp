// What an image says it is, and that everything which says it says the same thing.
//
// Three subjects, which are the three of tasks/phase-3/01-roles-and-boards.md:
//
//   1. **the descriptor** (src/common/fwdesc/Descriptor.h) against the specification's own
//      fixtures - `spec/fixtures/manifest/index.json`, written by spec/tools/manifestref.py from
//      spec/FIRMWARE-MANIFEST.md section 4: every `descriptors[*]` entry built from its `fields`
//      and compared with its `hex`, byte for byte, and read back field for field. Plus the
//      refusals: a version, board or band that does not fit its field with a terminating zero,
//      and a blob whose magic, length, CRC or terminator is wrong.
//
//   2. **the names**: role, board, band and the environment's own name, held across
//      platformio.ini, the box link's `HELLO` and the manifest entry. platformio.ini keeps one
//      `[img_*]` section per image and that is the only place any of the three is written; this
//      suite reads that file and checks each consumer against it, so a name that drifts is a red
//      test. The manifest entry's half of the same rule is in tools/release/test_release.py,
//      which reads the same sections.
//
//   3. **the band check** (src/common/bands/Bands.h): the table, the lookup and the refusal of a
//      frequency outside the band an image says it is in - with a made-up band, because 433 has
//      no block yet and the mechanism is what is being tested. On a board build the same table is
//      a static_assert (src/common/bands/BandCheck.cpp), which cannot be tested from here: a
//      build that fails to compile has no test to run.
//
// PROJECT_DIR and MANIFEST_FIXTURES_DIR are platformio.ini's ([env:native]).

#include <gtest/gtest.h>

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "common/bands/Bands.h"
#include "common/board/Boards.h"
#include "common/fwdesc/Descriptor.h"
#include "common/fwdesc/ImageDescriptor.h"
#include "common/link/codec/Numbers.h"
#include "common/version/Version.h"

#ifndef PROJECT_DIR
#error "PROJECT_DIR must be defined; platformio.ini's [env:native] does it"
#endif
#ifndef MANIFEST_FIXTURES_DIR
#error "MANIFEST_FIXTURES_DIR must be defined; platformio.ini's [env:native] does it"
#endif

namespace {

namespace fwdesc = twinoak::fwdesc;

// ----------------------------------------------------------------------------- bytes and hex

std::string toHex(const uint8_t* data, size_t len) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out += digits[data[i] >> 4];
    out += digits[data[i] & 0x0F];
  }
  return out;
}

std::vector<uint8_t> fromHex(const std::string& s) {
  std::vector<uint8_t> out;
  out.reserve(s.size() / 2);
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    out.push_back(uint8_t(std::stoul(s.substr(i, 2), nullptr, 16)));
  }
  return out;
}

std::string fileText(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.good()) << "cannot read " << path;
  std::stringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

// ----------------------------------------------------------------------------- the fixtures

JsonDocument& fixtureIndex() {
  static JsonDocument doc;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    const std::string path = std::string(MANIFEST_FIXTURES_DIR) + "/index.json";
    const std::string text = fileText(path);
    const DeserializationError err = deserializeJson(doc, text);
    EXPECT_TRUE(err == DeserializationError::Ok) << path << ": " << err.c_str();
  }
  return doc;
}

// ----------------------------------------------------------------------------- platformio.ini

std::string trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  const size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

// A line of platformio.ini without its comment: a line starting with ';' or '#' is all comment,
// and so is everything from a ';' or '#' that follows whitespace (test_rails does the same).
std::string stripComment(const std::string& raw) {
  const std::string line = trim(raw);
  if (line.empty() || line[0] == ';' || line[0] == '#') return "";
  for (size_t i = 1; i < line.size(); i++) {
    if ((line[i] == ';' || line[i] == '#') && (line[i - 1] == ' ' || line[i - 1] == '\t')) {
      return trim(line.substr(0, i));
    }
  }
  return line;
}

// platformio.ini as sections: the section name to its lines, comments removed, continuation
// lines kept as they are (a build_flags block is one key with many lines).
std::map<std::string, std::vector<std::string> > sections() {
  static std::map<std::string, std::vector<std::string> > out;
  if (!out.empty()) return out;
  std::istringstream in(fileText(std::string(PROJECT_DIR) + "/platformio.ini"));
  std::string raw;
  std::string current;
  while (std::getline(in, raw)) {
    const std::string line = stripComment(raw);
    if (line.empty()) continue;
    if (line[0] == '[' && line[line.size() - 1] == ']') {
      current = line.substr(1, line.size() - 2);
      out[current];  // a section with no lines is still a section
      continue;
    }
    if (!current.empty()) out[current].push_back(line);
  }
  EXPECT_FALSE(out.empty()) << "platformio.ini has no sections - is PROJECT_DIR right?";
  return out;
}

// The value of `key = value` in a section, or "" when it has none.
std::string value(const std::string& section, const std::string& key) {
  const std::map<std::string, std::vector<std::string> >& all = sections();
  const std::map<std::string, std::vector<std::string> >::const_iterator it = all.find(section);
  if (it == all.end()) return "";
  for (size_t i = 0; i < it->second.size(); i++) {
    const std::string& line = it->second[i];
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    if (trim(line.substr(0, eq)) == key) return trim(line.substr(eq + 1));
  }
  return "";
}

// One image of platformio.ini: an [img_*] section.
struct Image {
  std::string section;  // "img_repeater_rak4630_single"
  std::string role;
  std::string board;
  std::string band;
};

std::vector<Image> images() {
  std::vector<Image> out;
  const std::map<std::string, std::vector<std::string> >& all = sections();
  for (std::map<std::string, std::vector<std::string> >::const_iterator it = all.begin();
       it != all.end(); ++it) {
    if (it->first.compare(0, 4, "img_") != 0) continue;
    Image image;
    image.section = it->first;
    image.role = value(it->first, "role");
    image.board = value(it->first, "board");
    image.band = value(it->first, "band");
    out.push_back(image);
  }
  return out;
}

std::vector<std::string> boardEnvironments() {
  std::vector<std::string> out;
  const std::map<std::string, std::vector<std::string> >& all = sections();
  for (std::map<std::string, std::vector<std::string> >::const_iterator it = all.begin();
       it != all.end(); ++it) {
    if (it->first.compare(0, 4, "env:") != 0) continue;
    const std::string name = it->first.substr(4);
    if (name != "native") out.push_back(name);
  }
  return out;
}

std::string underscored(const std::string& board) {
  std::string out = board;
  for (size_t i = 0; i < out.size(); i++) {
    if (out[i] == '-') out[i] = '_';
  }
  return out;
}

// Every line of a section, joined: enough to look for one flag in a build_flags block.
std::string sectionText(const std::string& section) {
  const std::map<std::string, std::vector<std::string> >& all = sections();
  const std::map<std::string, std::vector<std::string> >::const_iterator it = all.find(section);
  if (it == all.end()) return "";
  std::string out;
  for (size_t i = 0; i < it->second.size(); i++) out += it->second[i] + "\n";
  return out;
}

}  // namespace

// ================================================================= 1. the descriptor

// Every `descriptors[*]` entry of the specification's fixtures that is a valid descriptor:
// built from its `fields` and compared with its `hex`, byte for byte.
TEST(Descriptor, IsTheSpecificationsBytesForEveryFixture) {
  JsonArray entries = fixtureIndex()["descriptors"].as<JsonArray>();
  ASSERT_FALSE(entries.isNull()) << "spec/fixtures/manifest/index.json has no descriptors";
  int checked = 0;
  for (JsonObject entry : entries) {
    if (!entry["valid"].as<bool>()) continue;
    JsonObject fields = entry["fields"].as<JsonObject>();
    ASSERT_FALSE(fields.isNull()) << entry["image"].as<const char*>();
    const std::string role = fields["role"].as<std::string>();
    const std::string version = fields["version"].as<std::string>();
    const std::string board = fields["board"].as<std::string>();
    const std::string band = fields["band"].as<std::string>();

    uint8_t blob[fwdesc::kLength] = {0};
    ASSERT_TRUE(fwdesc::build(blob, sizeof(blob), fwdesc::roleCode(role.c_str()),
                              uint8_t(fields["slot"].as<unsigned>()),
                              uint8_t(fields["link_major"].as<unsigned>()),
                              uint8_t(fields["link_minor"].as<unsigned>()), version.c_str(),
                              board.c_str(), band.c_str()))
        << entry["image"].as<const char*>();
    EXPECT_EQ(entry["hex"].as<std::string>(), toHex(blob, sizeof(blob)))
        << entry["image"].as<const char*>()
        << ": the bytes this firmware builds are not the specification's"
           " (spec/FIRMWARE-MANIFEST.md section 4)";

    // And read back: field for field, the fixture's own.
    fwdesc::Fields read;
    ASSERT_TRUE(fwdesc::parse(blob, sizeof(blob), read));
    EXPECT_EQ(fwdesc::roleCode(role.c_str()), read.role);
    EXPECT_EQ(fields["slot"].as<unsigned>(), read.slot);
    EXPECT_EQ(fields["link_major"].as<unsigned>(), read.link_major);
    EXPECT_EQ(fields["link_minor"].as<unsigned>(), read.link_minor);
    EXPECT_EQ(fields["flags"].as<unsigned>(), read.flags);
    EXPECT_EQ(version, std::string(read.version));
    EXPECT_EQ(board, std::string(read.board));
    EXPECT_EQ(band, std::string(read.band));
    checked++;
  }
  EXPECT_GE(checked, 2) << "the fixtures carry fewer valid descriptors than they used to";
}

// The specification's own invalid images, read out of the fixture directory: two descriptors and
// a broken CRC. Finding a descriptor *in an image* is the release tooling's and the Walter's
// (spec/tools/manifestref.py find_descriptor); what is held here is that the parser refuses the
// bytes of the broken one.
TEST(Descriptor, RefusesTheFixturesBrokenOne) {
  const std::string path = std::string(MANIFEST_FIXTURES_DIR) + "/image-broken-descriptor.bin";
  const std::string image = fileText(path);
  const std::string magic(fwdesc::kMagic);
  const size_t at = image.find(magic);
  ASSERT_NE(std::string::npos, at) << path << " has no descriptor magic in it at all";
  fwdesc::Fields read;
  EXPECT_FALSE(fwdesc::parse(reinterpret_cast<const uint8_t*>(image.data()) + at,
                             fwdesc::kLength, read))
      << "the fixture's deliberately broken descriptor was accepted";
}

TEST(Descriptor, RefusesAStringThatDoesNotFitItsField) {
  uint8_t blob[fwdesc::kLength] = {0};
  const std::string version32(fwdesc::kVersionSize, 'v');  // 32 characters: no room for the zero
  const std::string board24(fwdesc::kBoardSize, 'b');
  const std::string band8(fwdesc::kBandSize, '8');
  EXPECT_FALSE(fwdesc::build(blob, sizeof(blob), fwdesc::kRoleRepeater, fwdesc::kSlotAny, 1, 0,
                             version32.c_str(), "rak4630-single", "868"));
  EXPECT_FALSE(fwdesc::build(blob, sizeof(blob), fwdesc::kRoleRepeater, fwdesc::kSlotAny, 1, 0,
                             "0.3.0+abc1234", board24.c_str(), "868"));
  EXPECT_FALSE(fwdesc::build(blob, sizeof(blob), fwdesc::kRoleRepeater, fwdesc::kSlotAny, 1, 0,
                             "0.3.0+abc1234", "rak4630-single", band8.c_str()));
  // One character shorter is the longest that fits.
  EXPECT_TRUE(fwdesc::build(blob, sizeof(blob), fwdesc::kRoleRepeater, fwdesc::kSlotAny, 1, 0,
                            version32.substr(1).c_str(), board24.substr(1).c_str(),
                            band8.substr(1).c_str()));
}

TEST(Descriptor, RefusesABufferOfTheWrongSize) {
  uint8_t blob[fwdesc::kLength + 1] = {0};
  EXPECT_FALSE(fwdesc::build(blob, fwdesc::kLength - 1, fwdesc::kRoleRepeater, fwdesc::kSlotAny,
                             1, 0, "0.3.0", "rak4630-single", "868"));
  EXPECT_FALSE(fwdesc::build(blob, sizeof(blob), fwdesc::kRoleRepeater, fwdesc::kSlotAny, 1, 0,
                             "0.3.0", "rak4630-single", "868"));
}

TEST(Descriptor, RefusesAWrongMagicLengthCrcOrTerminator) {
  uint8_t good[fwdesc::kLength] = {0};
  ASSERT_TRUE(fwdesc::build(good, sizeof(good), fwdesc::kRoleRepeater, fwdesc::kSlotAny, 1, 0,
                            "0.3.0+abc1234", "rak4630-single", "868"));
  fwdesc::Fields read;
  ASSERT_TRUE(fwdesc::parse(good, sizeof(good), read));

  uint8_t blob[fwdesc::kLength];
  memcpy(blob, good, sizeof(blob));
  blob[0] = 'X';
  EXPECT_FALSE(fwdesc::parse(blob, sizeof(blob), read)) << "a wrong magic was accepted";

  memcpy(blob, good, sizeof(blob));
  blob[8] = 95;
  EXPECT_FALSE(fwdesc::parse(blob, sizeof(blob), read)) << "a wrong length was accepted";

  memcpy(blob, good, sizeof(blob));
  blob[20] ^= 0x01;  // one character of the version
  EXPECT_FALSE(fwdesc::parse(blob, sizeof(blob), read)) << "a failing crc was accepted";

  // A field with no terminating zero at all: the CRC is made right again so that only the
  // missing terminator is left to refuse it.
  memcpy(blob, good, sizeof(blob));
  for (size_t i = 48; i < 72; i++) blob[i] = 'b';
  const uint32_t crc = fwdesc::crc32(blob, fwdesc::kBodyLength);
  for (size_t i = 0; i < 4; i++) blob[92 + i] = uint8_t((crc >> (8 * i)) & 0xFF);
  EXPECT_FALSE(fwdesc::parse(blob, sizeof(blob), read))
      << "a board field with no terminating zero was accepted"
         " (spec/FIRMWARE-MANIFEST.md section 4: at least one)";
}

TEST(Descriptor, TheCrcIsZlibsOverTheFirstNinetyTwoBytes) {
  // The one value this firmware and spec/tools/manifestref.py must agree on, out of the
  // fixtures: the last four bytes of a fixture's hex are the CRC of the first ninety-two.
  JsonArray entries = fixtureIndex()["descriptors"].as<JsonArray>();
  for (JsonObject entry : entries) {
    if (!entry["valid"].as<bool>()) continue;
    const std::vector<uint8_t> blob = fromHex(entry["hex"].as<std::string>());
    ASSERT_EQ(fwdesc::kLength, blob.size());
    const uint32_t want = uint32_t(blob[92]) | (uint32_t(blob[93]) << 8) |
                          (uint32_t(blob[94]) << 16) | (uint32_t(blob[95]) << 24);
    EXPECT_EQ(want, fwdesc::crc32(blob.data(), fwdesc::kBodyLength))
        << entry["image"].as<const char*>();
  }
}

TEST(Descriptor, TheRoleCodesAreTheSpecifications) {
  EXPECT_EQ(0u, fwdesc::roleCode("something else"));
  EXPECT_EQ(1u, fwdesc::roleCode("repeater"));
  EXPECT_EQ(2u, fwdesc::roleCode("bridge"));
  EXPECT_EQ(3u, fwdesc::roleCode("scanner"));
  EXPECT_EQ(16u, fwdesc::roleCode("manager"));
  // And they are the box link's own HELLO codes, which is why `HELLO`'s `role` can be read
  // straight out of the descriptor (spec/LINK-PROTOCOL.md 5.1).
  EXPECT_EQ(unsigned(twinoak::link::kRoleUnknown), unsigned(fwdesc::kRoleUnknown));
  EXPECT_EQ(unsigned(twinoak::link::kRoleRepeater), unsigned(fwdesc::kRoleRepeater));
  EXPECT_EQ(unsigned(twinoak::link::kRoleBridge), unsigned(fwdesc::kRoleBridge));
  EXPECT_EQ(unsigned(twinoak::link::kRoleScanner), unsigned(fwdesc::kRoleScanner));
}

TEST(Descriptor, TheRoleNameOfACodeIsTheNameThatMadeIt) {
  // The scanner prints its own `role` out of its descriptor's role byte rather than out of the
  // define that made it (src/roles/scanner/main.cpp), so the two directions have to agree.
  for (const char* role : {"repeater", "bridge", "scanner", "manager"}) {
    EXPECT_STREQ(role, fwdesc::roleName(fwdesc::roleCode(role)));
  }
  EXPECT_STREQ("unknown", fwdesc::roleName(fwdesc::kRoleUnknown));
  EXPECT_STREQ("unknown", fwdesc::roleName(99)) << "a code with no name is not given one";
  EXPECT_EQ(unsigned(fwdesc::kRoleUnknown), unsigned(fwdesc::roleCode(fwdesc::roleName(99))))
      << "and the round trip does not invent a role out of the word 'unknown'";
}

// ================================================================= 2. the names

TEST(Names, EveryBoardEnvironmentHasAnImageSectionAndIsNamedAfterIt) {
  const std::vector<std::string> envs = boardEnvironments();
  ASSERT_FALSE(envs.empty()) << "platformio.ini has no board environment";
  const std::vector<Image> all = images();
  ASSERT_EQ(envs.size(), all.size())
      << "platformio.ini has " << envs.size() << " board environment(s) and " << all.size()
      << " [img_*] section(s): an image is one role on one board in one band, and each one is"
         " exactly one environment";
  for (size_t i = 0; i < all.size(); i++) {
    const Image& image = all[i];
    EXPECT_FALSE(image.role.empty()) << "[" << image.section << "] has no role";
    EXPECT_FALSE(image.board.empty()) << "[" << image.section << "] has no board";
    EXPECT_FALSE(image.band.empty()) << "[" << image.section << "] has no band";
    const std::string env = image.role + "_" + underscored(image.board);
    EXPECT_EQ("img_" + env, image.section)
        << "an [img_*] section is named after the environment it is for, and an environment is"
           " <role>_<board with underscores> (docs/ARCHITECTURE.md section 8)";
    EXPECT_NE(sections().end(), sections().find("env:" + env))
        << "[img_" << env << "] has no [env:" << env << "] beside it";
  }
}

TEST(Names, AnEnvironmentTakesItsThreeNamesFromItsImageSectionAndNowhereElse) {
  const std::vector<Image> all = images();
  for (size_t i = 0; i < all.size(); i++) {
    const Image& image = all[i];
    const std::string env = image.role + "_" + underscored(image.board);
    const std::string text = sectionText("env:" + env);
    ASSERT_FALSE(text.empty()) << "[env:" << env << "] is empty";
    const char* const keys[] = {"role", "board", "band"};
    for (size_t k = 0; k < 3; k++) {
      const std::string want = std::string("${") + image.section + "." + keys[k] + "}";
      EXPECT_NE(std::string::npos, text.find(want))
          << "[env:" << env << "] does not take TWINOAK_" << keys[k]
          << " from " << want << ": the three names are written in one place and read from"
             " there (platformio.ini's [img_*] section)";
    }
    // And the literals are not restated in the environment.
    EXPECT_EQ(std::string::npos, text.find("'\"" + image.board + "\"'"))
        << "[env:" << env << "] writes the board name out again";
  }
}

TEST(Names, ThisBuildsOwnThreeNamesAreOneOfThoseSections) {
  // The native environment is compiled with a board environment's TWINOAK_* defines so that the
  // portable half of the firmware says here what it says on the board. That copy cannot drift:
  // it must be one of platformio.ini's own [img_*] sections.
  const std::vector<Image> all = images();
  int matches = 0;
  for (size_t i = 0; i < all.size(); i++) {
    if (all[i].role == TWINOAK_ROLE && all[i].board == TWINOAK_BOARD &&
        all[i].band == TWINOAK_BAND) {
      matches++;
    }
  }
  EXPECT_EQ(1, matches) << "the TWINOAK_ROLE/BOARD/BAND this was compiled with (" << TWINOAK_ROLE
                        << ", " << TWINOAK_BOARD << ", " << TWINOAK_BAND
                        << ") are not exactly one [img_*] section of platformio.ini";
}

TEST(Names, TheImageDescriptorOfThisBuildCarriesThem) {
  // The object the compiler built is a valid descriptor, and it says this image's three names.
  fwdesc::Fields fields;
  ASSERT_TRUE(fwdesc::parse(fwdesc::imageDescriptor(), fwdesc::kLength, fields))
      << "this build's own descriptor does not parse: magic, length, crc or a terminator";
  EXPECT_EQ(unsigned(fwdesc::roleCode(TWINOAK_ROLE)), unsigned(fields.role));
  EXPECT_EQ(std::string(TWINOAK_BOARD), std::string(fields.board));
  EXPECT_EQ(std::string(TWINOAK_BAND), std::string(fields.band));
  EXPECT_EQ(std::string(TWINOAK_FW_VERSION), std::string(fields.version));
  // The box link it speaks, which is also the manifest's `link` and its `requires.link_major`.
  EXPECT_EQ(unsigned(twinoak::link::kProtoMajor), unsigned(fields.link_major));
  EXPECT_EQ(unsigned(twinoak::link::kProtoMinor), unsigned(fields.link_minor));
  EXPECT_EQ(0u, fields.flags);
  // And the readers the firmware's HELLO uses - a byte and three pointers into those bytes -
  // say the same, without parsing anything.
  EXPECT_EQ(unsigned(fields.role), unsigned(fwdesc::imageRole()));
  EXPECT_EQ(std::string(fields.version), std::string(fwdesc::imageVersion()));
  EXPECT_EQ(std::string(fields.board), std::string(fwdesc::imageBoard()));
  EXPECT_EQ(std::string(fields.band), std::string(fwdesc::imageBand()));
  EXPECT_EQ(unsigned(fields.slot), unsigned(fwdesc::imageDescriptorSlot()));
  // The bytes in the image are the bytes the portable builder makes.
  uint8_t built[fwdesc::kLength] = {0};
  ASSERT_TRUE(fwdesc::build(built, sizeof(built), fwdesc::roleCode(TWINOAK_ROLE), fields.slot,
                            twinoak::link::kProtoMajor, twinoak::link::kProtoMinor,
                            TWINOAK_FW_VERSION, TWINOAK_BOARD, TWINOAK_BAND));
  EXPECT_EQ(toHex(built, sizeof(built)), toHex(fwdesc::imageDescriptor(), fwdesc::kLength))
      << "the const object the compiler built is not what the portable builder builds";
}

TEST(Names, HelloSaysTheBoardsSlotAndTheDescriptorSaysWhichPositionsTheImageIsFor) {
  // What `HELLO` reports is the radio position this unit *is* - 0 on a single-radio board - and
  // what the descriptor says is which positions the image may go in (0xFF, any)
  // (spec/LINK-PROTOCOL.md 5.1, spec/FIRMWARE-MANIFEST.md section 4). Both come from the board
  // and from nothing else.
  using namespace twinoak::board;  // NOLINT: a test of this table
  EXPECT_EQ(0u, unsigned(slotOf(kBoards, kBoardCount, "rak4630-single")));
  EXPECT_EQ(unsigned(kSlotAny), unsigned(descriptorSlotOf(kBoards, kBoardCount, "rak4630-single")));
  EXPECT_EQ(unsigned(slotOf(kBoards, kBoardCount, TWINOAK_BOARD)),
            unsigned(twinoak::fwdesc::imageSlot()));
  EXPECT_EQ(unsigned(descriptorSlotOf(kBoards, kBoardCount, TWINOAK_BOARD)),
            unsigned(twinoak::fwdesc::imageDescriptorSlot()));
}

TEST(Names, EveryBoardOfPlatformioIniHasARowInTheBoardTable) {
  using namespace twinoak::board;  // NOLINT
  const std::vector<Image> all = images();
  for (size_t i = 0; i < all.size(); i++) {
    EXPECT_LE(0, indexOf(kBoards, kBoardCount, all[i].board.c_str()))
        << all[i].board << " has no row in src/common/board/Boards.h: a board is a target and a"
                           " table row (docs/ARCHITECTURE.md section 8)";
  }
  // A board nobody has added is not quietly slot 0.
  EXPECT_EQ(-1, indexOf(kBoards, kBoardCount, "rak4630-dual-a"));
}

TEST(Names, TheVersionStringSaysTheRole) {
  // `sensor get fw.version` and the console's own version line
  // (docs/ARCHITECTURE.md section 4): "twinoak-<role> ...".
  const std::string said = twinoak::versionString();
  EXPECT_EQ(0u, said.find(std::string("twinoak-") + TWINOAK_ROLE + " "))
      << "the version string does not begin with this image's role: " << said;
}

TEST(Names, TheManifestsImageIdIsRoleBoardBand) {
  // The release tooling composes it from buildinfo.json, which tools/pio/buildinfo.py writes
  // from the same three defines; tools/release/test_release.py runs that path over every [img_*]
  // section of platformio.ini. What is held here is the two places this repository's own scripts
  // read the three names by name, so a rename cannot leave one of them behind.
  const std::string buildinfo = fileText(std::string(PROJECT_DIR) + "/tools/pio/buildinfo.py");
  for (const char* define : {"TWINOAK_ROLE", "TWINOAK_BOARD", "TWINOAK_BAND"}) {
    EXPECT_NE(std::string::npos, buildinfo.find(define))
        << "tools/pio/buildinfo.py no longer reads " << define
        << ", so the manifest entry would not be this image's";
  }
  const std::string release = fileText(std::string(PROJECT_DIR) +
                                       "/tools/release/build_release.py");
  EXPECT_NE(std::string::npos,
            release.find("\"%s.%s.%s\" % (info[\"role\"], info[\"board\"], info[\"band\"])"))
      << "tools/release/build_release.py no longer composes the image id as"
         " <role>.<board>.<band> (spec/FIRMWARE-MANIFEST.md section 2)";
}

// ================================================================= 3. the band check

TEST(Bands, TheTableHoldsTheBandThisBuildSaysItIsIn) {
  using namespace twinoak::bands;  // NOLINT: a test of this table
  EXPECT_TRUE(known(kBands, kBandCount, TWINOAK_BAND))
      << TWINOAK_BAND << " has no row in src/common/bands/Bands.h";
  // The frequency itself is a board build's define; what is held here is the band's own range.
  EXPECT_TRUE(holds(kBands, kBandCount, "868", 869.618));
  EXPECT_TRUE(holds(kBands, kBandCount, "868", 863.0));
  EXPECT_TRUE(holds(kBands, kBandCount, "868", 870.0));
  EXPECT_FALSE(holds(kBands, kBandCount, "868", 862.9));
  EXPECT_FALSE(holds(kBands, kBandCount, "868", 870.1));
  EXPECT_FALSE(holds(kBands, kBandCount, "868", 433.5));
}

TEST(Bands, AnUnknownBandIsNeverHeldToBeRight) {
  using namespace twinoak::bands;  // NOLINT
  EXPECT_FALSE(known(kBands, kBandCount, "433"))
      << "433 has a row now: it needs a [band_433] block in platformio.ini and a radio measured"
         " against it";
  EXPECT_FALSE(holds(kBands, kBandCount, "433", 433.5))
      << "a band the table does not know was accepted, so a new band could ship by being named";
}

// The mechanism, with a band nobody has: this is what adding a row does, and it is a constant
// expression, which is what makes src/common/bands/BandCheck.cpp's static_assert possible.
TEST(Bands, AMadeUpBandBehavesLikeARealOne) {
  using namespace twinoak::bands;  // NOLINT
  static constexpr Band kMadeUp[] = {{"868", 863.0, 870.0}, {"915", 902.0, 928.0}};
  static constexpr size_t kCount = sizeof(kMadeUp) / sizeof(kMadeUp[0]);

  static_assert(known(kMadeUp, kCount, "915"), "a row added to the table is found at compile time");
  static_assert(holds(kMadeUp, kCount, "915", 915.0), "and its range holds at compile time");
  static_assert(!holds(kMadeUp, kCount, "915", 869.618),
                "and a frequency of another band does not");
  static_assert(indexOf(kMadeUp, kCount, "868") == 0, "the first row is still the first row");
  static_assert(indexOf(kMadeUp, kCount, "433") < 0, "and a band with no row has no index");

  EXPECT_TRUE(holds(kMadeUp, kCount, "915", 902.0));
  EXPECT_TRUE(holds(kMadeUp, kCount, "915", 928.0));
  EXPECT_FALSE(holds(kMadeUp, kCount, "915", 901.9));
  EXPECT_FALSE(holds(kMadeUp, kCount, "868", 915.0));
}

TEST(Bands, ABandsOwnEndsAreReadableAndABandWithNoRowHasNone) {
  using namespace twinoak::bands;  // NOLINT
  // The scanner's default sweep is the band's range and is written nowhere else
  // (src/roles/scanner/config.h), so the table has to be able to say what that range is.
  EXPECT_DOUBLE_EQ(863.0, lowOf(kBands, kBandCount, "868"));
  EXPECT_DOUBLE_EQ(870.0, highOf(kBands, kBandCount, "868"));
  EXPECT_DOUBLE_EQ(0.0, lowOf(kBands, kBandCount, "433"))
      << "a band the table does not know has no range, and 0 MHz is no frequency at all";
  EXPECT_DOUBLE_EQ(0.0, highOf(kBands, kBandCount, "433"));
  // At compile time too, which is what lets a build take its sweep from it.
  static constexpr Band kMadeUp[] = {{"915", 902.0, 928.0}};
  static_assert(lowOf(kMadeUp, 1, "915") == 902.0, "a band's low end is a constant expression");
  static_assert(highOf(kMadeUp, 1, "915") == 928.0, "and so is its high end");
}

TEST(Bands, EveryBandOfPlatformioIniHasARowAndItsDefaultFrequencyFitsIt) {
  using namespace twinoak::bands;  // NOLINT
  const std::vector<Image> all = images();
  ASSERT_FALSE(all.empty());
  for (size_t i = 0; i < all.size(); i++) {
    const std::string band = all[i].band;
    EXPECT_TRUE(known(kBands, kBandCount, band.c_str()))
        << band << " is an image's band in platformio.ini but has no row in"
                   " src/common/bands/Bands.h";
    const std::string freq = value("band_" + band, "freq");
    ASSERT_FALSE(freq.empty()) << "platformio.ini has no [band_" << band << "] with a freq";
    EXPECT_TRUE(holds(kBands, kBandCount, band.c_str(), std::stod(freq)))
        << "[band_" << band << "].freq is " << freq << ", which is outside band " << band
        << " - a board build of it does not compile (src/common/bands/BandCheck.cpp)";
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
