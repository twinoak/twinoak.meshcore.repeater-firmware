// The box link on the wire: Serial1 at 115200, 8N1, no flow control
// (spec/LINK-PROTOCOL.md section 1; docs/ARCHITECTURE.md section 4, layer 3).
//
// This is the thin part. All it does is the rule of ARCHITECTURE section 4, once per pass of
// loop(): read what the UART has (bounded), hand the session at most one request, write what fits
// without blocking. It keeps no protocol state - the session has all of it - and it never waits
// for the other end, because the repeater has to keep repeating with nobody on the link
// (LINK-PROTOCOL.md section 9).
//
// Two things about the Adafruit nRF52 core at the pinned commit (platform_packages,
// framework-arduinoadafruitnrf52 @ d541301) decide the numbers here:
//
//   - `Uart`'s receive ring is SERIAL_BUFFER_SIZE bytes and the define is #ifndef-guarded
//     (cores/nRF5/RingBuffer.h). The default 64 cannot hold a 246-byte frame, and 64 bytes is
//     5.5 ms of line time at 115200 - less than one pass of a repeater's loop() - so
//     platformio.ini raises it to 256. The ring holds SERIAL_BUFFER_SIZE - 1 usable bytes, which
//     is 255: one whole frame with its leading zero and room to spare.
//   - `Uart::write()` is DMA with a binary semaphore and takes it with portMAX_DELAY
//     (cores/nRF5/Uart.cpp): it blocks only while a previous transfer is still in flight, and
//     copies up to SERIAL_BUFFER_SIZE bytes in one go. `availableForWrite()` returns
//     SERIAL_BUFFER_SIZE when no transfer is in flight and 0 when one is. So "write what fits
//     without blocking" is exactly: write a whole frame when availableForWrite() is at least its
//     length, and otherwise leave it queued for the next pass. That is why the frame has to fit
//     in SERIAL_BUFFER_SIZE, and why 256 is the number.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "codec/Numbers.h"
#include "session/Session.h"

namespace twinoak {
namespace link {

class SerialTransport {
 public:
  explicit SerialTransport(Session& session) : session_(session) {}

  // Serial1 up, and the session's EV_BOOT queued. Called once from setup().
  void begin();

  // One bounded pass. Called once from loop().
  void loop();

  // The worst and the last time one pass took, in microseconds, and how many there have been:
  // the measurement tasks/phase-1/03-the-link-session.md asks for, kept where the bench can read
  // it back (docs/bench.md section 2).
  uint32_t worstPassMicros() const { return worst_us_; }
  uint32_t lastPassMicros() const { return last_us_; }
  uint32_t passes() const { return passes_; }

 private:
  // At most this many bytes are taken out of the UART in one pass. The ring cannot hold more than
  // kReceiveBuffer anyway; the bound is here so that the cost of a pass is a number and not a
  // consequence.
  static const size_t kReadBudget = kReceiveBuffer;

  Session& session_;
  uint32_t worst_us_ = 0;
  uint32_t last_us_ = 0;
  uint32_t passes_ = 0;
};

}  // namespace link
}  // namespace twinoak
