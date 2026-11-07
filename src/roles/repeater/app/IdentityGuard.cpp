#include "roles/repeater/app/IdentityGuard.h"

#include <stdio.h>
#include <string.h>

namespace twinoak {
namespace identity {

bool regionIsBlank(const uint8_t* bytes, size_t len) {
  if (bytes == nullptr) return false;
  for (size_t i = 0; i < len; i++) {
    if (bytes[i] != 0xFF) return false;
  }
  return true;
}

Verdict decide(Volume volume, KeyFile key, bool prefs_present) {
  Verdict v;
  switch (volume) {
    case Volume::kBlank:
      // The base was just written (spec/SYSTEM.md S-5a), or a chip was erased: nothing was ever
      // here, so a node is made.
      v.decision = Decision::kMakeNew;
      v.why = "the file system's region is blank: a first boot";
      return v;
    case Volume::kUnmountable:
      v.decision = Decision::kRefuse;
      v.why = "the file system at 0xED000 would not mount and is not blank";
      return v;
    case Volume::kFormatFailed:
      v.decision = Decision::kRefuse;
      v.why = "the file system's region is blank and could not be formatted";
      return v;
    case Volume::kMounted:
      break;
  }
  switch (key) {
    case KeyFile::kLoaded:
      v.decision = Decision::kUseSaved;
      v.why = "the saved key";
      return v;
    case KeyFile::kUnreadable:
      v.decision = Decision::kRefuse;
      v.why = "the key file /_main.id is there and does not read as a key";
      return v;
    case KeyFile::kAbsent:
      break;
  }
  if (prefs_present) {
    // Settings without a key: a node was provisioned here and its key file is gone. The one
    // case upstream's setup() turns into a new node with the old name and the old password.
    v.decision = Decision::kRefuse;
    v.why = "the settings are there and the key file /_main.id is not";
    return v;
  }
  // Neither: an `erase` (upstream's, which formats and leaves nothing), or a volume formatted by
  // hand. A first boot, as before.
  v.decision = Decision::kMakeNew;
  v.why = "no key and no settings: a first boot";
  return v;
}

// --- the lost mode -----------------------------------------------------------------------------

namespace {

// One token, trimmed of the spaces a console or a Walter may leave around it.
bool lineIs(const char* line, const char* word) {
  if (line == nullptr) return false;
  while (*line == ' ' || *line == '\t') line++;
  const size_t n = strlen(word);
  if (strncmp(line, word, n) != 0) return false;
  line += n;
  while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') line++;
  return *line == 0;
}

size_t put(char* out, size_t cap, const char* text) {
  if (out == nullptr || cap == 0) return 0;
  const int n = snprintf(out, cap, "%s", text);
  if (n < 0) {
    out[0] = 0;
    return 0;
  }
  return size_t(n) >= cap ? cap - 1 : size_t(n);
}

}  // namespace

size_t explainLost(const Verdict& verdict, bool erased, char* out, size_t cap) {
  if (out == nullptr || cap == 0) return 0;
  int n;
  if (erased) {
    n = snprintf(out, cap,
                 "IDENTITY LOST: %s; the file system has been erased - 'reboot' starts this "
                 "radio as a new node (docs/flashing.md 3a)",
                 verdict.why);
  } else {
    n = snprintf(out, cap,
                 "IDENTITY LOST: %s. No key was made and the mesh is not started. 'erase' then "
                 "'reboot' makes this radio a new node; the old key cannot be recovered "
                 "(docs/flashing.md 3a)",
                 verdict.why);
  }
  if (n < 0) {
    out[0] = 0;
    return 0;
  }
  return size_t(n) >= cap ? cap - 1 : size_t(n);
}

size_t LostIdentityCli::runCommand(char* line, char* reply, size_t reply_cap) {
  if (reply == nullptr || reply_cap == 0) return 0;
  reply[0] = 0;
  if (lineIs(line, "erase")) {
    // Upstream's reply shape ("File system erase: OK" / "Err", src/helpers/CommonCLI.cpp:280 at
    // the pin 81dbae59), with the one next step on the end.
    if (actions_.eraseFileSystem()) {
      erased_ = true;
      return put(reply, reply_cap, "File system erase: OK - 'reboot' makes a new node");
    }
    return put(reply, reply_cap, "File system erase: Err");
  }
  if (lineIs(line, "reboot")) {
    // Nothing is owed after this line (spec/LINK-PROTOCOL.md 5.7: some commands restart the node
    // and never answer). The reply is written for a fake that returns.
    const size_t n = put(reply, reply_cap, "rebooting");
    actions_.restart();
    return n;
  }
  return explainLost(*verdict_, erased_, reply, reply_cap);
}

}  // namespace identity
}  // namespace twinoak
