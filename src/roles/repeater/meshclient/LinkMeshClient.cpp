#include "roles/repeater/meshclient/LinkMeshClient.h"

#include <string.h>

namespace twinoak {
namespace meshclient {

void wipeBytes(void* p, size_t len) {
  volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
  for (size_t i = 0; i < len; i++) v[i] = 0;
}

// ----------------------------------------------------------------------------- the job (6.1)

uint8_t LinkMeshClient::beginJob(const link::JobBegin& m, uint32_t now_ms) {
  // The codec has already refused a path 6.1 does not allow (decodeJobBegin), so the job's own
  // check of the same bounds cannot fail here; it is kept because this file is not the only
  // caller of Job::begin, and a refusal is "a field is not a legal value" either way.
  const bool ok = job_.begin(m.pubkey, m.path.mode, m.path.hash_size, m.path.hop_count,
                             m.path.hashes.ptr, m.path.hashes.len, now_ms);
  if (!ok) return link::kErrMalformed;
  // "It ends any job in progress first" - the held reply with it (6.3: a record is kept until the
  // next send, JOB_BEGIN or JOB_END).
  reply_.clear();
  return link::kOk;
}

void LinkMeshClient::endJob() {
  job_.end();
  reply_.clear();
}

uint8_t LinkMeshClient::setPath(const link::Path& p) {
  // "ERR_STATE with no job" (6.1), judged before the path is: a Walter that forgot to begin a
  // job learns that and not that its path was wrong.
  if (!job_.begun()) return link::kErrState;
  const bool ok = job_.setPath(p.mode, p.hash_size, p.hop_count, p.hashes.ptr, p.hashes.len);
  return ok ? link::kOk : link::kErrMalformed;
}

uint8_t LinkMeshClient::getPath(link::Path& out) {
  // 6.1 says "the path the job holds now" and nothing about a node with no job; ERR_STATE is
  // the same answer PATH_SET gives (tasks/phase-2/reports/01.md, decisions).
  if (!job_.begun()) return link::kErrState;
  out.mode = job_.pathMode();
  out.hash_size = job_.pathHashSize();
  out.hop_count = job_.pathHopCount();
  // A view into the job's own storage, valid until the path changes - which is after this
  // response has been built.
  out.hashes = link::bytesOf(job_.pathBytes(), job_.pathByteCount());
  return link::kOk;
}

// ----------------------------------------------------------------------------- the sends (6.2)

uint8_t LinkMeshClient::gate(bool needs_job, uint32_t now_ms) {
  // In the order section 6.2 lists them: no job, then a reply still awaited. A trace needs no
  // job, but it cannot be sent while something else's reply is awaited either - one exchange at
  // a time.
  if (needs_job && !job_.begun()) return link::kErrState;
  if (job_.awaiting()) return link::kErrBusy;
  // And then ours: the ceiling (SendCeiling.h). Last of the three, so that a Walter with no job
  // is told that and so that nothing a request was going to be refused for anyway is counted
  // here. kErrBusy again, which is 6.2's word for "not now" - the Walter tells the two apart by
  // STATUS's flag bit 1, which is clear when it was the ceiling and set when a reply was awaited.
  if (!ceiling_.admits(now_ms)) {
    ceiling_.noteRefused();
    return link::kErrBusy;
  }
  return link::kOk;
}

Route LinkMeshClient::routeOfJob() const {
  Route r;
  // "SEND_LOGIN, SEND_REQUEST and SEND_CLI go direct when the job holds a path (modes 1 and 2)
  // and flood otherwise" (6.2). Mode 1 is zero hops: path_len 0, an empty path, sent direct -
  // the agent firmware's setDirectPath().
  r.direct = job_.pathMode() != kPathNone;
  r.path = job_.pathBytes();
  r.path_len = job_.pathLen();
  return r;
}

void LinkMeshClient::sent(uint8_t await_kind, uint32_t tag, uint32_t sender_ts, bool flooded,
                          uint32_t timeout_ms, link::SendResponse& out, uint32_t now_ms) {
  client_tx_++;  // one per send, and nothing else (6.4)
  // The ceiling is counted in the same two lines as client_tx, and nowhere else: whatever counts
  // as a send for the Walter's transaction count counts as one against the ceiling
  // (SendCeiling.h).
  ceiling_.noteSent(now_ms);
  out.send = flooded ? link::kSendFlood : link::kSendDirect;
  out.tag = tag;
  out.sender_ts = sender_ts;
  if (timeout_ms == 0) {
    // "timeout_ms of 0 means nothing is awaited" - and the record held from the last exchange
    // is let go, as any send lets it go.
    job_.clearAwait();
    reply_.clear();
    return;
  }
  // Sending and starting to wait are one step (6.2): the packet is queued and the wait is armed
  // in the same call, with nothing between them that could see a reply.
  job_.arm(await_kind, tag, now_ms + timeout_ms);
  reply_.arm(tag, now_ms);
}

uint8_t LinkMeshClient::sendLogin(const link::SendLogin& m, link::SendResponse& out,
                                  uint32_t now_ms) {
  const uint8_t g = gate(/*needs_job=*/true, now_ms);
  if (g != link::kOk) return g;
  // The codec has held the password to kLoginPasswordMax already; a longer one is kErrRange here
  // for the one caller that is not the codec.
  if (m.password.len > link::kLoginPasswordMax) return link::kErrRange;

  // The plaintext BaseChatMesh::sendLogin builds (src/helpers/BaseChatMesh.cpp:572-600 at the
  // pin): the timestamp, then the password's bytes and nothing after them. With no password it is
  // the four bytes of the timestamp alone, which is the ACL login: the datagram's plaintext is
  // zero-padded to a cipher block, so the repeater's handleLoginReq reads data[0] == 0 and
  // answers from its ACL (MyMesh.cpp:92-99). With one, it compares the bytes with its admin and
  // guest passwords by strcmp (MyMesh.cpp:101-104), which is why fifteen is the most that can
  // ever match: the field is sixteen with its NUL (src/helpers/CommonCLI.h:29).
  //
  // The tag is 0. handleLoginReq writes the **repeater's** getCurrentTimeUnique() into the
  // reply's first four bytes (MyMesh.cpp:133-134), not ours, so the reply is "the next response
  // from the peer" - the agent firmware found this on the air (its sendLogin comment).
  uint8_t plain[kLoginPlainMax];
  const uint32_t ts = port_.uniqueTime();
  memcpy(plain, &ts, 4);
  if (m.password.len > 0) memcpy(&plain[4], m.password.ptr, m.password.len);
  const Route route = routeOfJob();
  const bool ok = port_.sendAnonRequest(plain, 4 + m.password.len, route);
  // The one plaintext copy this file ever holds of a password, gone before this returns - sent or
  // not. The packet's own body is ciphertext.
  wipeBytes(plain, sizeof plain);
  if (!ok) return link::kErrFailed;
  sent(kAwaitResponse, /*tag=*/0, ts, !route.direct, m.timeout_ms, out, now_ms);
  return link::kOk;
}

uint8_t LinkMeshClient::sendRequest(const link::SendRequest& m, link::SendResponse& out,
                                    uint32_t now_ms) {
  const uint8_t g = gate(/*needs_job=*/true, now_ms);
  if (g != link::kOk) return g;
  // Mesh::createDatagram refuses a plaintext past 167 bytes (src/Mesh.cpp:490); said here as a
  // range rather than found out as a failure.
  if (m.data.len > kRequestDataMax) return link::kErrRange;

  // AgentMesh::sendRequest: the tag - the timestamp, which the repeater reflects back as the
  // reply's own first four bytes (MyMesh::handleRequest writes it at reply_data[0..3]) - then
  // the request type, then its data. The repeater runs it only when the timestamp is above the
  // last it saw from this key (MyMesh.cpp:674), which the unique-time source guarantees.
  uint8_t plain[kMaxDatagramPlaintext];
  const uint32_t ts = port_.uniqueTime();
  memcpy(plain, &ts, 4);
  plain[4] = m.req_type;
  if (m.data.len > 0) memcpy(&plain[kRequestHeader], m.data.ptr, m.data.len);
  const Route route = routeOfJob();
  const bool ok = port_.sendDatagram(kPayloadTypeReq, plain, kRequestHeader + m.data.len, route);
  wipeBytes(plain, sizeof plain);
  if (!ok) return link::kErrFailed;
  sent(kAwaitResponse, ts, ts, !route.direct, m.timeout_ms, out, now_ms);
  return link::kOk;
}

uint8_t LinkMeshClient::sendCli(const link::SendCli& m, link::SendResponse& out,
                                uint32_t now_ms) {
  const uint8_t g = gate(/*needs_job=*/true, now_ms);
  if (g != link::kOk) return g;
  // The codec has held the line to 1..kSendCliTextMax already (decodeSendCli).
  if (m.text.len < 1 || m.text.len > link::kSendCliTextMax) return link::kErrRange;

  // The shape BaseChatMesh::sendCommandData gives a CLI line (src/helpers/BaseChatMesh.cpp:
  // 462-470 at the pin): the sender timestamp, a flags byte of attempt 0 with TXT_TYPE_CLI_DATA
  // in its upper six bits, then the text. The receiving repeater (MyMesh::onPeerDataRecv,
  // MyMesh.cpp:701-756) runs it through its CLI for an admin client when the timestamp is at or
  // above the last it saw - an equal one is a retry and is answered with an empty reply, so
  // nothing runs twice - and answers with a TXT_MSG of CLI data that carries no tag. The
  // timestamp matters, and it comes from the unique-time source at the moment the packet is
  // built, as every tag of this client does: one strictly increasing series across requests and
  // CLI lines alike.
  uint8_t plain[kMaxDatagramPlaintext];
  const uint32_t ts = port_.uniqueTime();
  memcpy(plain, &ts, 4);
  plain[4] = uint8_t(kTxtTypeCliData << 2);
  memcpy(&plain[kCliHeader], m.text.ptr, m.text.len);
  const Route route = routeOfJob();
  const bool ok = port_.sendDatagram(kPayloadTypeTxtMsg, plain, kCliHeader + m.text.len, route);
  // Wiped whether or not the Walter flagged the line sensitive: a `password <new>` it forgot to
  // flag is gone all the same, and the cost is a loop over 167 bytes.
  wipeBytes(plain, sizeof plain);
  if (!ok) return link::kErrFailed;
  // Tag 0: a CLI reply carries the repeater's own timestamp and no tag (6.2).
  sent(kAwaitText, /*tag=*/0, ts, !route.direct, m.timeout_ms, out, now_ms);
  return link::kOk;
}

uint8_t LinkMeshClient::sendDiscovery(const link::SendDiscovery& m, link::SendResponse& out,
                                      uint32_t now_ms) {
  const uint8_t g = gate(/*needs_job=*/true, now_ms);
  if (g != link::kOk) return g;

  // AgentMesh::sendDiscovery, which is the companion's own CMD_SEND_PATH_DISCOVERY_REQ: "'Path
  // Discovery' is just a special case of flood + Telemetry req". A base-telemetry request - the
  // type, then the *inverse* permission mask (MyMesh::handleRequest reads `~payload[1]`,
  // MyMesh.cpp:241), three reserved bytes and four random ones for packet uniqueness - **always
  // flooded** whatever path the job holds, because the whole point is that the reply comes back
  // as a returned path and teaches the route (onReturnedPath).
  uint8_t plain[kDiscoveryPlain];
  const uint32_t ts = port_.uniqueTime();
  memcpy(plain, &ts, 4);
  plain[4] = kReqTypeGetTelemetry;
  plain[5] = uint8_t(~kTelemPermBase);
  plain[6] = plain[7] = plain[8] = 0;
  port_.randomBytes(&plain[9], 4);
  Route flood;  // never direct
  const bool ok = port_.sendDatagram(kPayloadTypeReq, plain, sizeof plain, flood);
  wipeBytes(plain, sizeof plain);
  if (!ok) return link::kErrFailed;
  sent(kAwaitResponse, ts, ts, /*flooded=*/true, m.timeout_ms, out, now_ms);
  return link::kOk;
}

uint8_t LinkMeshClient::sendTrace(const link::SendTrace& m, link::SendResponse& out,
                                  uint32_t now_ms) {
  // "SEND_TRACE needs no job" (6.2): a trace is addressed to nobody - the path is the packet.
  const uint8_t g = gate(/*needs_job=*/false, now_ms);
  if (g != link::kOk) return g;

  // A trace states its hash size as an exponent in the low two bits of its flags byte: every
  // repeater on the way matches its hash over `1 << (flags & 3)` bytes (Mesh::onRecvPacket,
  // src/Mesh.cpp:50 and :58 at the pin). So 1, 2, 4 and 8 bytes have an encoding and three do
  // not - "a trace cannot express three-byte hashes; hash_size 3 is ERR_RANGE" (6.2), which the
  // agent firmware found when it traced a path learned at three bytes (its ReplyCodec.h).
  uint8_t flags = 0;
  switch (m.hash_size) {
    case 1: flags = 0; break;
    case 2: flags = 1; break;
    case 4: flags = 2; break;
    case 8: flags = 3; break;
    case 3: return link::kErrRange;
    default: return link::kErrMalformed;  // not a value section 6.2 names at all
  }
  // The hops must be whole, and there must be at least one: an empty path has nowhere to go.
  if (m.hashes.len == 0 || (m.hashes.len % m.hash_size) != 0) return link::kErrMalformed;
  // The path travels in the payload, after the trace's 9-byte header, and Mesh::sendDirect
  // appends it without a bound of its own (src/Mesh.cpp:698-701): the bound is here.
  if (m.hashes.len > kTraceHashesMaxBytes) return link::kErrRange;

  const uint32_t ts = port_.uniqueTime();
  uint32_t auth = 0;
  port_.randomBytes(reinterpret_cast<uint8_t*>(&auth), 4);
  // Always direct: the hops are the path, and every one appends its SNR on the way.
  if (!port_.sendTrace(ts, auth, flags, m.hashes.ptr, m.hashes.len)) return link::kErrFailed;
  sent(kAwaitTrace, ts, ts, /*flooded=*/false, m.timeout_ms, out, now_ms);
  return link::kOk;
}

// ----------------------------------------------------------------------------- the reply (6.3)

void LinkMeshClient::checkDeadline(uint32_t now_ms) {
  if (!job_.awaitExpired(now_ms)) return;
  // The wait is over: the record says so once, and the extra peer leaves the front of upstream's
  // matches with the await.
  reply_.timeOut(job_.awaitDeadline());
  job_.clearAwait();
}

void LinkMeshClient::loop(uint32_t now_ms) {
  checkDeadline(now_ms);
  // The silence rule ends the job - and with it anything awaited or held (6.1, JOB_END's words).
  if (job_.loop(now_ms)) reply_.clear();
}

void LinkMeshClient::fill(link::ReplyRecord& out, uint32_t now_ms) const {
  out.state = reply_.state();
  out.tag = reply_.tag();
  out.via_path = reply_.viaPath() ? 1 : 0;
  out.snr_q4 = reply_.snrQ4();
  out.rssi = reply_.rssi();
  out.elapsed_ms = reply_.elapsedMs(now_ms);
  out.data = link::bytesOf(reply_.data(), reply_.dataLen());
  out.trace_flags = reply_.traceFlags();
  out.final_snr_q4 = reply_.finalSnrQ4();
  out.hashes = link::bytesOf(reply_.traceHashes(), reply_.traceHashLen());
  out.snrs = link::bytesOf(reply_.traceSnrs(), reply_.traceSnrCount());
}

void LinkMeshClient::getReply(link::ReplyRecord& out, uint32_t now_ms) {
  // A REPLY_GET after the deadline never says "pending": the deadline is looked at first.
  checkDeadline(now_ms);
  fill(out, now_ms);
}

bool LinkMeshClient::takeReplyEvent(link::ReplyRecord& out, uint32_t now_ms) {
  checkDeadline(now_ms);
  if (!reply_.eventDue()) return false;
  reply_.eventTaken();
  fill(out, now_ms);
  return true;
}

bool LinkMeshClient::onResponse(const uint8_t* data, size_t len, bool via_path, int8_t snr_q4,
                                int16_t rssi, uint32_t now_ms) {
  if (!reply_.takeResponse(job_.awaitKind(), job_.awaitTag(), data, len, via_path, snr_q4, rssi,
                           now_ms)) {
    return false;
  }
  job_.clearAwait();
  return true;
}

bool LinkMeshClient::onText(const uint8_t* data, size_t len, int8_t snr_q4, int16_t rssi,
                            uint32_t now_ms) {
  if (!reply_.takeText(job_.awaitKind(), data, len, snr_q4, rssi, now_ms)) return false;
  job_.clearAwait();
  return true;
}

bool LinkMeshClient::onTrace(uint32_t tag, uint8_t flags, const uint8_t* hashes, size_t hash_len,
                             const uint8_t* snrs, size_t snr_count, int8_t final_snr_q4,
                             int16_t rssi, uint32_t now_ms) {
  if (!reply_.takeTrace(job_.awaitKind(), job_.awaitTag(), tag, flags, hashes, hash_len, snrs,
                        snr_count, final_snr_q4, rssi, now_ms)) {
    return false;
  }
  job_.clearAwait();
  return true;
}

bool LinkMeshClient::onReturnedPath(uint8_t path_len, const uint8_t* path) {
  // AgentMesh::onPeerPathRecv: "take the path - the next transaction of this job goes direct".
  // MeshCore's encoding gives a hash size of 1 to 4 and a hop count of 0 to 63; a path of zero
  // hops is a peer that is a direct neighbour, which is the link's mode 1, and a hash size the
  // link does not allow (4, which upstream reserves too - Packet::isValidPathLen) leaves the
  // path as it was.
  const uint8_t size = pathHashSize(path_len);
  const uint8_t count = pathHopCount(path_len);
  if (count == 0) return job_.setPath(kPathDirect, 0, 0, nullptr, 0);
  if (path == nullptr) return false;
  return job_.setPath(kPathHops, size, count, path, size_t(size) * count);
}

// ----------------------------------------------------------------------------- counters (6.4)

void LinkMeshClient::getCounters(link::MeshCountersResponse& out) {
  RadioCounters radio;
  port_.radioCounters(radio);
  out.rx = radio.rx;
  out.tx = radio.tx;
  out.client_tx = client_tx_;
  out.noise_floor = radio.noise_floor;
  out.last_rssi = radio.last_rssi;
  out.last_snr_q4 = radio.last_snr_q4;
  // adverts, adverts_dropped and obs_dropped are the observation ring's (task 03) and the
  // session's to fill; they are left as `out` had them.
}

}  // namespace meshclient
}  // namespace twinoak
