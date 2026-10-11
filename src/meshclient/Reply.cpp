#include "Reply.h"

#include <string.h>

namespace twinoak {
namespace meshclient {

// ----------------------------------------------------------------------------- the rules

bool responseIsAwaited(uint8_t await_kind, uint32_t await_tag, uint32_t response_tag) {
  // A response while a text is awaited - or while nothing is - is an old reply that took another
  // route home (AgentMesh::takeResponse). With the awaited tag 0, the next response from the peer
  // is the one: a login reply carries the repeater's timestamp and not ours.
  if (await_kind != kAwaitResponse) return false;
  return await_tag == 0 || response_tag == await_tag;
}

bool textIsAwaited(uint8_t await_kind) { return await_kind == kAwaitText; }

bool traceIsAwaited(uint8_t await_kind, uint32_t await_tag, uint32_t trace_tag) {
  if (await_kind != kAwaitTrace) return false;
  return await_tag == 0 || trace_tag == await_tag;
}

bool parseCliText(const uint8_t* data, size_t len, const uint8_t*& text, size_t& text_len) {
  text = nullptr;
  text_len = 0;
  // The agent firmware's parseCliReply (src/mesh/ReplyCodec.cpp at 51faafb4): the timestamp,
  // then a flags byte whose upper six bits are the text type, then at least one byte of text.
  if (data == nullptr || len <= kTextHeader) return false;
  const uint8_t type = uint8_t(data[4] >> 2);
  if (type != kTxtTypeCliData && type != kTxtTypePlain) return false;
  text = data + kTextHeader;
  size_t n = 0;
  // Up to the first NUL: the plaintext is zero-padded to a cipher block, and the repeater's reply
  // is a C string (MyMesh.cpp:735, `strlen(reply)`).
  while (n < len - kTextHeader && text[n] != 0) n++;
  text_len = n;
  return true;
}

// ----------------------------------------------------------------------------- the record

Reply::Reply() { clear(); }

void Reply::clear() {
  state_ = kReplyNone;
  tag_ = 0;
  via_path_ = false;
  snr_q4_ = 0;
  rssi_ = kReplyRssiUnknown;
  sent_ms_ = 0;
  elapsed_ms_ = 0;
  event_due_ = false;
  memset(data_, 0, sizeof data_);
  data_len_ = 0;
  trace_flags_ = 0;
  final_snr_q4_ = 0;
  memset(hashes_, 0, sizeof hashes_);
  hash_len_ = 0;
  memset(snrs_, 0, sizeof snrs_);
  snr_count_ = 0;
}

void Reply::arm(uint32_t tag, uint32_t now_ms) {
  clear();
  state_ = kReplyPending;
  tag_ = tag;
  sent_ms_ = now_ms;
}

void Reply::outcome(uint8_t state, int8_t snr_q4, int16_t rssi, uint32_t now_ms) {
  state_ = state;
  snr_q4_ = snr_q4;
  rssi_ = rssi;
  elapsed_ms_ = now_ms - sent_ms_;
  event_due_ = true;
}

bool Reply::takeResponse(uint8_t await_kind, uint32_t await_tag, const uint8_t* data, size_t len,
                         bool via_path, int8_t snr_q4, int16_t rssi, uint32_t now_ms) {
  if (state_ != kReplyPending) return false;  // the first reply won
  if (data == nullptr || len < kResponseTagBytes) return false;
  uint32_t tag = 0;
  memcpy(&tag, data, kResponseTagBytes);
  if (!responseIsAwaited(await_kind, await_tag, tag)) return false;

  size_t n = len - kResponseTagBytes;
  if (n > sizeof data_) n = sizeof data_;
  memcpy(data_, data + kResponseTagBytes, n);
  data_len_ = n;
  tag_ = tag;  // the reply's own, for state 3 (6.3)
  via_path_ = via_path;
  outcome(kReplyResponse, snr_q4, rssi, now_ms);
  return true;
}

bool Reply::takeText(uint8_t await_kind, const uint8_t* data, size_t len, int8_t snr_q4,
                     int16_t rssi, uint32_t now_ms) {
  if (state_ != kReplyPending) return false;
  if (!textIsAwaited(await_kind)) return false;
  const uint8_t* text = nullptr;
  size_t text_len = 0;
  if (!parseCliText(data, len, text, text_len)) return false;

  if (text_len > sizeof data_) text_len = sizeof data_;
  memcpy(data_, text, text_len);
  data_len_ = text_len;
  // tag_ stays the awaited one, which is 0 for a CLI line: a CLI reply carries no tag.
  outcome(kReplyText, snr_q4, rssi, now_ms);
  return true;
}

bool Reply::takeTrace(uint8_t await_kind, uint32_t await_tag, uint32_t tag, uint8_t flags,
                      const uint8_t* hashes, size_t hash_len, const uint8_t* snrs,
                      size_t snr_count, int8_t final_snr_q4, int16_t rssi, uint32_t now_ms) {
  if (state_ != kReplyPending) return false;
  if (!traceIsAwaited(await_kind, await_tag, tag)) return false;

  if (hash_len > sizeof hashes_) hash_len = sizeof hashes_;
  if (snr_count > sizeof snrs_) snr_count = sizeof snrs_;
  if (hashes != nullptr) memcpy(hashes_, hashes, hash_len);
  hash_len_ = hashes != nullptr ? hash_len : 0;
  if (snrs != nullptr) memcpy(snrs_, snrs, snr_count);
  snr_count_ = snrs != nullptr ? snr_count : 0;
  tag_ = tag;
  trace_flags_ = flags;
  final_snr_q4_ = final_snr_q4;
  // The record's snr_q4 is the reply's SNR at this node, which for a trace is the last hop's -
  // the same number the trace's own final_snr_q4 carries.
  outcome(kReplyTrace, final_snr_q4, rssi, now_ms);
  return true;
}

bool Reply::timeOut(uint32_t deadline_ms) {
  if (state_ != kReplyPending) return false;
  // tag_ is the awaited tag, rssi unknown, snr 0: the fixture's "mesh reply: silence".
  outcome(kReplyTimeout, 0, kReplyRssiUnknown, deadline_ms);
  return true;
}

uint32_t Reply::elapsedMs(uint32_t now_ms) const {
  if (state_ == kReplyNone) return 0;
  if (state_ == kReplyPending) return now_ms - sent_ms_;
  return elapsed_ms_;
}

}  // namespace meshclient
}  // namespace twinoak
