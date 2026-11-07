#include "common/link/codec/Messages.h"

namespace twinoak {
namespace link {
namespace {

// ----------------------------------------------------------------------------- shared pieces

// A path, as section 6.1 has it: mode, hash_size, hop_count and the hops. The two size bytes are
// on the wire whatever the mode says.
bool readPath(Reader& r, Path& out) {
  out.mode = r.u8();
  const uint8_t size = r.u8();
  const uint8_t count = r.u8();
  if (!r.ok() || out.mode > kPathHops) return false;
  if (out.mode != kPathHops) {
    out.hash_size = 0;
    out.hop_count = 0;
    out.hashes = Bytes();
    return true;
  }
  if (size < kPathHashSizeMin || size > kPathHashSizeMax) return false;
  if (count < kPathHopCountMin || count > kPathHopCountMax) return false;
  const unsigned bytes = unsigned(size) * unsigned(count);
  if (bytes > kPathHashesMax) return false;
  out.hash_size = size;
  out.hop_count = count;
  out.hashes = r.take(bytes);
  return r.ok();
}

bool putPath(Writer& w, const Path& p) {
  if (p.mode > kPathHops) return false;
  if (p.mode != kPathHops) {
    w.u8(p.mode);
    w.u8(0);
    w.u8(0);
    return w.ok();
  }
  if (p.hash_size < kPathHashSizeMin || p.hash_size > kPathHashSizeMax) return false;
  if (p.hop_count < kPathHopCountMin || p.hop_count > kPathHopCountMax) return false;
  const unsigned bytes = unsigned(p.hash_size) * unsigned(p.hop_count);
  if (bytes > kPathHashesMax || p.hashes.len != bytes) return false;
  w.u8(p.mode);
  w.u8(p.hash_size);
  w.u8(p.hop_count);
  w.bytes(p.hashes);
  return w.ok();
}

void putHelloBody(Writer& w, const HelloBody& b) {
  w.u8(b.proto_major);
  w.u8(b.proto_minor);
  w.u8(b.role);
  w.u8(b.slot);
  w.u8(b.max_payload);
  w.u32(b.caps);
  w.u32(b.boot_id);
  w.str8(b.fw_version);
  w.str8(b.board);
  w.str8(b.upstream);
  // 1.1's one addition, at the end of the body, and only when the body says 1.1 or later
  // (section 5.1, the `band` row): a 1.0 Walter is answered the bytes it has always been
  // answered. The reference does the same (spec/tools/linkref.py, enc_hello_body).
  if (b.proto_minor >= 1) w.str8(b.band);
}

bool putReply(Writer& w, const ReplyRecord& rec) {
  if (rec.state > kReplyTrace) return false;
  w.u8(rec.state);
  w.u32(rec.tag);
  w.u8(rec.via_path);
  w.i8(rec.snr_q4);
  w.i16(rec.rssi);
  w.u32(rec.elapsed_ms);
  if (rec.state == kReplyResponse || rec.state == kReplyText) {
    w.bytes(rec.data);
  } else if (rec.state == kReplyTrace) {
    w.u8(rec.trace_flags);
    w.i8(rec.final_snr_q4);
    w.str8(rec.hashes);
    w.str8(rec.snrs);
  }
  return w.ok();
}

bool putObservation(Writer& w, const Observation& o) {
  if (o.pubkey == nullptr) return false;
  if (o.path.len != obsPathBytes(o.path_len)) return false;
  if (o.app_data.len > kObsAppDataMax) return false;
  w.raw(o.pubkey, kPubKeyLen);
  w.u32(o.adv_ts);
  w.u32(o.age_s);
  w.i8(o.snr_q4);
  w.i16(o.rssi);
  w.u8(o.path_len);
  w.bytes(o.path);
  w.str8(o.app_data);
  return w.ok();
}

}  // namespace

// ----------------------------------------------------------------------------- requests (decode)

bool decodeNoFields(Bytes payload) {
  (void)payload;  // whatever a newer Walter appended is ignored (section 5.1, "Versions")
  return true;
}

bool decodeHello(Bytes payload, Hello& out) {
  Reader r(payload);
  out.proto_major = r.u8();
  out.proto_minor = r.u8();
  out.peer_kind = r.u8();
  out.max_payload = r.u8();
  out.fw_version = r.str8();
  return r.ok();
}

bool decodePing(Bytes payload, Ping& out) {
  Reader r(payload);
  out.nonce = r.u32();
  return r.ok();
}

bool decodeTimeSet(Bytes payload, TimeSet& out) {
  Reader r(payload);
  out.epoch = r.u32();
  out.source = r.u8();
  out.flags = r.u8();
  return r.ok();
}

bool decodeReboot(Bytes payload, Reboot& out) {
  Reader r(payload);
  out.mode = r.u8();
  out.delay_ms = r.u16();
  return r.ok();
}

bool decodeSleepNotice(Bytes payload, SleepNotice& out) {
  Reader r(payload);
  out.expect_back_s = r.u32();
  return r.ok();
}

bool decodeCli(Bytes payload, Cli& out) {
  Reader r(payload);
  out.text = r.rest();
  // One command line, 1 to 160 bytes of UTF-8 (section 5.7). Neither an empty line nor a longer
  // one is a legal value, and neither is a line that is not text a person could have typed, so
  // all three are malformed rather than run.
  if (!r.ok() || out.text.len < 1 || out.text.len > kCliLineMax) return false;
  if (!isPlainText(out.text)) return false;
  return true;
}

bool decodeLppGet(Bytes payload, LppGet& out) {
  Reader r(payload);
  out.perm_mask = r.u8();
  return r.ok();
}

bool decodeLocalRequest(Bytes payload, LocalRequest& out) {
  Reader r(payload);
  out.req_type = r.u8();
  out.data = r.rest();
  // Section 5.9's two malformed shapes, and the only two: an empty payload - the Reader is not
  // ok() when there was not even a req_type to read - and more than 32 bytes of data. A
  // `req_type` this firmware does not know is **not** malformed: it is kErrRange, and the
  // session decides that from section 5.9's table (isLocalRequestType), the way REBOOT's mode is
  // decided by the board.
  if (!r.ok() || out.data.len > kLocalRequestMaxData) return false;
  return true;
}

bool decodeJobBegin(Bytes payload, JobBegin& out) {
  Reader r(payload);
  const Bytes key = r.take(kPubKeyLen);
  if (!r.ok()) return false;
  out.pubkey = key.ptr;
  return readPath(r, out.path);
}

bool decodePathSet(Bytes payload, Path& out) {
  Reader r(payload);
  return readPath(r, out);
}

bool decodeSendLogin(Bytes payload, SendLogin& out) {
  Reader r(payload);
  out.timeout_ms = r.u32();
  out.password = r.rest();
  if (!r.ok() || out.password.len > kLoginPasswordMax) return false;
  return true;
}

bool decodeSendRequest(Bytes payload, SendRequest& out) {
  Reader r(payload);
  out.timeout_ms = r.u32();
  out.req_type = r.u8();
  out.data = r.rest();
  return r.ok();
}

bool decodeSendCli(Bytes payload, SendCli& out) {
  Reader r(payload);
  out.timeout_ms = r.u32();
  out.flags = r.u8();
  out.text = r.rest();
  // 1 to 148 bytes (section 6.2): what is left of a packet once the timestamp and the flags byte
  // are in it.
  if (!r.ok() || out.text.len < 1 || out.text.len > kSendCliTextMax) return false;
  return true;
}

bool decodeSendDiscovery(Bytes payload, SendDiscovery& out) {
  Reader r(payload);
  out.timeout_ms = r.u32();
  return r.ok();
}

bool decodeSendTrace(Bytes payload, SendTrace& out) {
  Reader r(payload);
  out.timeout_ms = r.u32();
  out.hash_size = r.u8();
  out.hashes = r.rest();
  return r.ok();
}

bool decodeObsPull(Bytes payload, ObsPull& out) {
  Reader r(payload);
  out.max = r.u8();
  return r.ok();
}

// ----------------------------------------------------------------------------- responses (encode)

size_t encodeStatus(uint8_t status, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(status);
  return w.ok() ? w.size() : 0;
}

size_t encodeHelloResponse(const HelloBody& in, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(kOk);
  putHelloBody(w, in);
  return w.ok() ? w.size() : 0;
}

size_t encodePingResponse(const PingResponse& in, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(kOk);
  w.u32(in.nonce);
  w.u32(in.uptime_s);
  return w.ok() ? w.size() : 0;
}

size_t encodeStatusResponse(const StatusResponse& in, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(kOk);
  w.u32(in.uptime_s);
  w.u8(in.reset_reason);
  w.u8(in.time_state);
  w.u32(in.epoch);
  w.u16(in.flags);
  w.u8(in.obs_pending);
  w.u16(in.obs_dropped);
  w.u16(in.link_errors);
  w.u8(in.attention);
  return w.ok() ? w.size() : 0;
}

size_t encodeTimeSetResponse(const TimeSetResponse& in, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(kOk);
  w.u32(in.prev_epoch);
  w.u32(in.epoch);
  w.u8(in.time_state);
  return w.ok() ? w.size() : 0;
}

size_t encodeTimeGetResponse(const TimeGetResponse& in, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(kOk);
  w.u32(in.epoch);
  w.u8(in.time_state);
  return w.ok() ? w.size() : 0;
}

size_t encodeIdentityResponse(const IdentityResponse& in, uint8_t* out, size_t cap) {
  if (in.pubkey == nullptr) return 0;
  Writer w(out, cap);
  w.u8(kOk);
  w.raw(in.pubkey, kPubKeyLen);
  w.str8(in.name);
  return w.ok() ? w.size() : 0;
}

size_t encodeCliResponse(const CliResponse& in, uint8_t* out, size_t cap) {
  if (in.text.len > kCliReplyMax) return 0;  // the reply text, 0 to 160 bytes (section 5.7)
  Writer w(out, cap);
  w.u8(kOk);
  w.bytes(in.text);
  return w.ok() ? w.size() : 0;
}

size_t encodePowerResponse(const PowerResponse& in, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(kOk);
  w.u8(in.valid);
  for (uint8_t i = 0; i < kPowerChannels; i++) {
    w.u16(in.channels[i].bus_mv);
    w.i16(in.channels[i].current_ma);
  }
  return w.ok() ? w.size() : 0;
}

size_t encodeEnvResponse(const EnvResponse& in, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(kOk);
  w.u8(in.valid);
  w.i16(in.temp_centi);
  w.u16(in.rh_centi);
  w.u32(in.pressure_pa);
  return w.ok() ? w.size() : 0;
}

size_t encodeLppResponse(const LppResponse& in, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(kOk);
  w.bytes(in.lpp);
  return w.ok() ? w.size() : 0;
}

size_t encodeLocalResponse(const LocalResponse& in, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(kOk);
  w.bytes(in.data);
  return w.ok() ? w.size() : 0;
}

size_t encodePathGetResponse(const Path& in, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(kOk);
  if (!putPath(w, in)) return 0;
  return w.ok() ? w.size() : 0;
}

size_t encodeSendResponse(const SendResponse& in, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(kOk);
  w.u8(in.send);
  w.u32(in.tag);
  w.u32(in.sender_ts);
  return w.ok() ? w.size() : 0;
}

size_t encodeReplyGetResponse(const ReplyRecord& in, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(kOk);
  if (!putReply(w, in)) return 0;
  return w.ok() ? w.size() : 0;
}

size_t encodeObsPullResponse(const ObsPullResponse& in, uint8_t* out, size_t cap) {
  if (in.count > 0 && in.records == nullptr) return 0;
  Writer w(out, cap);
  w.u8(kOk);
  w.u8(in.remaining);
  w.u16(in.dropped);
  w.u8(in.count);
  for (uint8_t i = 0; i < in.count; i++) {
    if (!putObservation(w, in.records[i])) return 0;
  }
  return w.ok() ? w.size() : 0;
}

size_t encodeMeshCountersResponse(const MeshCountersResponse& in, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(kOk);
  w.u32(in.rx);
  w.u32(in.tx);
  w.u32(in.client_tx);
  w.u32(in.adverts);
  w.u32(in.adverts_dropped);
  w.u32(in.obs_dropped);
  w.i16(in.noise_floor);
  w.i16(in.last_rssi);
  w.i8(in.last_snr_q4);
  return w.ok() ? w.size() : 0;
}

// ----------------------------------------------------------------------------- events (encode)

size_t encodeEvBoot(uint8_t reset_reason, const HelloBody& in, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(reset_reason);
  putHelloBody(w, in);
  return w.ok() ? w.size() : 0;
}

size_t encodeEvMeshReply(const ReplyRecord& in, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  if (!putReply(w, in)) return 0;
  return w.ok() ? w.size() : 0;
}

size_t encodeEvAttention(const Attention& in, uint8_t* out, size_t cap) {
  Writer w(out, cap);
  w.u8(in.reason);
  w.u16(in.detail);
  w.u8(in.repeat);
  return w.ok() ? w.size() : 0;
}

}  // namespace link
}  // namespace twinoak
