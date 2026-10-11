// The box link's session: bytes in, one request handled, one frame out
// (spec/LINK-PROTOCOL.md sections 2, 3, 5.1-5.6; docs/ARCHITECTURE.md section 4, layer 2).
//
// Host-portable: no Arduino, no MeshCore, no heap, no recursion. Everything it knows about the
// board arrives through the interfaces of Interfaces.h, which is what lets test/test_link_session
// run the whole of it against fakes.
//
// One pass of loop() is three bounded steps, and the transport does them in this order
// (src/link/SerialTransport.cpp):
//
//     while (uart.available() && budget-- && !session.requestWaiting())
//       session.feedByte(uart.read(), now);         // 1. read what is there
//     session.poll(now);                            // 2. handle at most one request
//     if (session.hasOutgoing() && uart.roomFor(session.outgoingSize())) {
//       uart.write(session.outgoing(), session.outgoingSize());
//       session.outgoingSent(now);                  // 3. write what fits, never waiting
//     }
//
// Three properties come out of that shape, and each has a test:
//
//   - **A retried request is executed once.** The (type, seq) of the last request answered and
//     the response given are remembered, in one entry; the same pair again is answered from the
//     memory and no handler runs (section 3). That is what makes a lost response harmless for the
//     requests that will transmit on the mesh in phase 2.
//   - **Nothing is lost and nothing blocks.** A request that completes while the previous response
//     is still unsent stays in the receiver - the transport stops reading, the UART's own buffer
//     holds the rest - and is handled on the pass after the response has gone.
//   - **The link never stalls the mesh.** At most one request per pass, and the only unbounded
//     thing a handler may do is what a later task's interface does inside it.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../codec/FrameReceiver.h"
#include "../codec/FrameWriter.h"
#include "../codec/Messages.h"
#include "../codec/Numbers.h"
#include "Interfaces.h"

namespace twinoak {
namespace link {

class Session {
 public:
  explicit Session(const Services& services) : s_(services) {}

  // The link comes up. Queues EV_BOOT once, with the reset reason and the HELLO body
  // (section 5.1). Called from the transport's begin().
  void begin(uint32_t now_ms);

  // --- one pass ---------------------------------------------------------------------------

  // Feeds one byte from the UART. Returns true when a complete, CRC-checked request is now
  // waiting: the caller stops reading and lets poll() have it. Does nothing while one waits.
  bool feedByte(uint8_t b, uint32_t now_ms);

  // Whether a request is waiting to be handled.
  bool requestWaiting() const { return request_waiting_; }

  // Handles at most one waiting request, if nothing is still unsent, and fires the restart a
  // REBOOT asked for once its response has left and its delay has passed.
  void poll(uint32_t now_ms);

  // What the transport must put on the wire, in one piece and without interleaving anything.
  bool hasOutgoing() const { return has_outgoing_; }
  const uint8_t* outgoing() const { return writer_.bytes(); }
  size_t outgoingSize() const { return writer_.size(); }
  // The transport has handed those bytes to the UART.
  void outgoingSent(uint32_t now_ms);

  // --- what the rest of the firmware reads (docs/ARCHITECTURE.md section 4) ----------------

  // Frames the receiver dropped since boot, saturating: STATUS's link_errors and the `link.errors`
  // setting.
  uint16_t linkErrors() const { return rx_.drops(); }
  // Whether a valid request has ever arrived, and how long ago: the `walter.seen` setting.
  bool everSawRequest() const { return saw_request_; }
  uint32_t secondsSinceRequest(uint32_t now_ms) const {
    return saw_request_ ? (now_ms - last_request_ms_) / 1000u : 0u;
  }

  // The seq the next event will carry (section 3: counted from 0 and wrapping).
  uint8_t nextEventSeq() const { return event_seq_; }
  // How many requests have actually been executed - a handler call, not a remembered answer.
  // The retry memory's proof, and nothing else reads it.
  uint32_t executed() const { return executed_; }

 private:
  // Builds the HELLO body this firmware reports, which EV_BOOT repeats (section 5.1).
  HelloBody helloBody(uint8_t proto_minor) const;

  // Answers one request: builds the response payload into resp_, remembers it, frames it.
  void execute(const ParsedPacket& pkt, uint32_t now_ms);
  // Frames (type, seq, resp_) and makes it the outgoing frame.
  void sendResponse(uint8_t request_type, uint8_t seq, size_t len, uint32_t now_ms);
  // Frames one event with the next event seq.
  void sendEvent(uint8_t type, const uint8_t* payload, size_t len, uint32_t now_ms);
  // Whether the line has been idle long enough to prefix the frame with 0x00 (section 2).
  bool lineIdle(uint32_t now_ms) const;

  const Services& s_;

  FrameReceiver rx_;
  FrameWriter writer_;
  bool has_outgoing_ = false;
  bool request_waiting_ = false;

  // The one-entry memory of section 3: the last request executed and the response it was given.
  uint8_t resp_[kMaxPayload] = {0};
  size_t resp_len_ = 0;
  uint8_t mem_type_ = 0;
  uint8_t mem_seq_ = 0;
  bool have_memory_ = false;

  uint8_t event_seq_ = 0;
  uint32_t executed_ = 0;

  bool ever_sent_ = false;
  uint32_t last_tx_ms_ = 0;
  bool saw_request_ = false;
  uint32_t last_request_ms_ = 0;

  // A REBOOT whose response has not left yet, then its deadline (section 5.5).
  bool restart_asked_ = false;
  bool restart_armed_ = false;
  uint8_t restart_mode_ = 0;
  uint16_t restart_delay_ms_ = 0;
  uint32_t restart_at_ms_ = 0;
};

}  // namespace link
}  // namespace twinoak
