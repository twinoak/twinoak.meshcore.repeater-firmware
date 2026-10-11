#include "Version.h"

#include <stdio.h>
#include <string.h>

namespace twinoak {

size_t formatVersion(char* buf, size_t len, const char* version, const char* tag, const char* pin) {
  // %.8s shows the first eight characters of the pin and never reads past a shorter one.
  int n = snprintf(buf, len, "twinoak-repeater %s on MeshCore %s (%.8s)", version, tag, pin);
  return n < 0 ? 0 : static_cast<size_t>(n);
}

size_t formatUpstream(char* buf, size_t len, const char* tag, const char* pin) {
  int n = snprintf(buf, len, "%s+%.8s", tag, pin);
  return n < 0 ? 0 : static_cast<size_t>(n);
}

const char* versionString() {
  // Long enough for a semantic version with build metadata ("1.2.3+abcdef0"), a tag such as
  // "repeater-v1.17.1" and the eight digits of the pin; formatVersion truncates rather than
  // overruns if a future tag is longer than that.
  static char buf[96];
  if (buf[0] == 0) {
    formatVersion(buf, sizeof(buf), TWINOAK_FW_VERSION, TWINOAK_MESHCORE_TAG, TWINOAK_MESHCORE_PIN);
  }
  return buf;
}

const char* upstreamString() {
  // A tag such as "repeater-v1.17.1", a '+' and eight digits of the pin, with room for a longer
  // tag; formatUpstream truncates rather than overruns.
  static char buf[48];
  if (buf[0] == 0) {
    formatUpstream(buf, sizeof(buf), TWINOAK_MESHCORE_TAG, TWINOAK_MESHCORE_PIN);
  }
  return buf;
}

}  // namespace twinoak
