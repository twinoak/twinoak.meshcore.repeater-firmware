#include "roles/repeater/meshclient/SendCeiling.h"

namespace twinoak {
namespace meshclient {
namespace {

// Whether `then` is still inside `window` of `now`, on a millisecond clock that wraps every 49
// days: the unsigned difference is the elapsed time for any interval under 2^31 ms, which is the
// same arithmetic upstream's Dispatcher::millisHasNowPassed uses (src/Dispatcher.cpp:382).
inline bool within(uint32_t now, uint32_t then, uint32_t window) {
  return uint32_t(now - then) < window;
}

}  // namespace

uint8_t SendCeiling::inWindow(uint32_t now_ms) const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < count_; i++) {
    if (within(now_ms, at_[i], kCeilingWindowMs)) n++;
  }
  return n;
}

bool SendCeiling::admits(uint32_t now_ms) const {
  // The gap first: it is the limit a mistake hits first, and it is the cheaper answer.
  if (any_ && within(now_ms, last_, kCeilingGapMs)) return false;
  return inWindow(now_ms) < kCeilingSends;
}

void SendCeiling::noteSent(uint32_t now_ms) {
  at_[next_] = now_ms;
  next_ = uint8_t((next_ + 1) % kCeilingSends);
  if (count_ < kCeilingSends) count_++;
  last_ = now_ms;
  any_ = true;
}

void SendCeiling::noteRefused() {
  if (refused_ != 0xFFFFFFFFu) refused_++;
}

}  // namespace meshclient
}  // namespace twinoak
