// The reply record of spec/LINK-PROTOCOL.md section 6.3, and the rules that decide whether what
// arrived is the reply that was awaited (section 6.2, last paragraph).
//
// A port of the reply half of src/mesh/AgentMesh.{h,cpp} of twinoak/meshmaster-agent-firmware at
// 51faafb427cba41b0de9983736b6621bc155f829 - MeshReply, takeResponse, takeText, onTraceRecv -
// with the rules pulled out as pure functions so that test/test_meshclient can script arrivals
// against them, and with the record kept from its arrival until the next send, JOB_BEGIN or
// JOB_END instead of being read once by a blocking await(). The rules, each learned on the air by
// that firmware and each a test here:
//
//   - a login, a request and a discovery await a **response**; a CLI line awaits a **text**; a
//     trace awaits a **trace**;
//   - the awaited tag 0 means "the next reply from the peer whatever its tag": a login reply
//     carries the *repeater's* timestamp, not ours (MyMesh::handleLoginReq writes its own
//     getCurrentTimeUnique() into the reply, examples/simple_repeater/MyMesh.cpp:133-134 at the
//     pin d9296435), and a CLI reply carries no tag at all;
//   - a response while a text is awaited is dropped: it is an old reply that took another route
//     home. A response with another tag is dropped for the same reason;
//   - a text that is not CLI data is dropped (what a repeater answers a CLI line with is a TXT_MSG
//     whose flags byte says TXT_TYPE_CLI_DATA, MyMesh.cpp:743; a repeater from before v1.17
//     said TXT_TYPE_PLAIN, and the agent firmware's parseCliReply reads both);
//   - the first reply wins; a second is dropped, because the record is no longer pending;
//   - the timeout is the deadline the send armed, and reaching it is an outcome reported once.
//
// Host-portable: no Arduino, no MeshCore, no heap. The buffers are the record's own and are sized
// by MeshCore's packet (src/MeshCore.h at the pin: MAX_PACKET_PAYLOAD 184, MAX_PATH_SIZE 64),
// which src/app/TwinOakMesh.cpp holds against upstream's macros with static_asserts.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Job.h"

namespace twinoak {
namespace meshclient {

// ----------------------------------------------------------------------------- the numbers

constexpr uint8_t kReplyDataMax = 184;    // MAX_PACKET_PAYLOAD: a response's data cannot be longer
constexpr uint8_t kTraceHashesMax = 175;  // MAX_PACKET_PAYLOAD less the trace's 9-byte header
constexpr uint8_t kTraceSnrsMax = 64;     // MAX_PATH_SIZE: one SNR byte per hop in Packet::path
constexpr int16_t kReplyRssiUnknown = 0x7FFF;  // the link's kRssiUnknown

// The record's state, with the wire's values (6.3).
enum ReplyState : uint8_t {
  kReplyNone = 0,
  kReplyPending = 1,
  kReplyTimeout = 2,
  kReplyResponse = 3,
  kReplyText = 4,
  kReplyTrace = 5,
};

// A text datagram's flags byte carries the text type in its upper six bits
// (src/helpers/TxtDataHelpers.h at the pin; MyMesh.cpp:704 reads `data[4] >> 2`).
constexpr uint8_t kTxtTypePlain = 0;
constexpr uint8_t kTxtTypeCliData = 1;
// The plaintext of a text datagram: a four-byte timestamp, the flags byte, then the text.
constexpr size_t kTextHeader = 5;
// The plaintext of a response: a four-byte tag, then the repeater's reply bytes.
constexpr size_t kResponseTagBytes = 4;

// ----------------------------------------------------------------------------- the rules

// Whether a RESPONSE whose first four bytes are `response_tag` is the reply awaited: a response
// must be what is awaited, and the tag must match unless the awaited tag is 0.
bool responseIsAwaited(uint8_t await_kind, uint32_t await_tag, uint32_t response_tag);
// Whether a TXT_MSG is the reply awaited: only while a text is.
bool textIsAwaited(uint8_t await_kind);
// Whether a completed TRACE with this tag is the reply awaited.
bool traceIsAwaited(uint8_t await_kind, uint32_t await_tag, uint32_t trace_tag);

// Reads a text datagram's plaintext as a CLI reply: true, with `text`/`text_len` set to the reply
// text up to its first NUL (MeshCore zero-pads the plaintext to a cipher block), when the flags
// byte says CLI data or plain text and there is at least one byte after the header. False for
// anything else, which is then not a reply this client reads.
bool parseCliText(const uint8_t* data, size_t len, const uint8_t*& text, size_t& text_len);

// ----------------------------------------------------------------------------- the record

class Reply {
 public:
  Reply();

  // Nothing held (state 0): JOB_BEGIN, JOB_END, and a send that awaits nothing.
  void clear();
  // A send has queued its packet and is awaiting (state 1): the record is the awaited tag and
  // the moment of the send, and elapsed_ms counts from here.
  void arm(uint32_t tag, uint32_t now_ms);

  // --- arrivals: each returns true when it was the awaited reply and was taken -------------
  //
  // Each is judged against what the Job says is awaited (its awaitKind() and awaitTag()) and
  // against this record being pending - the first reply wins. The caller clears the Job's await
  // when one returns true.

  // A RESPONSE: `data` is the decrypted plaintext, tag first. `via_path` says it rode in a
  // returned path.
  bool takeResponse(uint8_t await_kind, uint32_t await_tag, const uint8_t* data, size_t len,
                    bool via_path, int8_t snr_q4, int16_t rssi, uint32_t now_ms);
  // A TXT_MSG: `data` is the decrypted plaintext, timestamp first.
  bool takeText(uint8_t await_kind, const uint8_t* data, size_t len, int8_t snr_q4, int16_t rssi,
                uint32_t now_ms);
  // A completed TRACE, as Mesh::onTraceRecv delivers it: the flags byte, the round-trip hashes as
  // this node sent them, and one SNR byte per hop that the repeaters appended.
  bool takeTrace(uint8_t await_kind, uint32_t await_tag, uint32_t tag, uint8_t flags,
                 const uint8_t* hashes, size_t hash_len, const uint8_t* snrs, size_t snr_count,
                 int8_t final_snr_q4, int16_t rssi, uint32_t now_ms);

  // The deadline passed while pending (state 2). `deadline_ms` is the Job's, so elapsed_ms is
  // exactly the timeout the send asked for. True when the record moved to timeout.
  bool timeOut(uint32_t deadline_ms);

  // --- reporting --------------------------------------------------------------------------

  // Whether an outcome (state 2 to 5) arrived that EV_MESH_REPLY has not yet reported. The
  // session asks once a pass and calls eventTaken() when it has built the event - whether or not
  // the Walter was awake to receive it: a held-back event is REPLY_GET's business (6.3).
  bool eventDue() const { return event_due_; }
  void eventTaken() { event_due_ = false; }

  uint8_t state() const { return state_; }
  uint32_t tag() const { return tag_; }
  bool viaPath() const { return via_path_; }
  int8_t snrQ4() const { return snr_q4_; }
  int16_t rssi() const { return rssi_; }
  // From the send to the reply, to the timeout, or - while pending - to `now_ms`.
  uint32_t elapsedMs(uint32_t now_ms) const;
  // State 3: the response after its tag. State 4: the text. Empty otherwise.
  const uint8_t* data() const { return data_; }
  size_t dataLen() const { return data_len_; }
  // State 5.
  uint8_t traceFlags() const { return trace_flags_; }
  int8_t finalSnrQ4() const { return final_snr_q4_; }
  const uint8_t* traceHashes() const { return hashes_; }
  size_t traceHashLen() const { return hash_len_; }
  const uint8_t* traceSnrs() const { return snrs_; }
  size_t traceSnrCount() const { return snr_count_; }

 private:
  void outcome(uint8_t state, int8_t snr_q4, int16_t rssi, uint32_t now_ms);

  uint8_t state_;
  uint32_t tag_;
  bool via_path_;
  int8_t snr_q4_;
  int16_t rssi_;
  uint32_t sent_ms_;
  uint32_t elapsed_ms_;
  bool event_due_;

  uint8_t data_[kReplyDataMax];
  size_t data_len_;

  uint8_t trace_flags_;
  int8_t final_snr_q4_;
  uint8_t hashes_[kTraceHashesMax];
  size_t hash_len_;
  uint8_t snrs_[kTraceSnrsMax];
  size_t snr_count_;
};

}  // namespace meshclient
}  // namespace twinoak
