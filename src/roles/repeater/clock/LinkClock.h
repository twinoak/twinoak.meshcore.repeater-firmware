// The clock keeper as the box link's ClockAccess (spec/LINK-PROTOCOL.md sections 5.3 and 5.4).
//
// Twenty lines between two portable layers, and the one place that holds the keeper's State
// values against the wire's: a static_assert here is what makes "the keeper's state is
// time_state" a fact the compiler checks rather than a comment. src/roles/repeater/link/BoardLink.cpp hands one
// of these to the session, which is what puts TIME_PERSIST in HELLO's caps - a capability is an
// interface that is there (src/common/link/session/Interfaces.h).

#pragma once

#include <stdint.h>

#include "common/link/session/Interfaces.h"
#include "roles/repeater/clock/ClockKeeper.h"

namespace twinoak {
namespace clock {

static_assert(uint8_t(kUnset) == link::kTimeUnset, "State must be LINK-PROTOCOL 5.4's time_state");
static_assert(uint8_t(kRestored) == link::kTimeRestored, "State must be 5.4's time_state");
static_assert(uint8_t(kSet) == link::kTimeSet, "State must be 5.4's time_state");
static_assert(uint8_t(kRtc) == link::kTimeRtc, "State must be 5.4's time_state");

class LinkClock : public link::ClockAccess {
 public:
  explicit LinkClock(ClockKeeper& keeper) : k_(keeper) {}

  uint32_t epoch() const override { return k_.epoch(); }
  uint8_t timeState() const override { return k_.timeState(); }
  bool advertsHeld() const override { return k_.advertsHeld(); }
  bool setTime(uint32_t epoch, bool force, uint32_t& prev_epoch) override {
    return k_.setTime(epoch, force, prev_epoch);
  }

 private:
  ClockKeeper& k_;
};

}  // namespace clock
}  // namespace twinoak
