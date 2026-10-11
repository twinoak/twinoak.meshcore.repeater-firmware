// Asking for the Walter's attention, and keeping quiet while it sleeps
// (spec/LINK-PROTOCOL.md section 8; docs/ARCHITECTURE.md section 4).
//
// The Walter is powered from the same battery and spends most of its life asleep. Two things
// follow, and this one small machine is both of them:
//
//   - **Attention.** Something happened that the box should know about now rather than at its
//     next wake: a person asked for it over the mesh, the battery is going, the observation ring
//     is filling. Raising one costs the battery an LTE attach, so the protocol spells out exactly
//     what the RAK may do - two 0x00 bytes to wake the UART, 300 ms, then EV_ATTENTION every
//     1000 ms with `repeat` counting from 0, at most ten times, ended early by any valid request
//     - and limits it to one new attention per reason per hour.
//   - **Silence.** After SLEEP_NOTICE the RAK believes the Walter is asleep and sends no events
//     but EV_BOOT and EV_ATTENTION, until any valid request arrives.
//
// Host-portable: no Arduino, no MeshCore, no heap, no clock of its own. Every method takes the
// millisecond clock from its caller, which is what lets test/test_attention wind a fake one and
// what keeps the machine honest about a millis() wrap (every comparison is a signed difference).
//
// The session drives it (src/link/session/Session.cpp): once a pass it asks dueNow() what to put
// on the wire, and tells the machine when the bytes have gone. Nothing here knows about frames.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../codec/Messages.h"
#include "../codec/Numbers.h"
#include "../session/Interfaces.h"

namespace twinoak {
namespace link {

// One new attention per reason per hour (section 8), counted from when it was raised and kept in
// RAM - so a node that resets forgets it, which errs towards telling the box rather than not.
constexpr uint32_t kAttentionLimitMs = 3600000;
// The gap between the two wake bytes and the first event (section 8). The Walter's UART is not
// listening yet when the bytes arrive; losing the first events is expected, which is why there
// are ten of them.
constexpr uint32_t kAttentionWakeGapMs = 300;
// Reasons are 1 to 6 (Numbers.h's AttentionReason), one of them pending and the rest behind it.
constexpr uint8_t kAttentionReasonMax = kAttentionFault;
constexpr uint8_t kAttentionQueueDepth = kAttentionReasonMax - 1;

class AttentionMachine : public AttentionAccess {
 public:
  // Something wants the box awake. `detail` is the reason's own number (section 8's table).
  // Returns true when this call made it pending or put it in the queue; false when it was
  // refused, which is one of three things and none of them is an error:
  //   - the reason is already pending or already queued ("not added twice");
  //   - this reason was raised less than an hour ago (the rate limit);
  //   - the reason is not one of 1 to 6.
  bool raise(uint8_t reason, uint16_t detail, uint32_t now_ms);

  // The restart attention of section 8, reason 1, raised after EV_BOOT **only if** the Walter was
  // believed asleep when the node went down - otherwise EV_BOOT alone tells it everything this
  // event would. On this board nothing survives a reset to say so (tasks/phase-1/reports/04.md,
  // "Where the surviving RAM would live"), so src/link/BoardLink.cpp passes false and the day a
  // board can answer the question it passes the answer.
  bool noteBoot(uint8_t reset_reason, bool walter_was_asleep, uint32_t now_ms);

  // --- what the session asks and tells it -------------------------------------------------

  uint8_t pendingReason() const override { return pending_ ? reason_ : uint8_t(kAttentionNone); }
  bool walterAsleep() const override { return asleep_; }

  uint8_t dueNow(uint32_t now_ms) const override;
  Attention pendingEvent() const override;
  void wakeSent(uint32_t now_ms) override;
  void eventSent(uint32_t now_ms) override;

  void noteRequest(uint8_t request_type, uint32_t now_ms) override;
  void requestAnswered(uint32_t now_ms) override;
  void sleepNotice(uint32_t expect_back_s, uint32_t now_ms) override;

  // --- what a report and the bench read ---------------------------------------------------

  // How many EV_ATTENTION sends the current attention has had: 0 to kAttentionRepeats.
  uint8_t sendsSoFar() const { return sends_; }
  // Reasons waiting behind the pending one.
  uint8_t queued() const { return queue_len_; }
  // What the Walter said it would be back in, when it last said so (0 = unknown).
  uint32_t expectBackSeconds() const { return expect_back_s_; }
  // Attentions raised and attentions the hourly limit refused, since boot.
  uint32_t raised() const { return raised_; }
  uint32_t rateLimited() const { return rate_limited_; }

 private:
  struct Waiting {
    uint8_t reason;
    uint16_t detail;
  };

  // Makes `reason` the pending one and starts it at the wake bytes.
  void beginAttention(uint8_t reason, uint16_t detail, uint32_t now_ms);
  // The pending attention is over (a request arrived): the next in the queue takes its place.
  void endAttention(uint32_t now_ms);
  bool isQueued(uint8_t reason) const;

  // The pending attention, and where in section 8's sequence it is.
  bool pending_ = false;
  uint8_t reason_ = kAttentionNone;
  uint16_t detail_ = 0;
  bool wake_sent_ = false;    // the two 0x00 bytes have gone
  uint8_t sends_ = 0;         // EV_ATTENTION sends so far; `repeat` is this value
  uint32_t next_at_ms_ = 0;   // when the next send is due (meaningless until wake_sent_)
  bool acked_ = false;        // a request has arrived: the repeats are over, the reason still shows

  Waiting queue_[kAttentionQueueDepth] = {{0, 0}};
  uint8_t queue_len_ = 0;

  // The hourly limit, one entry per reason (index 0 is kAttentionNone and is never used).
  bool ever_raised_[kAttentionReasonMax + 1] = {false};
  uint32_t raised_at_ms_[kAttentionReasonMax + 1] = {0};

  bool asleep_ = false;
  uint32_t expect_back_s_ = 0;

  uint32_t raised_ = 0;
  uint32_t rate_limited_ = 0;
};

// Whether an event of this type may be sent while the Walter is believed asleep: EV_BOOT and
// EV_ATTENTION may, and nothing else (section 8). A free function because it is a rule of the
// protocol and not a state of the machine - the session asks it for every event it sends, and
// phase 2's EV_MESH_REPLY is the first one the answer will be no for.
inline bool eventAllowedAsleep(uint8_t type) {
  return type == kMsgEvBoot || type == kMsgEvAttention;
}

}  // namespace link
}  // namespace twinoak
