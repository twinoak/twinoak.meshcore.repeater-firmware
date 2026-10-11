// The rails, as a test and not a promise (README.md "Rails that hold in every task",
// spec/SYSTEM.md S-12 and S-13, tasks/phase-1/01): it reads platformio.ini and walks src/ and
// fails on
//   - the packet-logging define or the mesh-debug define set anywhere in platformio.ini (in a
//     flag, not in a comment), or mentioned at all in a file of src/ - which also forbids
//     upstream's debug-print macro in our sources, on purpose: nothing the mesh says is logged as
//     text;
//   - a line of src/ that prints a packet's payload or an advert's application data - any line
//     that names `payload` or `app_data` beside a print-like call. The second is section 7's
//     rule: the name inside an advert's app_data is something a stranger chose, and neither end
//     logs it;
//   - a print-like call anywhere under src/link/, or a use of `Serial` that is not the framing
//     or the flush before a restart: a CLI line over the link is a console command and can be
//     `password <new>`, whose reply echoes it back (tasks/phase-1/05-local-management.md);
//   - a line of src/link/codec/, src/link/session/ or src/clock/ that allocates or uses a string
//     class: the box link's portable half and the clock keeper are fed buffers and fill buffers,
//     on a node that must repeat for months without a heap that fragments
//     (tasks/phase-1/02-the-link-codec.md, 03, 04);
//   - a board environment whose resolved build flags define ADMIN_PASSWORD as upstream's default,
//     or do not define it at all (without the define upstream's MyMesh.cpp defaults it to that
//     same word, which is how phase 0's stock image came to carry it);
//   - a board environment that turns on either of upstream's two sensor drivers this firmware
//     replaces: ENV_INCLUDE_INA3221, which applies one shunt value to all three channels where
//     this board has 100, 20 and 50 milliohm, and ENV_INCLUDE_BME280, which samples the part
//     inside querySensors and so would put an I2C conversion on the path of a box-link request
//     (tasks/phase-1/06-power-and-environment.md; src/power/Environment.h);
//   - a file of src/link/ or of src/power/ other than BoardPower.cpp that names an I2C bus or a
//     sensor driver: "a request on the box link never performs I2C" is a property of the shape -
//     one file reaches a bus, and the handlers answer out of a cache - and this is that shape
//     held in place.
// The forbidden names are written as split strings so that a grep of the repository for them finds
// the places that set them and not this test.
//
// The agent firmware's test_tls_policy is the model: a walk, a list, a failure per hit.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifndef PROJECT_DIR
#error "PROJECT_DIR must be defined; platformio.ini's [env:native] does it"
#endif

namespace {

namespace fs = std::filesystem;

const std::string kPacketLogging = std::string("MESH_PACKET") + "_LOGGING";
const std::string kMeshDebug = std::string("MESH_") + "DEBUG";
const std::string kAdminPassword = std::string("ADMIN_") + "PASSWORD";
// The two of upstream's sensor drivers src/power/ replaces. Split like the names above so that a
// grep of the repository for them finds platformio.ini's comment and not this test.
const std::string kUpstreamIna3221 = std::string("ENV_INCLUDE_") + "INA3221";
const std::string kUpstreamBme280 = std::string("ENV_INCLUDE_") + "BME280";
const std::string kUpstreamDefault = std::string("pass") + "word";

// The project root: PROJECT_DIR as the build passed it, or - should that path not hold a
// platformio.ini (a stale define, a moved checkout) - the nearest ancestor of the working
// directory that does.
fs::path projectRoot() {
  fs::path p(PROJECT_DIR);
  if (fs::is_regular_file(p / "platformio.ini")) return p;
  for (fs::path d = fs::current_path(); !d.empty(); d = d.parent_path()) {
    if (fs::is_regular_file(d / "platformio.ini")) return d;
    if (d == d.root_path()) break;
  }
  return p;
}

std::string trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  const size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

// A platformio.ini line without its comment: a line that starts with ';' or '#' is all comment,
// and so is everything from a ';' or '#' that follows whitespace.
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

bool isWordChar(char c) { return isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Whether `word` occurs in `line` as a whole identifier, so that the mesh-debug define does not
// match a longer name that merely starts with it.
bool hasWord(const std::string& line, const std::string& word) {
  size_t pos = 0;
  while ((pos = line.find(word, pos)) != std::string::npos) {
    const bool before = pos == 0 || !isWordChar(line[pos - 1]);
    const size_t end = pos + word.size();
    const bool after = end >= line.size() || !isWordChar(line[end]);
    if (before && after) return true;
    pos = end;
  }
  return false;
}

struct Ini {
  // Section name -> its lines, comments stripped, with the file line number of each.
  std::map<std::string, std::vector<std::pair<int, std::string>>> sections;
  std::vector<std::string> order;
};

Ini readIni(const fs::path& file) {
  Ini ini;
  std::ifstream in(file);
  std::string raw, section;
  int n = 0;
  while (std::getline(in, raw)) {
    n++;
    const std::string line = stripComment(raw);
    if (line.empty()) continue;
    if (line.front() == '[' && line.back() == ']') {
      section = line.substr(1, line.size() - 2);
      if (!ini.sections.count(section)) ini.order.push_back(section);
      ini.sections[section];
      continue;
    }
    ini.sections[section].push_back({n, line});
  }
  return ini;
}

// The lines that decide an environment's flags: its own, those of the section it `extends`, and
// those of every section it pulls in with ${section.key} - recursively, each once.
void resolve(const Ini& ini, const std::string& name, std::set<std::string>& seen,
             std::vector<std::pair<int, std::string>>& out) {
  if (seen.count(name) || !ini.sections.count(name)) return;
  seen.insert(name);
  for (const auto& entry : ini.sections.at(name)) {
    out.push_back(entry);
    const std::string& line = entry.second;
    if (line.rfind("extends", 0) == 0) {
      const size_t eq = line.find('=');
      if (eq != std::string::npos) resolve(ini, trim(line.substr(eq + 1)), seen, out);
    }
    size_t pos = 0;
    while ((pos = line.find("${", pos)) != std::string::npos) {
      const size_t dot = line.find('.', pos);
      const size_t close = line.find('}', pos);
      if (dot == std::string::npos || close == std::string::npos || dot > close) break;
      resolve(ini, line.substr(pos + 2, dot - pos - 2), seen, out);
      pos = close;
    }
  }
}

// Whether a flag line defines `name` (as `-D name`, `-D name=...` or `-Dname=...`); on success,
// `value` is what follows the '=' with its quoting removed, or "" when there was no '='.
bool defineValue(const std::string& line, const std::string& name, std::string& value) {
  size_t pos = 0;
  while ((pos = line.find(name, pos)) != std::string::npos) {
    const bool before = pos == 0 || !isWordChar(line[pos - 1]);
    const size_t end = pos + name.size();
    const bool after = end >= line.size() || !isWordChar(line[end]);
    if (before && after) {
      const std::string head = trim(line.substr(0, pos));
      const bool isDefine = head == "-D" || (head.size() >= 2 && head.compare(head.size() - 2, 2, "-D") == 0);
      if (isDefine) {
        std::string v;
        if (end < line.size() && line[end] == '=') {
          v = line.substr(end + 1);
          const size_t sp = v.find_first_of(" \t");
          if (sp != std::string::npos) v = v.substr(0, sp);
        }
        std::string clean;
        for (char c : v) {
          if (c != '\'' && c != '"' && c != '\\') clean += c;
        }
        value = clean;
        return true;
      }
    }
    pos = end;
  }
  return false;
}

std::vector<fs::path> filesUnder(const fs::path& dir) {
  std::vector<fs::path> files;
  if (!fs::is_directory(dir)) return files;
  for (const auto& item : fs::recursive_directory_iterator(dir)) {
    if (item.is_regular_file()) files.push_back(item.path());
  }
  return files;
}

std::string rel(const fs::path& root, const fs::path& p) {
  return fs::relative(p, root).generic_string();
}

}  // namespace

TEST(Rails, PlatformioIniNeverEnablesPacketLoggingOrMeshDebug) {
  const fs::path root = projectRoot();
  const Ini ini = readIni(root / "platformio.ini");
  ASSERT_FALSE(ini.sections.empty()) << "no platformio.ini under " << root;
  for (const auto& section : ini.sections) {
    for (const auto& entry : section.second) {
      if (hasWord(entry.second, kPacketLogging)) {
        ADD_FAILURE() << "platformio.ini:" << entry.first << ": " << kPacketLogging << " in ["
                      << section.first << "]: nothing the mesh says is logged as text (S-12)";
      }
      if (hasWord(entry.second, kMeshDebug)) {
        ADD_FAILURE() << "platformio.ini:" << entry.first << ": " << kMeshDebug << " in ["
                      << section.first << "]: nothing the mesh says is logged as text (S-12)";
      }
    }
  }
}

TEST(Rails, EveryBoardEnvironmentDefinesAnAdminPasswordThatIsNotUpstreamsDefault) {
  const fs::path root = projectRoot();
  const Ini ini = readIni(root / "platformio.ini");
  int boards = 0;
  for (const std::string& section : ini.order) {
    if (section.rfind("env:", 0) != 0 || section == "env:native") continue;
    boards++;
    std::set<std::string> seen;
    std::vector<std::pair<int, std::string>> lines;
    resolve(ini, section, seen, lines);
    bool defined = false;
    for (const auto& entry : lines) {
      std::string value;
      if (!defineValue(entry.second, kAdminPassword, value)) continue;
      defined = true;
      if (value == kUpstreamDefault || value.empty()) {
        ADD_FAILURE() << "platformio.ini:" << entry.first << ": [" << section << "] defines "
                      << kAdminPassword << " as '" << value
                      << "' - no default password is ever on the air (S-13)";
      }
    }
    if (!defined) {
      ADD_FAILURE() << "[" << section << "] does not define " << kAdminPassword
                    << "; upstream's MyMesh.cpp then defaults it to '" << kUpstreamDefault
                    << "' (S-13)";
    }
  }
  EXPECT_GT(boards, 0) << "platformio.ini has no board environment";
}

TEST(Rails, NoBoardEnvironmentTurnsOnUpstreamsOwnIna3221OrBme280) {
  const fs::path root = projectRoot();
  const Ini ini = readIni(root / "platformio.ini");
  int boards = 0;
  for (const std::string& section : ini.order) {
    if (section.rfind("env:", 0) != 0 || section == "env:native") continue;
    boards++;
    std::set<std::string> seen;
    std::vector<std::pair<int, std::string>> lines;
    resolve(ini, section, seen, lines);
    for (const auto& entry : lines) {
      std::string value;
      if (defineValue(entry.second, kUpstreamIna3221, value)) {
        ADD_FAILURE() << "platformio.ini:" << entry.first << ": [" << section << "] defines "
                      << kUpstreamIna3221
                      << " - upstream applies one shunt value to all three channels and this"
                         " board's are 100, 20 and 50 milliohm (CURRENT-SENSE.md section 4)";
      }
      if (defineValue(entry.second, kUpstreamBme280, value)) {
        ADD_FAILURE() << "platformio.ini:" << entry.first << ": [" << section << "] defines "
                      << kUpstreamBme280
                      << " - upstream samples the part inside querySensors, which is on the path"
                         " of the box link's LPP_GET (src/power/Environment.h)";
      }
    }
  }
  EXPECT_GT(boards, 0) << "platformio.ini has no board environment";
}

TEST(Rails, NothingInSrcMentionsMeshDebugOrPacketLogging) {
  const fs::path root = projectRoot();
  const std::vector<fs::path> files = filesUnder(root / "src");
  EXPECT_GT(files.size(), 5u) << "the walk of src/ found almost nothing - is PROJECT_DIR right?";
  for (const fs::path& file : files) {
    std::ifstream in(file, std::ios::binary);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
      n++;
      if (line.find(kPacketLogging) != std::string::npos) {
        ADD_FAILURE() << rel(root, file) << ":" << n << ": " << kPacketLogging;
      }
      if (line.find(kMeshDebug) != std::string::npos) {
        ADD_FAILURE() << rel(root, file) << ":" << n << ": " << kMeshDebug
                      << " (this also forbids upstream's debug-print macro in our sources)";
      }
    }
  }
}

TEST(Rails, NothingInSrcPrintsAPacketsPayloadOrAnAdvertsApplicationData) {
  // `payload` is the mesh's own bytes. `app_data` is an advert's, and the **name a stranger
  // chose** is inside it: "The name inside app_data is something a stranger chose. Neither end
  // logs it" (spec/LINK-PROTOCOL.md section 7). This firmware copies those bytes into the
  // observation ring and hands them to the Walter without parsing them, so no line of src/ has
  // any business naming either beside something that writes to a console
  // (tasks/phase-2/03-observations.md, the rails-test line).
  const fs::path root = projectRoot();
  const char* const kPrintLike[] = {"print", "write(", "Hex("};
  const char* const kMeshBytes[] = {"payload", "app_data"};
  for (const fs::path& file : filesUnder(root / "src")) {
    std::ifstream in(file, std::ios::binary);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
      n++;
      for (const char* bytes : kMeshBytes) {
        if (line.find(bytes) == std::string::npos) continue;
        for (const char* p : kPrintLike) {
          if (line.find(p) != std::string::npos) {
            ADD_FAILURE() << rel(root, file) << ":" << n << ": '" << bytes << "' beside '" << p
                          << "' - nothing the mesh says is logged as text (S-12)";
            break;
          }
        }
      }
    }
  }
}

TEST(Rails, NothingUnderThePortableLinkOrTheClockAllocatesOrHoldsAString) {
  const fs::path root = projectRoot();
  // The two portable layers of the box link (docs/ARCHITECTURE.md section 4) and the clock keeper
  // (section 5). The link's transport is not here: it is the file that is allowed to know about
  // Arduino. src/clock/ is here whole, BoardClock.cpp included - the file system's own
  // allocation is the core's, inside File::open, and nothing of ours has a reason to add to it.
  // src/meshclient/ is the mesh client's portable state (section 7): the job, its path and the
  // shared secret's buffer, which is given its memory once and wipes it rather than freeing it.
  const char* const kDirs[] = {"link/codec", "link/session", "clock", "meshclient"};
  // The names, and whether a whole-identifier match is what to look for: `new` must not match
  // `renew`, where `std::string` is not an identifier at all.
  const std::pair<std::string, bool> kForbidden[] = {
      {"new", true},    {"malloc", true},      {"calloc", true}, {"realloc", true},
      {"String", true}, {"std::string", false}};
  for (const char* name : kDirs) {
    const fs::path dir = root / "src" / name;
    const std::vector<fs::path> files = filesUnder(dir);
    EXPECT_GT(files.size(), 2u) << "almost nothing under src/" << name
                                << " - is PROJECT_DIR right?";
    for (const fs::path& file : files) {
      std::ifstream in(file, std::ios::binary);
      std::string line;
      int n = 0;
      while (std::getline(in, line)) {
        n++;
        // A comment may say any of these words; code may not. Nothing under these directories has
        // a string literal or a block comment, so the first "//" ends the code of a line.
        const size_t comment = line.find("//");
        const std::string code = comment == std::string::npos ? line : line.substr(0, comment);
        for (const auto& forbidden : kForbidden) {
          const bool hit = forbidden.second ? hasWord(code, forbidden.first)
                                            : code.find(forbidden.first) != std::string::npos;
          if (hit) {
            ADD_FAILURE() << rel(root, file) << ":" << n << ": '" << forbidden.first
                          << "' - this code is given its buffers and never allocates";
          }
        }
      }
    }
  }
}

TEST(Rails, NoRequestHandlerCanReachAnI2cBus) {
  // The acceptance criterion of phase 1's task 06: **a request on the box link never performs
  // I2C**, shown from the code's structure. That structure is one file deep: src/power/
  // BoardPower.cpp owns the only `Wire` of this firmware's own sources (src/board/target.cpp's
  // setClock is upstream's bus being configured, not a transaction) and the only sensor driver,
  // and it hands them to the two readers behind an interface; the readers are driven from
  // TwinOakSensors::loop() once a pass; and POWER_GET, ENV_GET and LPP_GET answer out of the
  // snapshot those readers leave (src/power/LinkPower.h). So nothing under src/link/, and
  // nothing under src/power/ but that one file, may name a bus or a driver at all.
  const fs::path root = projectRoot();
  const char* const kBusNames[] = {"Wire.", "Wire1.", "TwoWire", "Adafruit_BME280",
                                   "Adafruit_INA3221"};
  const char* const kDirs[] = {"link", "power"};

  int walked = 0;
  for (const char* name : kDirs) {
    for (const fs::path& file : filesUnder(root / "src" / name)) {
      // The board's half of src/power/ is where the bus is allowed to be, and it is the only
      // file of either directory that is.
      if (file.filename() == "BoardPower.cpp" || file.filename() == "BoardPower.h") continue;
      walked++;
      std::ifstream in(file, std::ios::binary);
      std::string line;
      int n = 0;
      while (std::getline(in, line)) {
        n++;
        const size_t comment = line.find("//");
        const std::string code = comment == std::string::npos ? line : line.substr(0, comment);
        for (const char* bus : kBusNames) {
          if (code.find(bus) != std::string::npos) {
            ADD_FAILURE() << rel(root, file) << ":" << n << ": '" << bus
                          << "' - a request on the box link never performs I2C, and this"
                             " directory answers out of a cache"
                             " (tasks/phase-1/06-power-and-environment.md)";
          }
        }
      }
    }
  }
  EXPECT_GT(walked, 10) << "almost nothing under src/link or src/power - is PROJECT_DIR right?";
}

TEST(Rails, NothingUnderTheBoxLinkCanLogACliLineOrItsReply) {
  // A CLI line over the link is a console command, so it can be `password <new>` - and the reply
  // to that one is `password now: <it>`, because upstream echoes it back to be sure
  // (src/helpers/CommonCLI.cpp at the pin d9296435, the `password ` branch). `get prv.key` is
  // another. So the whole of src/link/ is held to one rule: **it does not print**. The only
  // thing that directory writes is a frame, to Serial1, and the only other thing it does with a
  // UART is flush it before a restart - so the allowlist below is the whole of its business with
  // a console, and a print-like call anywhere under it is a failure whatever it prints.
  const fs::path root = projectRoot();
  const char* const kPrintLike[] = {"print", "Print", "cout", "puts(", "fputs"};
  const char* const kAllowedSerial[] = {
      "Serial1.begin(", "Serial1.available(", "Serial1.availableForWrite(", "Serial1.read(",
      "Serial1.write(", "Serial1.flush(",     "Serial.flush(",
  };

  const std::vector<fs::path> files = filesUnder(root / "src" / "link");
  EXPECT_GT(files.size(), 5u) << "almost nothing under src/link - is PROJECT_DIR right?";
  for (const fs::path& file : files) {
    std::ifstream in(file, std::ios::binary);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
      n++;
      // A comment may name any of these; code may not. Nothing under src/link has a block
      // comment, and the one string literal it has holds no such word.
      const size_t comment = line.find("//");
      const std::string code = comment == std::string::npos ? line : line.substr(0, comment);
      for (const char* p : kPrintLike) {
        if (code.find(p) != std::string::npos) {
          ADD_FAILURE() << rel(root, file) << ":" << n << ": '" << p
                        << "' - nothing under the box link prints, because a CLI line and its"
                           " reply can hold a password (LINK-PROTOCOL.md 5.7, S-12)";
        }
      }
      // Every use of the UART *objects* - `Serial.` and `Serial1.` - and not of the names that
      // merely contain the word (SerialTransport, kRebootSerialDfu, enterSerialDfu).
      size_t at = 0;
      while ((at = code.find("Serial", at)) != std::string::npos) {
        size_t after = at + 6;
        if (after < code.size() && code[after] == '1') after++;
        if (after >= code.size() || code[after] != '.') {
          at += 6;
          continue;
        }
        bool allowed = false;
        for (const char* s : kAllowedSerial) {
          const std::string call(s);
          if (code.compare(at, call.size(), call) == 0) allowed = true;
        }
        if (!allowed) {
          ADD_FAILURE() << rel(root, file) << ":" << n
                        << ": a use of a UART that is not the framing or the flush before a"
                           " restart: " << trim(code);
        }
        at += 6;
      }
    }
  }
}

TEST(Rails, NothingUnderTheMeshClientPrintsOrLogs) {
  // The mesh client's portable half (src/meshclient/) holds, for the length of a send, the one
  // plaintext copy of a password a Walter asked it to log in with, and afterwards the bytes a
  // repeater answered - a CLI reply that can echo a password back. So, as for src/link/, the
  // whole directory is held to one rule: **it does not print**, and it names no packet bytes
  // beside a print-like call (tasks/phase-2/02-the-five-sends.md, the rails-test line; S-12).
  const fs::path root = projectRoot();
  const char* const kPrintLike[] = {"print", "Print", "cout", "puts(", "fputs", "LOG_",
                                    "Serial"};
  const std::vector<fs::path> files = filesUnder(root / "src" / "meshclient");
  EXPECT_GT(files.size(), 5u) << "almost nothing under src/meshclient - is PROJECT_DIR right?";
  for (const fs::path& file : files) {
    std::ifstream in(file, std::ios::binary);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
      n++;
      const size_t comment = line.find("//");
      const std::string code = comment == std::string::npos ? line : line.substr(0, comment);
      for (const char* p : kPrintLike) {
        if (code.find(p) != std::string::npos) {
          ADD_FAILURE() << rel(root, file) << ":" << n << ": '" << p
                        << "' - nothing under the mesh client prints: it holds a password for"
                           " the length of a send and a repeater's reply after it (S-12)";
        }
      }
    }
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
