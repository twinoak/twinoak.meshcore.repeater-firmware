// The box link's session: bytes in, one request handled, one frame out
// (spec/LINK-PROTOCOL.md sections 2, 3, 5.1-5.8, 6, 7 and 8; docs/ARCHITECTURE.md section 4,
// layer 2).
//
// Host-portable: no Arduino, no MeshCore, no heap, no recursion. Everything it knows about the
// board arrives through the interfaces of Interfaces.h, which is what lets test/test_link_session
// run the whole of it against fakes.
//
// One pass of loop() is three bounded steps, and the transport does them in this order
// (src/common/link/SerialTransport.cpp):
//
//     while (uart.available() && budget-- && !session.requestWaiting())
//       session.feedByte(uart.read(), now);         // 1. read what is there
//     session.poll(now);                            // 2. handle at most one request, or let
//                                                   //    the attention machine have the wire
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

#include "common/link/attention/Attention.h"
#include "common/link/codec/FrameReceiver.h"
#include "common/link/codec/FrameWriter.h"
#include "common/link/codec/Messages.h"
#include "common/link/codec/Numbers.h"
#include "common/link/session/Interfaces.h"

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
  // Usually a frame the writer built; after an attention was raised, the two 0x00 bytes that
  // wake a sleeping Walter (section 8), which are not a frame and are the one thing this session
  // sends that is not one.
  bool hasOutgoing() const { return has_outgoing_; }
  const uint8_t* outgoing() const { return out_ptr_; }
  size_t outgoingSize() const { return out_len_; }
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

  // Whether an event of this type would be sent now, or held back because the Walter is
  // believed asleep (section 8: no events but EV_BOOT and EV_ATTENTION). Every event this
  // session sends asks it first; phase 2's EV_MESH_REPLY is the first one it will answer no for.
  bool eventAllowed(uint8_t type) const {
    if (s_.attention == nullptr || !s_.attention->walterAsleep()) return true;
    return eventAllowedAsleep(type);
  }

  // The seq the next event will carry (section 3: counted from 0 and wrapping).
  uint8_t nextEventSeq() const { return event_seq_; }
  // How many requests have actually been executed - a handler call, not a remembered answer.
  // The retry memory's proof, and nothing else reads it.
  uint32_t executed() const { return executed_; }

  // How many CLI replies filled the 160-byte window upstream's console gives its interpreter, or
  // wrote past it into the guard behind it (Session.cpp's cliHandler). Zero on a healthy node;
  // what a bench reads when a console reply comes back cut short.
  uint32_t cliTruncations() const { return cli_truncations_; }
  uint32_t cliOverruns() const { return cli_overruns_; }

 private:
  // Builds the HELLO body this firmware reports, which EV_BOOT repeats (section 5.1).
  HelloBody helloBody(uint8_t proto_minor) const;

  // Answers one request: builds the response payload into resp_, remembers it, frames it.
  void execute(const ParsedPacket& pkt, uint32_t now_ms);
  // Frames (type, seq, resp_) and makes it the outgoing frame.
  void sendResponse(uint8_t request_type, uint8_t seq, size_t len, uint32_t now_ms);
  // Frames one event with the next event seq, unless the Walter is believed asleep and this is
  // not an event section 8 lets through.
  void sendEvent(uint8_t type, const uint8_t* payload, size_t len, uint32_t now_ms);
  // CLI (section 5.7): the line run as if typed on the console, the reply back.
  size_t cliHandler(const Cli& m);
  // The five sends (section 6.2): the client's answer, or its refusal, into resp_.
  size_t sendHandler(uint8_t type, Bytes body, uint32_t now_ms);
  // Section 6.3's event, when nothing else wants the wire: true when one was built.
  bool pumpMeshReply(uint32_t now_ms);
  // Section 8's sequence, when nothing else wants the wire: the wake bytes, then the events.
  void pumpAttention(uint32_t now_ms);
  // Whether the line has been idle long enough to prefix the frame with 0x00 (section 2).
  bool lineIdle(uint32_t now_ms) const;

  const Services& s_;

  FrameReceiver rx_;
  FrameWriter writer_;
  bool has_outgoing_ = false;
  const uint8_t* out_ptr_ = nullptr;
  size_t out_len_ = 0;
  bool out_is_response_ = false;  // what is on hand is a response, not an event or the wake bytes
  bool wrote_attention_event_ = false;  // ... and what is on hand is the EV_ATTENTION just built
  bool request_waiting_ = false;

  // The console's two buffers, as upstream's own console loop sizes them (src/roles/repeater/main.cpp:
  // `static char command[160]` and `char reply[160]`), in .bss and not on a 2 KB stack:
  //
  //   - the line is a copy, NUL-terminated, because the interpreter writes into it in place;
  //   - the reply is given exactly the kCliReplyMax bytes upstream's console gives it - so a
  //     reply is at most 159 characters and its NUL, here as on USB - and behind that window
  //     sit kCliGuard bytes that are zeroed before the call and read after it. One of upstream's
  //     own commands can write past the window on a console that has nothing behind it at all
  //     (tasks/phase-1/reports/05.md names it and gives the arithmetic, which is what sets the
  //     guard at 64); here the guard absorbs it, the overrun is counted, and what this link
  //     sends is the window's own bytes.
  static const size_t kCliGuard = 64;
  char line_[kCliLineMax + 1] = {0};
  char reply_[kCliReplyMax + kCliGuard] = {0};
  uint32_t cli_truncations_ = 0;
  uint32_t cli_overruns_ = 0;

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
