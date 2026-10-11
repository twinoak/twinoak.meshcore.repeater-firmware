// The backstop on what the mesh client may send (tasks/phase-2/04-the-rails.md).
//
// A repeater is shared infrastructure, and from phase 2 on this one takes orders from another MCU.
// The Walter's agent paces itself to a **budget the MeshMaster server sets** - its default is
// `budget 2/min gap 20 s`, which the heartbeat carries and the console prints
// (twinoak/meshmaster-agent-firmware's README.md at 51faafb4, the `server` and `jobs` screens).
// This class is the ceiling that budget can never reach in normal use: a mistake on the Walter -
// a loop in its job runner, a lease it misread, a firmware of its own that is being written -
// cannot turn this unit into a nuisance on the mesh, because the RAK refuses before the radio is
// asked.
//
// Two limits, both build constants:
//
//   - **at most 6 client sends in any 60 seconds**: three times the server's default rate, so no
//     paced agent can ever meet it. On this channel (869.618 MHz, 62.5 kHz, SF8, CR8, 32 preamble
//     symbols - RadioLibWrapper::preambleLengthForSF at the pin) the longest packet this client
//     can build is about 1.7 s of airtime flooded and about 2.2 s direct along a long path, so six
//     of them are at most some 13 seconds of transmission a minute - 22 %, inside the 50 % this
//     repeater allows itself by default (MyMesh overrides getAirtimeBudgetFactor() to return
//     `_prefs.airtime_factor`, which its constructor sets to 1.0 - examples/simple_repeater/
//     MyMesh.h:134-137 and MyMesh.cpp:890 at the pin d9296435 - and 1/(1+1) of a 3 600 000 ms
//     window is the budget, src/Dispatcher.cpp:25-27 and :38-52). A real exchange is far smaller:
//     a login is about 0.6 s, a status request 0.7 s, and the server's own 2 a minute is some 2 %
//     of the hour. **A console `set dutycycle` below about 22 % could be saturated by six
//     maximum-size sends a minute**, and then upstream simply delays - nothing is refused and
//     nothing is lost (docs/ARCHITECTURE.md section 7).
//   - **at least 2 seconds between two**: a tenth of the server's default gap, and of the same
//     order as the airtime of one packet, so a send is never more than one packet ahead of the
//     radio (docs/ARCHITECTURE.md section 7 has what that costs a packet waiting to be
//     forwarded).
//
// Over the ceiling a send is refused with `ERR_BUSY` (spec/LINK-PROTOCOL.md 6.2's word for "not
// now") and counted; `sensor get client.refused` is where the count is read, from the USB console
// or by a mesh admin, because a Walter that is being refused is a Walter whose own counters cannot
// be trusted. A refused send **costs nothing**: it is not counted against the ceiling, so a
// run-away Walter does not push its own next legitimate send further away.
//
// This does **not** replace upstream's duty-cycle accounting, which covers every packet the radio
// sends - ours and the hundreds it forwards - and is the only thing that can hold the regulatory
// limit (`Dispatcher::checkSend`, src/Dispatcher.cpp:275-354; the budget is decremented by the
// measured airtime of every completed send at :88-98, locally originated packets included,
// because there is one outbound queue and one `tx_budget_ms`). This counts *sends of ours* and is
// about the mesh's patience rather than the band's: 6 a minute is a ceiling a correct Walter never
// touches, and upstream's budget is one a busy repeater might.
//
// Host-portable: no Arduino, no MeshCore, no allocation. test/test_meshclient drives it against a
// fake clock; test/test_link_session drives it through a real Session.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace twinoak {
namespace meshclient {

// The ceiling. Both are build constants and both are reasoned about above; they are not settings,
// because a limit the Walter could raise is not a backstop against the Walter.
#ifndef TWINOAK_CLIENT_SENDS_PER_WINDOW
#define TWINOAK_CLIENT_SENDS_PER_WINDOW 6
#endif
#ifndef TWINOAK_CLIENT_WINDOW_MS
#define TWINOAK_CLIENT_WINDOW_MS 60000
#endif
#ifndef TWINOAK_CLIENT_MIN_GAP_MS
#define TWINOAK_CLIENT_MIN_GAP_MS 2000
#endif

constexpr uint8_t kCeilingSends = TWINOAK_CLIENT_SENDS_PER_WINDOW;
constexpr uint32_t kCeilingWindowMs = TWINOAK_CLIENT_WINDOW_MS;
constexpr uint32_t kCeilingGapMs = TWINOAK_CLIENT_MIN_GAP_MS;

static_assert(kCeilingSends >= 2, "a ceiling of one send would make a login and its request one");
static_assert(kCeilingGapMs * kCeilingSends <= kCeilingWindowMs,
              "the gap alone would be the rate limit, which would hide the window");

class SendCeiling {
 public:
  // Whether a send now is inside the ceiling. Asked before anything is built, so that a refused
  // send puts nothing on the air and nothing in a buffer.
  bool admits(uint32_t now_ms) const;

  // A send went. Called where `client_tx` is counted (src/meshclient/LinkMeshClient.cpp) and
  // nowhere else, so the two can never disagree about what a send is.
  void noteSent(uint32_t now_ms);

  // A send was refused by this ceiling: counted for `sensor get client.refused`, saturating at
  // the widest number that key can print. It does not touch the window - a refusal is not a send.
  void noteRefused();

  // How many sends this ceiling has refused since boot.
  uint32_t refused() const { return refused_; }

  // How many of the last sends are still inside the window: 0 to kCeilingSends. For the tests and
  // for a reader; nothing in the firmware reads it.
  uint8_t inWindow(uint32_t now_ms) const;

 private:
  // The times of the last kCeilingSends sends, in a ring whose order does not matter: counting
  // the ones still inside the window is the whole of "at most 6 in any 60 seconds", and a ring of
  // exactly that many cannot answer the question wrongly - if all six are inside the window the
  // ceiling is reached, and if one has left it there is room for one. Six 32-bit words.
  uint32_t at_[kCeilingSends] = {0};
  uint8_t count_ = 0;  // how many of at_ hold a time (0..kCeilingSends)
  uint8_t next_ = 0;   // where the next send is written (the oldest when the ring is full)
  uint32_t last_ = 0;  // the most recent send, for the gap
  bool any_ = false;   // whether last_ means anything yet
  uint32_t refused_ = 0;
};

}  // namespace meshclient
}  // namespace twinoak
