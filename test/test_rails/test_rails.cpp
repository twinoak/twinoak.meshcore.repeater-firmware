// The rails, as a test and not a promise: eleven rules, one test apiece, each failing with a
// sentence that names the rule it holds (tasks/phase-2/04-the-rails.md and phase-3/02, README.md's
// "Rails that hold in every task", spec/SYSTEM.md S-12 and S-13, spec/LINK-PROTOCOL.md section
// 6's head).
//
// It reads platformio.ini, walks src/, tools/ and .github/, and hashes spec/. In the order the
// task lists them:
//
//   1. **no MESH_PACKET_LOGGING, no MESH_DEBUG, in any environment** - not in a flag of
//      platformio.ini (a comment may say the words, which is how that file explains itself) and
//      not mentioned at all in a file of src/, which also forbids upstream's debug-print macro in
//      our sources. Does not catch: a define passed on a command line by hand.
//   2. **no source under src/ prints or logs a packet payload, a CLI line or its reply, an
//      advert's application data, or a password**. Three tests: any line of src/ that names
//      `payload` or `app_data` beside a print-like call; every print-like call under src/common/link/,
//      where a CLI line and its reply live (a `password <new>` and the echo that repeats it); and
//      the same for src/roles/repeater/meshclient/, which holds the one plaintext copy of a password a send
//      carries. Does not catch: a payload copied into a variable of another name and printed
//      from a directory that is allowed to print - which is why the two directories are held to
//      "no printing at all" rather than to a list of names.
//   3. **no board environment defines ADMIN_PASSWORD as upstream's default** (or leaves it out,
//      which makes upstream's own MyMesh.cpp default it to that same word), **and the first-boot
//      path that replaces it exists** and is called from setup() before anything can be logged
//      into. Does not catch: whether the random password is good - that is test/test_password.
//   4. **every flood the client originates passes a path-hash size of 2 or more**: the constant
//      is 2, every call of upstream's sendFlood in src/ passes that constant, and no line passes
//      a literal 1. Does not catch: a *path the Walter gives us* with one-byte hashes, which the
//      link deliberately allows (`kPathHashSizeMin` is 1) because that path was chosen by
//      somebody else - S-12 is about the hashes **this firmware chooses**.
//   5. **onPeerPathRecv returns false for the client's peer**: no reciprocal path, one packet per
//      send. The branch that handles our peer is read out of src/roles/repeater/app/TwinOakMesh.cpp and must
//      return false and nothing else. Does not catch: upstream's own path return for upstream's
//      own peers, which is upstream's business and is left alone.
//   6. **nothing under src/ writes a file whose content came from SEND_LOGIN or a sensitive
//      SEND_CLI**: the modules those bytes pass through - the link's codec and session, the mesh
//      client, and the one board file that builds the packet - name no file-system API at all.
//      Does not catch: a byte handed to another directory first; nothing does that today, and
//      rule 2's "these directories do not print" is the fence on the other side.
//   7. **upstream's INA3221 support is not in the build**, nor its BME280: the first applies one
//      shunt value to all three channels where this board has 100, 20 and 50 milliohm, and the
//      second samples the part inside querySensors, which is on the path of a box-link request.
//   8. **no file under spec/ differs from its manifest** - the SHA-256 and the byte count of
//      every line of spec/MANIFEST.txt, computed here, which is the same check
//      `spec/tools/specsync.py verify spec` makes in tools/verify.ps1 and in CI - **and no file
//      under the pinned MeshCore checkout is referenced for writing by any script**: a line of
//      tools/ or .github/ that names the checkout carries no write-like call, and only the three
//      scripts that compile out of it name it at all. Does not catch: a path under the checkout
//      put in a variable on one line and written on another.
//   9. **no heap allocation in src/common/link/ or src/roles/repeater/meshclient/** (nor in src/roles/repeater/clock/): `new`,
//      `malloc`, `calloc`, `realloc`, Arduino's `String` or `std::string` in code - a comment may
//      name them. A node that must repeat for months is given its buffers and fills buffers.
//      Does not catch: a container of a library called through a header, or a recursion deep
//      enough to matter; neither directory calls one.
//
// Two rails of earlier tasks are kept beside those nine because they are the same kind of thing:
// **a request on the box link never performs I2C** (one file reaches a bus, the handlers answer
// out of a cache - phase 1's task 06), and the two prints rails of rule 2 above.
//
// And two the scanner brought with it (phase 3's task 02), which are NoiseScope's own two
// promises and the reason a scanner-and-back round trip is safe for the repeater's identity:
//
//  10. **nothing under src/roles/scanner/ transmits.** Not one of RadioLib's transmit entry
//      points is named in that directory - and the role is the only one that drives the radio
//      itself, so the grep is over the whole of it. Does not catch: a transmit reached through a
//      function pointer, or upstream's own radio, which this image does not contain at all.
//  11. **nothing under src/roles/scanner/ names a file system.** The node's identity, its name
//      and its settings live in the internal file system, and an image that cannot open it
//      cannot lose them - which is what makes "flash the scanner, then flash the repeater back"
//      a round trip and not a provisioning. Does not catch: a raw write through the nRF52's NVMC
//      registers, which nothing in this repository names.
//
// The forbidden names are written as split strings so that a grep of the repository for them
// finds the places that set them and not this test.
//
// The agent firmware's test_tls_policy is the model: a walk, a list, a failure per hit.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Rule 4 reads the firmware's own constant rather than a copy of it.
#include "roles/repeater/meshclient/MeshPort.h"

#ifndef PROJECT_DIR
#error "PROJECT_DIR must be defined; platformio.ini's [env:native] does it"
#endif

namespace {

namespace fs = std::filesystem;

const std::string kPacketLogging = std::string("MESH_PACKET") + "_LOGGING";
const std::string kMeshDebug = std::string("MESH_") + "DEBUG";
const std::string kAdminPassword = std::string("ADMIN_") + "PASSWORD";
// The two of upstream's sensor drivers src/common/power/ replaces. Split like the names above so that a
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

// --- what the newer rails read ------------------------------------------------------------

// The bytes of a file, whole: the rails that hash spec/ and the ones that walk a source file
// line by line both start here. The files involved are kilobytes.
std::vector<uint8_t> fileBytes(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                              std::istreambuf_iterator<char>());
}

std::string fileText(const fs::path& file) {
  const std::vector<uint8_t> bytes = fileBytes(file);
  return std::string(bytes.begin(), bytes.end());
}

std::vector<std::string> linesOf(const fs::path& file) {
  std::vector<std::string> lines;
  std::ifstream in(file, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  return lines;
}

// A source line without its line comment. Code may not name what these rails forbid; a comment
// may, which is how the files that keep these rules explain why they keep them.
std::string withoutComment(const std::string& line) {
  const size_t at = line.find("//");
  return at == std::string::npos ? line : line.substr(0, at);
}

// SHA-256 (FIPS 180-4), so that rule 8 **is** the check spec/tools/specsync.py makes and not an
// approximation of it. Fifty lines is cheaper than a rail that only compares file sizes, and the
// test holds this implementation against the standard's own two vectors before it trusts it.
inline uint32_t rotr32(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

std::string sha256Hex(const std::vector<uint8_t>& in) {
  static const uint32_t K[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
      0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
      0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
      0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
      0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
      0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
      0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
      0xc67178f2};
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

  std::vector<uint8_t> m = in;
  const uint64_t bits = uint64_t(in.size()) * 8u;
  m.push_back(0x80);
  while (m.size() % 64 != 56) m.push_back(0);
  for (int i = 7; i >= 0; i--) m.push_back(uint8_t(bits >> (8 * i)));

  for (size_t off = 0; off < m.size(); off += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
      w[i] = (uint32_t(m[off + 4 * i]) << 24) | (uint32_t(m[off + 4 * i + 1]) << 16) |
             (uint32_t(m[off + 4 * i + 2]) << 8) | uint32_t(m[off + 4 * i + 3]);
    }
    for (int i = 16; i < 64; i++) {
      const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], x = h[7];
    for (int i = 0; i < 64; i++) {
      const uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
      const uint32_t ch = (e & f) ^ ((~e) & g);
      const uint32_t t1 = x + s1 + ch + K[i] + w[i];
      const uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = s0 + maj;
      x = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += x;
  }

  static const char* const kHex = "0123456789abcdef";
  std::string out;
  for (uint32_t v : h) {
    for (int i = 7; i >= 0; i--) out += kHex[(v >> (4 * i)) & 0xF];
  }
  return out;
}

}  // namespace

TEST(Rails, Rule1_PlatformioIniNeverEnablesPacketLoggingOrMeshDebug) {
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

TEST(Rails, Rule3_EveryBoardEnvironmentDefinesAnAdminPasswordThatIsNotUpstreamsDefault) {
  // **Every environment that compiles upstream's repeater**, which is where the default would
  // otherwise take effect: BUILD_EXAMPLE is what puts examples/simple_repeater/MyMesh.cpp in the
  // build, and that file is what defaults ADMIN_PASSWORD to upstream's word when the define is
  // absent. An environment without it compiles no MeshCore, has no admin and no mesh, and is
  // held to the other side of the same rule instead - it must not name the define at all, so a
  // password cannot arrive in an image that has nowhere to put it (phase 3's task 02: the
  // scanner).
  const fs::path root = projectRoot();
  const Ini ini = readIni(root / "platformio.ini");
  const std::string kBuildExample = std::string("BUILD_") + "EXAMPLE";
  int boards = 0;
  int with_mesh = 0;
  for (const std::string& section : ini.order) {
    if (section.rfind("env:", 0) != 0 || section == "env:native") continue;
    boards++;
    std::set<std::string> seen;
    std::vector<std::pair<int, std::string>> lines;
    resolve(ini, section, seen, lines);
    bool compiles_upstream = false;
    for (const auto& entry : lines) {
      std::string ignored;
      if (defineValue(entry.second, kBuildExample, ignored)) compiles_upstream = true;
    }
    bool defined = false;
    for (const auto& entry : lines) {
      std::string value;
      if (!defineValue(entry.second, kAdminPassword, value)) continue;
      defined = true;
      if (!compiles_upstream) {
        ADD_FAILURE() << "platformio.ini:" << entry.first << ": [" << section << "] defines "
                      << kAdminPassword << " and compiles no MeshCore - there is no admin in"
                                           " that image and nothing reads it (S-13)";
      } else if (value == kUpstreamDefault || value.empty()) {
        ADD_FAILURE() << "platformio.ini:" << entry.first << ": [" << section << "] defines "
                      << kAdminPassword << " as '" << value
                      << "' - no default password is ever on the air (S-13)";
      }
    }
    if (compiles_upstream) {
      with_mesh++;
      if (!defined) {
        ADD_FAILURE() << "[" << section << "] does not define " << kAdminPassword
                      << "; upstream's MyMesh.cpp then defaults it to '" << kUpstreamDefault
                      << "' (S-13)";
      }
    }
  }
  EXPECT_GT(boards, 0) << "platformio.ini has no board environment";
  EXPECT_GT(with_mesh, 0) << "no board environment compiles upstream's repeater any more, so"
                             " this rail is checking nothing";
}

TEST(Rails, Rule7_NoBoardEnvironmentTurnsOnUpstreamsOwnIna3221OrBme280) {
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
                         " of the box link's LPP_GET (src/common/power/Environment.h)";
      }
    }
  }
  EXPECT_GT(boards, 0) << "platformio.ini has no board environment";
}

TEST(Rails, Rule1_NothingInSrcMentionsMeshDebugOrPacketLogging) {
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

TEST(Rails, Rule2_NothingInSrcPrintsAPacketsPayloadOrAnAdvertsApplicationData) {
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

TEST(Rails, Rule9_NoHeapAllocationUnderTheLinkTheClockOrTheMeshClient) {
  const fs::path root = projectRoot();
  // Rule 9 of the task: **no heap allocation in src/common/link/ or src/roles/repeater/meshclient/** - and src/roles/repeater/clock/
  // beside them, which has been held to it since phase 1's task 04. The whole of src/common/link/ now,
  // its board half and its transport included (phase 1 held only the two portable layers): a node
  // that must repeat for months is given its buffers and fills buffers, and the files that touch
  // Arduino have no more reason to allocate than the portable ones. src/roles/repeater/clock/'s BoardClock.cpp
  // is here too - the file system's own allocation is the core's, inside File::open, and nothing
  // of ours has a reason to add to it. src/roles/repeater/meshclient/ is the mesh client's state (section 7):
  // the job, its path, the shared secret's buffer and the observation ring, each given its memory
  // once; the ring wipes rather than frees.
  const char* const kDirs[] = {"common/link", "roles/repeater/clock",
                               "roles/repeater/meshclient"};
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

TEST(Rails, AlsoKept_NoRequestHandlerCanReachAnI2cBus) {
  // The acceptance criterion of phase 1's task 06: **a request on the box link never performs
  // I2C**, shown from the code's structure. That structure is two files deep:
  // src/common/power/WireBus.cpp **is** the bus - the I2cBus of the reader's port, and the only
  // `Wire` transactions of this firmware's own sources (src/common/board/target.cpp's setClock is
  // upstream's bus being configured, not a transaction) - and src/common/power/BoardPower.cpp
  // owns the one of it and the only sensor driver, and hands both to the two readers behind that
  // port; the readers are driven from TwinOakSensors::loop() once a pass; and POWER_GET, ENV_GET
  // and LPP_GET answer out of the snapshot those readers leave (src/common/power/LinkPower.h). So
  // nothing under src/common/link/, and nothing under src/common/power/ but those two, may name a
  // bus or a driver at all.
  //
  // WireBus is its own file because the scanner reads the same part through the same reader and
  // compiles none of BoardPower.cpp (phase 3's task 02) - which is also why the `Wire` it uses is
  // still exactly one implementation and not a second one beside it.
  const fs::path root = projectRoot();
  const char* const kBusNames[] = {"Wire.", "Wire1.", "TwoWire", "Adafruit_BME280",
                                   "Adafruit_INA3221"};
  const char* const kDirs[] = {"common/link", "common/power"};

  int walked = 0;
  for (const char* name : kDirs) {
    for (const fs::path& file : filesUnder(root / "src" / name)) {
      // The board's half of src/common/power/ is where the bus is allowed to be, and these are
      // the only files of either directory that are.
      const std::string name_of = file.filename().generic_string();
      if (name_of == "BoardPower.cpp" || name_of == "BoardPower.h" ||
          name_of == "WireBus.cpp" || name_of == "WireBus.h") {
        continue;
      }
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
  EXPECT_GT(walked, 10) << "almost nothing under src/common/link or src/common/power - is PROJECT_DIR right?";
}

TEST(Rails, Rule2_NothingUnderTheBoxLinkCanLogACliLineOrItsReply) {
  // A CLI line over the link is a console command, so it can be `password <new>` - and the reply
  // to that one is `password now: <it>`, because upstream echoes it back to be sure
  // (src/helpers/CommonCLI.cpp at the pin d9296435, the `password ` branch). `get prv.key` is
  // another. So the whole of src/common/link/ is held to one rule: **it does not print**. The only
  // thing that directory writes is a frame, to Serial1, and the only other thing it does with a
  // UART is flush it before a restart - so the allowlist below is the whole of its business with
  // a console, and a print-like call anywhere under it is a failure whatever it prints.
  const fs::path root = projectRoot();
  const char* const kPrintLike[] = {"print", "Print", "cout", "puts(", "fputs"};
  const char* const kAllowedSerial[] = {
      "Serial1.begin(", "Serial1.available(", "Serial1.availableForWrite(", "Serial1.read(",
      "Serial1.write(", "Serial1.flush(",     "Serial.flush(",
  };

  const std::vector<fs::path> files = filesUnder(root / "src" / "common" / "link");
  EXPECT_GT(files.size(), 5u) << "almost nothing under src/common/link - is PROJECT_DIR right?";
  for (const fs::path& file : files) {
    std::ifstream in(file, std::ios::binary);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
      n++;
      // A comment may name any of these; code may not. Nothing under src/common/link has a block
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

TEST(Rails, Rule2_NothingUnderTheMeshClientPrintsOrLogs) {
  // The mesh client's portable half (src/roles/repeater/meshclient/) holds, for the length of a send, the one
  // plaintext copy of a password a Walter asked it to log in with, and afterwards the bytes a
  // repeater answered - a CLI reply that can echo a password back. So, as for src/common/link/, the
  // whole directory is held to one rule: **it does not print**, and it names no packet bytes
  // beside a print-like call (tasks/phase-2/02-the-five-sends.md, the rails-test line; S-12).
  const fs::path root = projectRoot();
  const char* const kPrintLike[] = {"print", "Print", "cout", "puts(", "fputs", "LOG_",
                                    "Serial"};
  const std::vector<fs::path> files =
      filesUnder(root / "src" / "roles" / "repeater" / "meshclient");
  EXPECT_GT(files.size(), 5u)
      << "almost nothing under src/roles/repeater/meshclient - is PROJECT_DIR right?";
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


// ---------------------------------------------------------------------- rule 3, the other half

TEST(Rails, Rule3_TheFirstBootPathThatReplacesThatPasswordExists) {
  // The define above is a placeholder and nothing else: on a boot that finds no saved
  // preferences, src/roles/repeater/app/FirstBoot.cpp draws a random password into upstream's own prefs and
  // saves them before loop() ever runs (spec/SYSTEM.md S-13). The rail is that this path is
  // **there and called**, because the define alone would be a default password on the air.
  const fs::path root = projectRoot();
  for (const char* name : {"roles/repeater/app/AdminPassword.h",
                           "roles/repeater/app/AdminPassword.cpp",
                           "roles/repeater/app/FirstBoot.h",
                           "roles/repeater/app/FirstBoot.cpp"}) {
    EXPECT_TRUE(fs::is_regular_file(root / "src" / name))
        << "src/" << name << " is gone: no default password is ever on the air (S-13), and this"
           " is the path that replaces the build's placeholder";
  }
  // And setup() calls it: the test, then the first boot it decides, by name.
  const std::string main_cpp = fileText(root / "src" / "roles" / "repeater" / "main.cpp");
  for (const char* call : {"hasSavedPrefs(", "firstBoot("}) {
    EXPECT_NE(std::string::npos, main_cpp.find(call))
        << "src/roles/repeater/main.cpp no longer calls " << call
        << "): a node with no settings would then keep the build's placeholder password (S-13)";
  }
  // ... and the first boot draws the password, whatever else it does (phase 3's task 03 put the
  // profile's lines beside it in the same call).
  const std::string first_boot =
      fileText(root / "src" / "roles" / "repeater" / "app" / "FirstBoot.cpp");
  for (const char* call : {"ensureAdminPassword(", "generatePassword("}) {
    EXPECT_NE(std::string::npos, first_boot.find(call))
        << "src/roles/repeater/app/FirstBoot.cpp no longer calls " << call
        << "): the build's placeholder password would survive a first boot (S-13)";
  }
  // The password's own rules are tested natively, which needs the file in the native build.
  const std::string ini = fileText(root / "platformio.ini");
  EXPECT_NE(std::string::npos, ini.find("+<roles/repeater/app/AdminPassword.cpp>"))
      << "platformio.ini's [env:native] no longer compiles src/roles/repeater/app/AdminPassword.cpp, so"
         " test/test_password no longer holds what a first-boot password looks like (S-13)";
}

// ------------------------------------------------------------------------------------- rule 4

TEST(Rails, Rule4_EveryFloodTheClientOriginatesUsesTwoByteHashes) {
  // "Path hashes this firmware chooses are two bytes or more" (S-12, meshmaster.dk D-33).
  // Upstream's Mesh::sendFlood takes the size as its third argument and **defaults it to 1**
  // (src/Mesh.h at the pin d9296435), so every call of ours has to pass the constant - and the
  // constant has to be 2 or more. Both halves are checked: the value, from the header the
  // firmware itself uses, and the calls, by walking src/.
  //
  // What is deliberately not checked: a path the Walter hands us in JOB_BEGIN or PATH_SET may
  // have one-byte hashes (link::kPathHashSizeMin is 1), because that path was chosen by the
  // repeater that answered us and not by this firmware. S-12 is about the hashes we choose.
  EXPECT_GE(int(twinoak::meshclient::kFloodPathHashSize), 2)
      << "every flood this firmware originates must carry two-byte path hashes or wider (S-12)";

  const fs::path root = projectRoot();
  int calls = 0;
  for (const fs::path& file : filesUnder(root / "src")) {
    int n = 0;
    for (const std::string& raw : linesOf(file)) {
      n++;
      const std::string code = withoutComment(raw);
      if (code.find("sendFlood(") == std::string::npos) continue;
      calls++;
      if (code.find("kFloodPathHashSize") == std::string::npos) {
        ADD_FAILURE() << rel(root, file) << ":" << n
                      << ": a flood whose path-hash size is not kFloodPathHashSize - upstream"
                         " defaults that argument to 1 and this firmware never floods at one"
                         " byte (S-12)";
      }
    }
  }
  EXPECT_GT(calls, 0) << "no call of sendFlood under src/ - has the mesh client moved?";
}

TEST(Rails, Rule4_TheFirstBootProfileAsksForTwoByteHashesInAdvertsToo) {
  // The other half of S-12's path-hash rule, and the half upstream decides from a preference
  // rather than from an argument: this node's own **flooded adverts** go out at
  // `path_hash_mode + 1` bytes (MyMesh::sendSelfAdvertisement,
  // examples/simple_repeater/MyMesh.cpp:1035 at the pin d9296435; the periodic flood advert at
  // :1297), and upstream's default is 0 - one byte. The first-boot profile's one line is what
  // raises it, so the rail is that the line is still in the table and that the table is still
  // compiled by the native environment, where test/test_firstboot holds every rule about it.
  const fs::path root = projectRoot();
  const std::string profile =
      fileText(root / "src" / "roles" / "repeater" / "firstboot" / "Profile.cpp");
  EXPECT_NE(std::string::npos, profile.find("\"set path.hash.mode 1\""))
      << "the first-boot profile no longer raises path.hash.mode, so a fresh unit would flood"
         " its adverts with one-byte path hashes (S-12, meshmaster.dk D-33)";
  const std::string ini = fileText(root / "platformio.ini");
  EXPECT_NE(std::string::npos, ini.find("+<roles/repeater/firstboot/Profile.cpp>"))
      << "platformio.ini's [env:native] no longer compiles"
         " src/roles/repeater/firstboot/Profile.cpp, so test/test_firstboot no longer holds the"
         " profile's table or its reply rule";
}

// ------------------------------------------------------------------------------------- rule 5

TEST(Rails, Rule5_OnPeerPathRecvReturnsFalseForTheClientsPeer) {
  // One packet per send (S-12): a returned path from the job's peer teaches us its route and is
  // **not** answered with a path of our own. Mesh::onRecvPacket sends that reciprocal path only
  // when this override returns true (src/Mesh.cpp:173-178 at the pin), so the rail is the branch
  // that handles our peer: it returns false, and it contains no other return.
  const fs::path root = projectRoot();
  const fs::path file = root / "src" / "roles" / "repeater" / "app" / "TwinOakMesh.cpp";
  const std::vector<std::string> lines = linesOf(file);
  ASSERT_FALSE(lines.empty()) << "src/roles/repeater/app/TwinOakMesh.cpp is gone";

  int start = -1;
  for (size_t i = 0; i < lines.size(); i++) {
    if (lines[i].find("bool TwinOakMesh::onPeerPathRecv(") != std::string::npos) start = int(i);
  }
  ASSERT_GE(start, 0) << "no definition of TwinOakMesh::onPeerPathRecv in"
                         " src/roles/repeater/app/TwinOakMesh.cpp: the override that must not send a reciprocal"
                         " path (S-12) is not there to read";

  // The branch is the one guarded by peerSlotIsOurs; read it to its closing brace by counting.
  int branch = -1;
  for (size_t i = size_t(start); i < lines.size(); i++) {
    if (lines[i].find("peerSlotIsOurs") != std::string::npos) {
      branch = int(i);
      break;
    }
    if (i > size_t(start) && lines[i].rfind("}", 0) == 0) break;  // the function ended first
  }
  ASSERT_GE(branch, 0) << "TwinOakMesh::onPeerPathRecv has no branch for the client's own peer:"
                          " the extra peer in front is what the whole mesh client rests on"
                          " (docs/ARCHITECTURE.md section 2)";

  int depth = 0;
  bool returns_false = false;
  bool returns_other = false;
  bool closed = false;
  for (size_t i = size_t(branch); i < lines.size() && !closed; i++) {
    const std::string code = withoutComment(lines[i]);
    if (code.find("return") != std::string::npos) {
      if (code.find("return false;") != std::string::npos) {
        returns_false = true;
      } else {
        returns_other = true;
      }
    }
    for (char c : code) {
      if (c == '{') depth++;
      if (c == '}') {
        depth--;
        if (depth <= 0) closed = true;
      }
    }
  }
  EXPECT_TRUE(returns_false) << "the branch of TwinOakMesh::onPeerPathRecv that takes the job"
                                " peer's returned path does not return false - MeshCore then"
                                " sends a reciprocal path and the send was two packets (S-12,"
                                " one packet per send)";
  EXPECT_FALSE(returns_other) << "that branch has a return that is not `return false;` - every"
                                 " way out of it must leave the reciprocal path unsent (S-12)";
}

// ------------------------------------------------------------------------------------- rule 6

TEST(Rails, Rule6_NothingThatCarriesAPasswordNamesTheFileSystem) {
  // "A password is in RAM for the length of the exchange and nowhere else" (S-12). A SEND_LOGIN's
  // password and a sensitive SEND_CLI's line pass through exactly four places: the link's codec
  // (the frame, and the decoded views into its buffer), the link's session (which wipes that
  // buffer after every send), the mesh client (which builds the plaintext and wipes it), and the
  // one board file that turns that plaintext into a packet. **None of them names a file-system
  // API at all** - no open, no File, no InternalFS - so no line of them can write those bytes to
  // flash, deliberately or by a copy somebody adds later. The firmware's own two writers are
  // src/roles/repeater/clock/BoardClock.cpp (the clock file) and src/common/power/BoardPower.cpp (the preferences),
  // and neither is on this path.
  //
  // What this does not catch: bytes handed to a directory that is allowed to write. Nothing does
  // that today, and rule 2 - these directories do not print either - is the fence on the other
  // side of the same path.
  const fs::path root = projectRoot();
  const char* const kFsNames[] = {"InternalFS", "LittleFS", "FILE_O_", "FILESYSTEM", "fopen",
                                  "ofstream",   "fwrite",   "SdFat",   ".open(",     "->open("};
  std::vector<fs::path> files;
  for (const char* dir : {"common/link/codec", "common/link/session",
                          "roles/repeater/meshclient"}) {
    for (const fs::path& f : filesUnder(root / "src" / dir)) files.push_back(f);
  }
  files.push_back(root / "src" / "roles" / "repeater" / "app" / "TwinOakMesh.h");
  files.push_back(root / "src" / "roles" / "repeater" / "app" / "TwinOakMesh.cpp");
  EXPECT_GT(files.size(), 15u) << "almost nothing on the password's path - is PROJECT_DIR right?";

  for (const fs::path& file : files) {
    int n = 0;
    for (const std::string& raw : linesOf(file)) {
      n++;
      const std::string code = withoutComment(raw);
      for (const char* name : kFsNames) {
        if (code.find(name) != std::string::npos) {
          ADD_FAILURE() << rel(root, file) << ":" << n << ": '" << name
                        << "' - a SEND_LOGIN's password and a sensitive SEND_CLI's line pass"
                           " through this file, and nothing on that path may reach the file"
                           " system (S-12)";
        }
      }
    }
  }
}

// ------------------------------------------------------------------------------------- rule 8

TEST(Rails, Rule8_NoFileUnderSpecDiffersFromItsManifest) {
  // spec/ is a copy and is never edited here (spec/README.md): the specification is changed where
  // it lives and copied again with spec/tools/specsync.py, which writes spec/MANIFEST.txt - the
  // SHA-256 and the byte count of every file. `specsync.py verify spec` is step 1 of
  // tools/verify.ps1 and of CI; this is the same check in the native tests, so that a session
  // which edits a file of spec/ by mistake is told by the tests and not only by the script.
  const fs::path root = projectRoot();
  // The implementation of SHA-256 used below, against the two vectors of FIPS 180-4, so that a
  // mistake in it cannot pass this rail by hashing everything to the same wrong thing.
  EXPECT_EQ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            sha256Hex(std::vector<uint8_t>()))
      << "the SHA-256 in this test is wrong, so rule 8 proves nothing";
  const std::string abc = "abc";
  EXPECT_EQ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            sha256Hex(std::vector<uint8_t>(abc.begin(), abc.end())))
      << "the SHA-256 in this test is wrong, so rule 8 proves nothing";

  const fs::path spec = root / "spec";
  ASSERT_TRUE(fs::is_directory(spec)) << "no spec/ under " << root;
  std::ifstream manifest(spec / "MANIFEST.txt");
  ASSERT_TRUE(manifest.good()) << "spec/MANIFEST.txt is gone: the copy has nothing to be held to";

  std::set<std::string> listed;
  std::string line;
  int entries = 0;
  while (std::getline(manifest, line)) {
    const std::string t = trim(line);
    if (t.empty() || t[0] == '#') continue;
    std::istringstream in(t);
    std::string digest, name;
    unsigned long long bytes = 0;
    in >> digest >> bytes >> name;
    ASSERT_FALSE(name.empty()) << "spec/MANIFEST.txt has a line this test cannot read: " << t;
    entries++;
    listed.insert(name);
    const fs::path file = spec / name;
    if (!fs::is_regular_file(file)) {
      ADD_FAILURE() << "spec/" << name << " is in MANIFEST.txt and not on disk: spec/ is a copy"
                       " and is never edited here (spec/README.md)";
      continue;
    }
    const std::vector<uint8_t> data = fileBytes(file);
    EXPECT_EQ(bytes, (unsigned long long)data.size())
        << "spec/" << name << " is " << data.size() << " bytes and its manifest says " << bytes
        << ": spec/ is a copy and is never edited here - change the specification where it lives"
           " and copy it with spec/tools/specsync.py (spec/README.md)";
    EXPECT_EQ(digest, sha256Hex(data))
        << "spec/" << name << " differs from its manifest: spec/ is a copy and is never edited"
           " here (spec/README.md)";
  }
  EXPECT_GT(entries, 5) << "spec/MANIFEST.txt lists almost nothing - is PROJECT_DIR right?";

  // And nothing on disk that the manifest does not list: a file added to the copy is as much an
  // edit of it as a byte changed in one. The two files that belong to this repository rather than
  // to the specification are MANIFEST.txt and README.md (spec/tools/specsync.py's own LOCAL set),
  // and the tools are listed like everything else.
  for (const fs::path& file : filesUnder(spec)) {
    const std::string name = rel(spec, file);
    if (name == "MANIFEST.txt" || name == "README.md" || name == ".gitattributes") continue;
    if (name.find("__pycache__/") != std::string::npos) continue;
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".pyc") == 0) continue;
    EXPECT_EQ(1u, listed.count(name))
        << "spec/" << name << " is not in MANIFEST.txt: spec/ is a copy and is never edited here"
           " - add the file where the specification lives and copy it (spec/README.md)";
  }
}

TEST(Rails, Rule8_NoScriptWritesIntoThePinnedMeshCoreCheckout) {
  // "MeshCore is not edited" (spec/SYSTEM.md S-2, CLAUDE.md): it is a pinned dependency under
  // .pio/libdeps/<env>/MeshCore, read and compiled, never patched. Three scripts of ours reach
  // into that checkout - they compile lib/ed25519 and the variant out of it and run its uf2conv -
  // and this rail holds all three to reading: a line that names the checkout carries no
  // write-like call, and no other script of tools/ or .github/ names it at all.
  //
  // What this does not catch: a path under the checkout assigned to a variable on one line and
  // written through that variable on another. The allowlist below is the other half of the
  // answer - a new script that reaches into the checkout fails here until somebody says so.
  const fs::path root = projectRoot();
  const char* const kCheckoutNames[] = {"PROJECT_LIBDEPS_DIR", "libdeps", "meshcore.dir"};
  const char* const kWriteLike[] = {
      "os.remove", "os.unlink",   "shutil.copy", "shutil.rmtree", "os.rename",
      "makedirs",  "Set-Content", "Out-File",    "Remove-Item",   "Copy-Item",
      "New-Item",  "sed -i",      "tee ",        ".write(",       "WriteFile",
  };
  const std::set<std::string> kMayReachIntoIt = {
      "tools/pio/meshcore_private_libs.py",
      "tools/pio/meshcore_uf2.py",
      "tools/pio/meshcore_variant.py",
  };

  int named = 0;
  for (const char* dir : {"tools", ".github"}) {
    for (const fs::path& file : filesUnder(root / dir)) {
      const std::string name = rel(root, file);
      if (name.find("__pycache__/") != std::string::npos) continue;
      int n = 0;
      bool names_it = false;
      for (const std::string& raw : linesOf(file)) {
        n++;
        bool here = false;
        for (const char* needle : kCheckoutNames) {
          if (raw.find(needle) != std::string::npos) here = true;
        }
        if (!here) continue;
        names_it = true;
        for (const char* w : kWriteLike) {
          if (raw.find(w) != std::string::npos) {
            ADD_FAILURE() << name << ":" << n << ": '" << w
                          << "' on a line that names the pinned MeshCore checkout - MeshCore is a"
                             " dependency and not one file of it is ever written (S-2)";
          }
        }
      }
      if (names_it) {
        named++;
        EXPECT_EQ(1u, kMayReachIntoIt.count(name))
            << name << " reaches into the pinned MeshCore checkout and is not one of the three"
                       " scripts that may: MeshCore is read and compiled, never written (S-2,"
                       " docs/upstream.md)";
      }
    }
  }
  EXPECT_EQ(int(kMayReachIntoIt.size()), named)
      << "the scripts that compile out of the pinned checkout are not the three this rail knows"
         " about - docs/upstream.md lists them among the places a pin bump re-reads";
}

// --------------------------------------------------------------------------------- rules 10, 11

namespace {

// Every file of the scanner role, which is the only directory of src/ that drives a radio itself
// and the only image of this family that is flashed over a provisioned repeater and off again.
std::vector<fs::path> scannerFiles(const fs::path& root) {
  return filesUnder(root / "src" / "roles" / "scanner");
}

}  // namespace

TEST(Rails, Rule10_NothingUnderTheScannerTransmits) {
  // "Never transmits. RX only" is NoiseScope's first line and the reason it may be flashed on a
  // mast whose licence is somebody else's (docs/noisescope.md). The role drives the SX1262
  // through RadioLib directly - upstream's wrapper, which would not expose these either, is not
  // in that image at all - so every one of RadioLib's own transmit entry points is forbidden by
  // name in the whole directory. `begin()` takes a transmit power because RadioLib's signature
  // has one; a power that is never applied to a packet is not a transmission, and setting it
  // *would* be, which is why setOutputPower and setTxParams are on the list.
  const fs::path root = projectRoot();
  const char* const kTransmitCalls[] = {
      "transmit(",       "startTransmit(", "finishTransmit(", "transmitDirect(",
      "setOutputPower(", "setTxParams(",   "setPaConfig(",
  };
  const std::vector<fs::path> files = scannerFiles(root);
  EXPECT_GT(files.size(), 8u) << "the walk of src/roles/scanner/ found almost nothing - is"
                                 " PROJECT_DIR right?";
  for (const fs::path& file : files) {
    int n = 0;
    for (const std::string& raw : linesOf(file)) {
      n++;
      const std::string code = withoutComment(raw);
      for (const char* call : kTransmitCalls) {
        if (code.find(call) != std::string::npos) {
          ADD_FAILURE() << rel(root, file) << ":" << n << ": '" << call
                        << "' - the scanner never transmits, which is what makes it safe to"
                           " flash on a mast (docs/noisescope.md)";
        }
      }
    }
  }
}

TEST(Rails, Rule11_NothingUnderTheScannerNamesAFileSystem) {
  // "MeshCore's on-flash config is not touched: this firmware never writes flash, so a
  // scanner-to-repeater round trip preserves the repeater's identity" (docs/noisescope.md). The
  // image compiles no file system at all - the whole of src/common/power/BoardPower.cpp, which is
  // where this firmware's preferences file lives, is outside its build_src_filter - and this is
  // the other half of that: not one line of the role names a way of opening one.
  const fs::path root = projectRoot();
  const char* const kFsNames[] = {"InternalFS",    "LittleFS", "FILE_O_", "FILESYSTEM",
                                  "InternalFileSystem", "fopen",    "ofstream", "fwrite",
                                  "SdFat",         ".open(",   "->open(", "IdentityStore"};
  for (const fs::path& file : scannerFiles(root)) {
    int n = 0;
    for (const std::string& raw : linesOf(file)) {
      n++;
      const std::string code = withoutComment(raw);
      for (const char* name : kFsNames) {
        if (code.find(name) != std::string::npos) {
          ADD_FAILURE() << rel(root, file) << ":" << n << ": '" << name
                        << "' - the scanner never writes flash, which is what makes flashing the"
                           " repeater back a round trip and not a provisioning"
                           " (docs/noisescope.md)";
        }
      }
    }
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
