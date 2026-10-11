// The mesh client as the box link's MeshClientAccess (spec/LINK-PROTOCOL.md sections 5.3 and 6):
// the job (task 01), the five sends, the reply and the counters (task 02).
//
// A port of the sending half of src/mesh/AgentMesh.{h,cpp} of twinoak/meshmaster-agent-firmware
// at 51faafb427cba41b0de9983736b6621bc155f829 - sendLogin, sendRequest, sendCli, sendDiscovery,
// sendTrace, dispatch - onto the link's messages, with the plaintext of each packet built here,
// portably, and the packet itself built and queued by the board behind MeshPort.h. What that file
// learned on the air is kept, line for line, and said where it is kept:
//
//   - a login's tag is 0, because the reply carries the repeater's timestamp and not ours;
//   - a password is at most fifteen bytes, because BaseChatMesh::sendLogin sends at most fifteen
//     and the repeater's field is sixteen with its NUL;
//   - a CLI line's timestamp is this node's unique second, strictly above the last the repeater
//     saw from this key, or it is not run;
//   - a discovery always floods, whatever path the job holds;
//   - a trace cannot carry three-byte hashes, because its flags byte states the hash size as an
//     exponent;
//   - no reciprocal path is ever sent: one packet per send.
//
// And the rules of section 6 the RAK enforces, each a refusal here before anything is built:
// ERR_STATE without a job (a trace needs none), ERR_BUSY while a reply is awaited, ERR_RANGE and
// ERR_MALFORMED for what cannot be sent. Nothing here blocks: a send queues one packet and arms a
// deadline; loop() is where the deadline is checked; the reply arrives through onResponse(),
// onText(), onTrace() and onReturnedPath(), which the board calls from its peer overrides.
//
// Every buffer that held a password - the plaintext built here - is wiped before the send
// returns; the link's receive buffer, where the request's bytes are, is the session's to wipe
// and it does so after every send (src/link/session/Session.cpp).
//
// Host-portable: no Arduino, no MeshCore. test/test_link_session drives a real Session through
// one of these against a fake MeshPort, byte for byte against the fixtures.

#pragma once

#include <stdint.h>

#include "../link/codec/Messages.h"
#include "../link/codec/Numbers.h"
#include "../link/session/Interfaces.h"
#include "Job.h"
#include "MeshPort.h"
#include "Reply.h"

namespace twinoak {
namespace meshclient {

static_assert(uint8_t(kPathNone) == link::kPathNone, "PathMode must be LINK-PROTOCOL 6.1's path_mode");
static_assert(uint8_t(kPathDirect) == link::kPathDirect, "PathMode must be 6.1's path_mode");
static_assert(uint8_t(kPathHops) == link::kPathHops, "PathMode must be 6.1's path_mode");
static_assert(kPeerKeyLen == link::kPubKeyLen, "a peer's key is the link's pubkey");
static_assert(kHashSizeMin == link::kPathHashSizeMin, "6.1's bounds, in one place");
static_assert(kHashSizeMax == link::kPathHashSizeMax, "6.1's bounds, in one place");
static_assert(kHopCountMin == link::kPathHopCountMin, "6.1's bounds, in one place");
static_assert(kHopCountMax == link::kPathHopCountMax, "6.1's bounds, in one place");
static_assert(kPathBytesMax == link::kPathHashesMax, "6.1's bounds, in one place");
static_assert(kIdleEndMs == link::kJobIdleEndMs, "the 120-second rule, in one place");
static_assert(uint8_t(kReplyNone) == link::kReplyNone, "ReplyState must be 6.3's state");
static_assert(uint8_t(kReplyPending) == link::kReplyPending, "ReplyState must be 6.3's state");
static_assert(uint8_t(kReplyTimeout) == link::kReplyTimeout, "ReplyState must be 6.3's state");
static_assert(uint8_t(kReplyResponse) == link::kReplyResponse, "ReplyState must be 6.3's state");
static_assert(uint8_t(kReplyText) == link::kReplyText, "ReplyState must be 6.3's state");
static_assert(uint8_t(kReplyTrace) == link::kReplyTrace, "ReplyState must be 6.3's state");
static_assert(kReplyRssiUnknown == link::kRssiUnknown, "the unknown RSSI, in one place");

// The plaintext of each send, as the agent firmware lays it out and as the receiving repeater
// reads it (examples/simple_repeater/MyMesh.cpp at the pin d9296435):
constexpr size_t kLoginPlainMax = 4 + link::kLoginPasswordMax;  // timestamp, then the password
constexpr size_t kRequestHeader = 5;    // tag (the timestamp), then the request type, then data
constexpr size_t kCliHeader = 5;        // timestamp, the flags byte, then the line
constexpr size_t kDiscoveryPlain = 13;  // tag, type, the inverse permission mask, 3 reserved, 4 random
// REQ_TYPE_GET_TELEMETRY_DATA and TELEM_PERM_BASE (MyMesh.cpp:48 and src/helpers/SensorManager.h
// at the pin): what the companion's own path discovery asks for.
constexpr uint8_t kReqTypeGetTelemetry = 0x03;
constexpr uint8_t kTelemPermBase = 0x01;
// The longest request data a datagram can carry after its five-byte header.
constexpr size_t kRequestDataMax = kMaxDatagramPlaintext - kRequestHeader;  // 162
static_assert(kCliHeader + link::kSendCliTextMax <= kMaxDatagramPlaintext,
              "a 148-byte CLI line fits a datagram");

class LinkMeshClient : public link::MeshClientAccess {
 public:
  LinkMeshClient(Job& job, MeshPort& port) : job_(job), port_(port) {}

  // --- MeshClientAccess -------------------------------------------------------------------

  bool jobBegun() const override { return job_.begun(); }
  bool replyAwaited() const override { return job_.awaiting(); }

  uint8_t beginJob(const link::JobBegin& m, uint32_t now_ms) override;
  void endJob() override;
  uint8_t setPath(const link::Path& p) override;
  uint8_t getPath(link::Path& out) override;
  void noteLinkRequest(uint32_t now_ms) override { job_.noteLinkRequest(now_ms); }

  uint8_t sendLogin(const link::SendLogin& m, link::SendResponse& out, uint32_t now_ms) override;
  uint8_t sendRequest(const link::SendRequest& m, link::SendResponse& out,
                      uint32_t now_ms) override;
  uint8_t sendCli(const link::SendCli& m, link::SendResponse& out, uint32_t now_ms) override;
  uint8_t sendDiscovery(const link::SendDiscovery& m, link::SendResponse& out,
                        uint32_t now_ms) override;
  uint8_t sendTrace(const link::SendTrace& m, link::SendResponse& out, uint32_t now_ms) override;

  void getReply(link::ReplyRecord& out, uint32_t now_ms) override;
  bool takeReplyEvent(link::ReplyRecord& out, uint32_t now_ms) override;
  void getCounters(link::MeshCountersResponse& out) override;

  // --- the board's side -------------------------------------------------------------------

  // Once a pass, from the board's link loop: the job's silence rule (Job::loop) and the reply's
  // deadline. Nothing else looks at the clock for the wait - there is no await() here.
  void loop(uint32_t now_ms);

  // What the peer overrides of src/app/TwinOakMesh.cpp hand over, each already decrypted with
  // the job's secret and each from the job's peer and nobody else. Every one returns true when it
  // was the awaited reply and was taken; false when it was dropped unread of its meaning.
  bool onResponse(const uint8_t* data, size_t len, bool via_path, int8_t snr_q4, int16_t rssi,
                  uint32_t now_ms);
  bool onText(const uint8_t* data, size_t len, int8_t snr_q4, int16_t rssi, uint32_t now_ms);
  bool onTrace(uint32_t tag, uint8_t flags, const uint8_t* hashes, size_t hash_len,
               const uint8_t* snrs, size_t snr_count, int8_t final_snr_q4, int16_t rssi,
               uint32_t now_ms);
  // A returned path from the peer (6.3, "a returned path is learned"): `path_len` is MeshCore's
  // encoded byte and `path` its bytes. The job's path becomes it - zero hops is the direct mode -
  // and the rest of the job goes direct. True when the job took it.
  bool onReturnedPath(uint8_t path_len, const uint8_t* path);

  // Packets the client originated since boot: one per send, and nothing else (6.4).
  uint32_t clientTx() const { return client_tx_; }

  Job& job() { return job_; }
  Reply& reply() { return reply_; }
  const Reply& reply() const { return reply_; }

 private:
  // The refusals every send but a trace starts with: kErrState without a job, kErrBusy while a
  // reply is awaited. kOk means "go on".
  uint8_t gate(bool needs_job) const;
  // The route the job's path gives a login, a request or a CLI line: direct with a path (modes 1
  // and 2), a flood without one.
  Route routeOfJob() const;
  // What every successful send ends with: the count, the answer, and the wait armed in the same
  // step when `timeout_ms` is not 0 (6.2, "no reply can slip between them").
  void sent(uint8_t await_kind, uint32_t tag, uint32_t sender_ts, bool flooded,
            uint32_t timeout_ms, link::SendResponse& out, uint32_t now_ms);
  // The deadline, looked at: a pending reply whose time is up becomes a timeout, once.
  void checkDeadline(uint32_t now_ms);
  void fill(link::ReplyRecord& out, uint32_t now_ms) const;

  Job& job_;
  MeshPort& port_;
  Reply reply_;
  uint32_t client_tx_ = 0;
};

// Zeroes `len` bytes at `p` through a volatile pointer, so that the optimiser keeps the stores:
// the agent firmware's wipeBytes (src/agent/Credentials.cpp at 51faafb4).
void wipeBytes(void* p, size_t len);

}  // namespace meshclient
}  // namespace twinoak
