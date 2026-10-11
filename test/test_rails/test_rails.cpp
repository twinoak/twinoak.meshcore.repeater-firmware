// The rails, as a test and not a promise (README.md "Rails that hold in every task",
// spec/SYSTEM.md S-12 and S-13, tasks/phase-1/01): it reads platformio.ini and walks src/ and
// fails on
//   - the packet-logging define or the mesh-debug define set anywhere in platformio.ini (in a
//     flag, not in a comment), or mentioned at all in a file of src/ - which also forbids
//     upstream's debug-print macro in our sources, on purpose: nothing the mesh says is logged as
//     text;
//   - a line of src/ that prints a packet's payload - any line that names `payload` beside a
//     print-like call;
//   - a line of src/link/codec/ or src/link/session/ that allocates or uses a string class: the
//     box link's portable half is fed buffers and fills buffers, on a node that must repeat for
//     months without a heap that fragments (tasks/phase-1/02-the-link-codec.md, 03);
//   - a board environment whose resolved build flags define ADMIN_PASSWORD as upstream's default,
//     or do not define it at all (without the define upstream's MyMesh.cpp defaults it to that
//     same word, which is how phase 0's stock image came to carry it).
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

TEST(Rails, NothingInSrcPrintsAPacketsPayload) {
  const fs::path root = projectRoot();
  const char* const kPrintLike[] = {"print", "write(", "Hex("};
  for (const fs::path& file : filesUnder(root / "src")) {
    std::ifstream in(file, std::ios::binary);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
      n++;
      if (line.find("payload") == std::string::npos) continue;
      for (const char* p : kPrintLike) {
        if (line.find(p) != std::string::npos) {
          ADD_FAILURE() << rel(root, file) << ":" << n << ": a payload beside '" << p
                        << "' - nothing the mesh says is logged as text (S-12)";
          break;
        }
      }
    }
  }
}

TEST(Rails, NothingUnderThePortableLinkAllocatesOrHoldsAString) {
  const fs::path root = projectRoot();
  // The two portable layers of the box link (docs/ARCHITECTURE.md section 4). The transport above
  // them is not here: it is the file that is allowed to know about Arduino.
  const char* const kDirs[] = {"codec", "session"};
  // The names, and whether a whole-identifier match is what to look for: `new` must not match
  // `renew`, where `std::string` is not an identifier at all.
  const std::pair<std::string, bool> kForbidden[] = {
      {"new", true},    {"malloc", true},      {"calloc", true}, {"realloc", true},
      {"String", true}, {"std::string", false}};
  for (const char* name : kDirs) {
    const fs::path dir = root / "src" / "link" / name;
    const std::vector<fs::path> files = filesUnder(dir);
    EXPECT_GT(files.size(), 2u) << "almost nothing under src/link/" << name
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
                          << "' - the portable link is given its buffers and never allocates";
          }
        }
      }
    }
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
