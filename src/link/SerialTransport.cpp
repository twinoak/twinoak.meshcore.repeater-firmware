#include "SerialTransport.h"

// Arduino comes in through the one place that includes upstream's headers (src/board/target.h
// says why): Serial1 and micros() are the core's, and nothing of MeshCore is needed here.
#include "../board/UpstreamHeaders.h"

namespace twinoak {
namespace link {

void SerialTransport::begin() {
  // Section 1: 115200 baud, 8 data bits, no parity, 1 stop bit, no flow control. SERIAL_8N1 is
  // the core's default for begin(baud); it is spelled out because the link's electrical half is
  // frozen and this line is the other half of it.
  Serial1.begin(115200, uint16_t(SERIAL_8N1));
  session_.begin(millis());
}

void SerialTransport::loop() {
  const uint32_t started_us = micros();
  // One reading of the clock for the whole pass: a pass is tens of microseconds and the session's
  // two uses of it - the 100 ms idle rule and a REBOOT's delay - are in milliseconds.
  const uint32_t now = millis();

  // 1. Read what the UART has, bounded, and stop at the byte that completes a request: what is
  //    left stays in the core's ring until the next pass, so nothing is read that cannot be
  //    acted on.
  size_t budget = kReadBudget;
  while (budget > 0 && !session_.requestWaiting() && Serial1.available() > 0) {
    budget--;
    session_.feedByte(uint8_t(Serial1.read()), now);
  }

  // 2. At most one request, and the restart a REBOOT may have asked for.
  session_.poll(now);

  // 3. Write the frame on hand if the whole of it fits in the UART's transmit buffer with no
  //    transfer in flight; otherwise it waits for the next pass. Nothing is ever sent in pieces
  //    (section 2: "Send a frame in one piece").
  if (session_.hasOutgoing()) {
    const size_t len = session_.outgoingSize();
    if (Serial1.availableForWrite() >= int(len)) {
      Serial1.write(session_.outgoing(), len);
      session_.outgoingSent(now);
    }
  }

  last_us_ = micros() - started_us;
  if (last_us_ > worst_us_) worst_us_ = last_us_;
  passes_++;
}

}  // namespace link
}  // namespace twinoak
