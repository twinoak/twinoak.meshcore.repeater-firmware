#include "roles/scanner/LoopWatch.h"

namespace twinoak {
namespace scanner {

LoopWatch::LoopWatch(uint32_t deadline_ms, uint32_t tick_ms)
    // A cadence of nothing would be a deadline that never grows, so one millisecond is the floor.
    // It is a clamp and not a refusal because this constructor runs before main() on the board
    // (a file-scope object of src/roles/scanner/LoopWdt.cpp) and has nowhere to say no.
    : deadline_ms_(deadline_ms),
      tick_ms_(tick_ms == 0u ? 1u : tick_ms),
      beat_(false),
      fired_(false),
      silent_ms_(0u) {}

void LoopWatch::beat() { beat_ = true; }

bool LoopWatch::tick() {
  if (beat_) {
    beat_ = false;
    silent_ms_ = 0u;
    return false;
  }
  silent_ms_ += tick_ms_;
  if (deadline_ms_ == 0u || fired_) return false;
  if (silent_ms_ < deadline_ms_) return false;
  fired_ = true;
  return true;
}

}  // namespace scanner
}  // namespace twinoak
