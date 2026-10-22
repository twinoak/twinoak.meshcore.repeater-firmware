// The 8 s loop check NoiseScope was designed around, as arithmetic: has the main loop come round
// inside the deadline?
//
// Until phase 3.5's task 03 that check *was* the nRF52840's hardware watchdog, at 8 s
// (src/roles/scanner/Wdt.h) - and that is why a USB update could not replace the scanner: the
// watchdog cannot be stopped, it survives the soft reset into the bootloader, the bootloader does
// not feed it, and a 414 KB image takes 26 s to go down a 115200-baud DFU. So the hardware
// watchdog stays at a timeout an update measurably fits in and *this* is the 8 s check
// (tasks/phase-3.5/reports/03.md; the decision is that task's head).
//
// The two halves of it: the main loop leaves a heartbeat (`beat()`), and something that does not
// depend on the loop asks, at a fixed cadence, whether one has been left since it last looked
// (`tick()`). On the board that something is an RTC interrupt and the answer `true` is a reset
// (src/roles/scanner/LoopWdt.h). Here it is neither: no Arduino, no interrupt and no clock, so
// what the deadline *means* is settled by test/test_scanner and not by a board.
//
// Host-portable - no Arduino, no RadioLib, C++11 - and compiled by [env:native].
//
// **The one thing that is not portable** is that `beat()` runs in the main loop and `tick()` in
// an interrupt that can land between any two of its instructions. Both touch single words and
// each word has exactly one writer per state (`beat()` only ever sets `beat_`; `tick()` only ever
// clears it), which on a Cortex-M4 is a store nothing can tear. Nothing here takes a lock,
// disables an interrupt or needs to: the worst an unlucky interleaving can do is carry one
// heartbeat over into the next window, which costs a tick of lateness and never a reset that
// should not have happened.

#pragma once

#include <stdint.h>

namespace twinoak {
namespace scanner {

class LoopWatch {
 public:
  // `deadline_ms` of 0 turns the check off: `tick()` then never says the loop stopped, which is
  // what a build that wants the hardware watchdog alone would ask for. `tick_ms` is how often
  // `tick()` is called and is the resolution of the answer - a deadline is reached between
  // `deadline_ms` and `deadline_ms + tick_ms` of silence, never before.
  LoopWatch(uint32_t deadline_ms, uint32_t tick_ms);

  // The main loop was here. Called once per pass, from the loop and from nowhere else.
  void beat();

  // One cadence tick: true exactly once, when the loop has not come round inside the deadline.
  // After that it keeps saying false - the reset is already on its way, and a second one would
  // only be a second one.
  bool tick();

  // How long the loop has been silent, as whole ticks. Zero right after a heartbeat.
  uint32_t silentMs() const { return silent_ms_; }

  uint32_t deadlineMs() const { return deadline_ms_; }
  uint32_t tickMs() const { return tick_ms_; }

  // Whether `tick()` has already said the loop stopped.
  bool fired() const { return fired_; }

 private:
  const uint32_t deadline_ms_;
  const uint32_t tick_ms_;
  volatile bool beat_;
  volatile bool fired_;
  volatile uint32_t silent_ms_;
};

}  // namespace scanner
}  // namespace twinoak
