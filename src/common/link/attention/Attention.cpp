#include "common/link/attention/Attention.h"

namespace twinoak {
namespace link {

// ----------------------------------------------------------------------------- raising one

bool AttentionMachine::raise(uint8_t reason, uint16_t detail, uint32_t now_ms) {
  if (reason < kAttentionRestarted || reason > kAttentionReasonMax) return false;

  // "A reason already pending or queued is not added twice" - the second battery-low of the same
  // discharge is the same news as the first.
  if (pending_ && reason_ == reason) return false;
  if (isQueued(reason)) return false;

  // "At most one new attention per reason per hour, counted from when it was raised, kept in
  // RAM" (section 8), because each one costs the battery an LTE attach. The difference is signed
  // so that the millisecond clock's wrap is one subtraction and not a special case.
  if (ever_raised_[reason] && int32_t(now_ms - raised_at_ms_[reason]) < int32_t(kAttentionLimitMs)) {
    rate_limited_++;
    return false;
  }

  ever_raised_[reason] = true;
  raised_at_ms_[reason] = now_ms;
  raised_++;

  if (!pending_) {
    beginAttention(reason, detail, now_ms);
    return true;
  }
  // One pending at a time, with a small queue behind it. The queue is as deep as there are other
  // reasons, and no reason can be in it twice, so it cannot overflow - the check is here because
  // a seventh reason added by a later protocol version would otherwise go unnoticed.
  if (queue_len_ >= kAttentionQueueDepth) return false;
  queue_[queue_len_].reason = reason;
  queue_[queue_len_].detail = detail;
  queue_len_++;
  return true;
}

bool AttentionMachine::noteBoot(uint8_t reset_reason, bool walter_was_asleep, uint32_t now_ms) {
  if (!walter_was_asleep) return false;
  // Reason 1's detail is the reset reason (section 8's table).
  return raise(kAttentionRestarted, reset_reason, now_ms);
}

void AttentionMachine::beginAttention(uint8_t reason, uint16_t detail, uint32_t now_ms) {
  pending_ = true;
  reason_ = reason;
  detail_ = detail;
  wake_sent_ = false;
  sends_ = 0;
  acked_ = false;
  next_at_ms_ = now_ms;  // the wake bytes are due at once
}

void AttentionMachine::endAttention(uint32_t now_ms) {
  pending_ = false;
  reason_ = kAttentionNone;
  detail_ = 0;
  wake_sent_ = false;
  sends_ = 0;
  acked_ = false;
  if (queue_len_ == 0) return;

  // The next reason takes its place and starts its own sequence. The Walter has just made a
  // request, so it is awake - but this reason has never been sent, and the wake bytes before it
  // cost two bytes on a line that is idle anyway.
  const Waiting next = queue_[0];
  for (uint8_t i = 1; i < queue_len_; i++) queue_[i - 1] = queue_[i];
  queue_len_--;
  beginAttention(next.reason, next.detail, now_ms);
}

bool AttentionMachine::isQueued(uint8_t reason) const {
  for (uint8_t i = 0; i < queue_len_; i++) {
    if (queue_[i].reason == reason) return true;
  }
  return false;
}

// ----------------------------------------------------------------------------- the sequence

uint8_t AttentionMachine::dueNow(uint32_t now_ms) const {
  if (!pending_ || acked_) return kAttentionDueNothing;  // "until any valid request arrives"
  if (!wake_sent_) return kAttentionDueWake;
  // "A pending attention stays visible in STATUS until the Walter has made a request", whether or
  // not the sends were used up - so the attention does not end here, it only stops sending.
  if (sends_ >= kAttentionRepeats) return kAttentionDueNothing;
  return int32_t(now_ms - next_at_ms_) >= 0 ? kAttentionDueEvent : kAttentionDueNothing;
}

Attention AttentionMachine::pendingEvent() const {
  Attention e;
  e.reason = reason_;
  e.detail = detail_;
  e.repeat = sends_;  // "`repeat` counting from 0"
  return e;
}

void AttentionMachine::wakeSent(uint32_t now_ms) {
  if (!pending_ || wake_sent_) return;
  wake_sent_ = true;
  next_at_ms_ = now_ms + kAttentionWakeGapMs;  // 300 ms, then the first event
}

void AttentionMachine::eventSent(uint32_t now_ms) {
  if (!pending_ || !wake_sent_ || sends_ >= kAttentionRepeats) return;
  sends_++;
  next_at_ms_ = now_ms + kAttentionRepeatMs;  // every 1000 ms, ten times in all
}

// ----------------------------------------------------------------------------- the Walter

void AttentionMachine::noteRequest(uint8_t request_type, uint32_t now_ms) {
  // SLEEP_NOTICE is the one request that does not mean the Walter means to stay awake: it says
  // the opposite, and its own handler sets the flag a moment later. Keeping the flag here is also
  // what makes a *retried* SLEEP_NOTICE harmless - section 3 answers a retry from the session's
  // memory without running the handler again, so a clear here could never be undone.
  if (request_type != kMsgSleepNotice) asleep_ = false;

  // "Until any valid request arrives": the request is the acknowledgement, whatever it asked
  // for. The sends stop now; the reason stays in STATUS until this request has been answered,
  // which is requestAnswered() below.
  (void)now_ms;
  if (pending_) acked_ = true;
}

void AttentionMachine::requestAnswered(uint32_t now_ms) {
  if (pending_ && acked_) endAttention(now_ms);
}

void AttentionMachine::sleepNotice(uint32_t expect_back_s, uint32_t now_ms) {
  (void)now_ms;
  asleep_ = true;
  expect_back_s_ = expect_back_s;  // 0 = unknown; nothing here behaves differently for it
}

}  // namespace link
}  // namespace twinoak
