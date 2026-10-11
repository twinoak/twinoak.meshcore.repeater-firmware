// Every message of protocol 1.0 as a plain struct and the one function this firmware needs for
// it: a decoder for every request, an encoder for every response and every event
// (spec/LINK-PROTOCOL.md sections 5 to 8). In 1.0 only the Walter sends requests and only the RAK
// sends events, so the other half of each pair is the Walter's firmware's and is not here.
//
// Host-portable: no Arduino, no MeshCore, no heap, no recursion. The rules every function here
// keeps:
//
//   - A decoder reads in a straight line through a Reader that cannot read past the payload it was
//     given, then checks lengths and legal values. It returns false - *malformed*, the session's
//     kErrMalformed - rather than reading out of bounds.
//   - Trailing bytes a decoder does not know are ignored, because a later minor version adds
//     fields at the end of a payload (LINK-PROTOCOL.md 5.1, "Versions").
//   - A value whose own section names another status is decoded and passed up, not refused here:
//     REBOOT's mode and SEND_TRACE's hash_size are kErrRange when the board cannot do them
//     (sections 5.5 and 6.2), and that is the session's answer to give.
//   - Every view a decoder hands back (a str8's characters, a password, a path's hashes) points
//     into the payload it was given and lives exactly as long as that payload's bytes do.
//   - An encoder writes the whole payload or nothing, and returns the number of bytes written - 0
//     when a field is not legal or the buffer is too small. Every response payload begins with
//     status; when status is not kOk the payload is that one byte and nothing else (section 3),
//     which encodeStatus writes.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "common/link/codec/Bytes.h"
#include "common/link/codec/Numbers.h"

namespace twinoak {
namespace link {

// ----------------------------------------------------------------------------- shared pieces

// A path on this link (LINK-PROTOCOL.md section 6.1). hash_size and hop_count are 0 unless mode
// is kPathHops; a decoder reads the two bytes whatever the mode says and reports 0 for them, as
// the reference implementation does (spec/tools/linkref.py, dec_path).
struct Path {
  uint8_t mode = kPathNone;
  uint8_t hash_size = 0;
  uint8_t hop_count = 0;
  Bytes hashes;  // hash_size x hop_count bytes, outward order; empty unless mode is kPathHops
};

// The body of a HELLO response, which EV_BOOT repeats after its reset reason (section 5.1).
struct HelloBody {
  uint8_t proto_major = kProtoMajor;
  uint8_t proto_minor = kProtoMinor;
  uint8_t role = kRoleUnknown;
  uint8_t slot = 0;
  uint8_t max_payload = uint8_t(kMaxPayload);
  uint32_t caps = 0;
  uint32_t boot_id = 0;
  Bytes fw_version;
  Bytes board;
  Bytes upstream;
};

// A reply record: what REPLY_GET answers and what EV_MESH_REPLY carries (section 6.3).
struct ReplyRecord {
  uint8_t state = kReplyNone;
  uint32_t tag = 0;
  uint8_t via_path = 0;
  int8_t snr_q4 = 0;
  int16_t rssi = kRssiUnknown;
  uint32_t elapsed_ms = 0;
  // state kReplyResponse: the response after its four tag bytes. state kReplyText: the text.
  Bytes data;
  // state kReplyTrace, as MeshCore delivers a completed trace.
  uint8_t trace_flags = 0;
  int8_t final_snr_q4 = 0;
  Bytes hashes;
  Bytes snrs;
};

// One observation record of an OBS_PULL response (section 7). The name inside app_data is
// something a stranger chose: neither end logs it.
struct Observation {
  const uint8_t* pubkey = nullptr;  // kPubKeyLen bytes
  uint32_t adv_ts = 0;
  uint32_t age_s = 0;
  int8_t snr_q4 = 0;
  int16_t rssi = kRssiUnknown;
  uint8_t path_len = 0;  // MeshCore's encoding: see obsHashSize/obsHopCount in Numbers.h
  Bytes path;            // obsPathBytes(path_len) bytes
  Bytes app_data;        // 0 to kObsAppDataMax bytes, untouched
};

// ----------------------------------------------------------------------------- requests (decode)

struct Hello {
  uint8_t proto_major = 0;
  uint8_t proto_minor = 0;
  uint8_t peer_kind = 0;
  uint8_t max_payload = 0;
  Bytes fw_version;
};

struct Ping {
  uint32_t nonce = 0;
};

struct TimeSet {
  uint32_t epoch = 0;
  uint8_t source = 0;
  uint8_t flags = 0;
};

struct Reboot {
  uint8_t mode = 0;
  uint16_t delay_ms = 0;
};

struct SleepNotice {
  uint32_t expect_back_s = 0;
};

struct Cli {
  Bytes text;  // one command line, 1 to kCliLineMax bytes of UTF-8, no terminator
};

struct LppGet {
  uint8_t perm_mask = 0;
};

struct JobBegin {
  const uint8_t* pubkey = nullptr;  // kPubKeyLen bytes, into the payload
  Path path;
};

struct SendLogin {
  uint32_t timeout_ms = 0;
  Bytes password;  // 0 to kLoginPasswordMax bytes; empty is the ACL login
};

struct SendRequest {
  uint32_t timeout_ms = 0;
  uint8_t req_type = 0;
  Bytes data;
};

struct SendCli {
  uint32_t timeout_ms = 0;
  uint8_t flags = 0;  // bit 0 sensitive: the line holds a password
  Bytes text;         // 1 to kSendCliTextMax bytes
};

struct SendDiscovery {
  uint32_t timeout_ms = 0;
};

struct SendTrace {
  uint32_t timeout_ms = 0;
  uint8_t hash_size = 0;  // 1, 2, 4 or 8; 3 is kErrRange and the session's to refuse
  Bytes hashes;
};

struct ObsPull {
  uint8_t max = 0;
};

// STATUS, TIME_GET, IDENTITY, POWER_GET, ENV_GET, JOB_END, PATH_GET, REPLY_GET and MESH_COUNTERS
// carry no fields. Their payload is empty, and trailing bytes a newer Walter may add are ignored
// (section 5.1) - so this accepts anything and exists to say that in one place.
bool decodeNoFields(Bytes payload);

bool decodeHello(Bytes payload, Hello& out);
bool decodePing(Bytes payload, Ping& out);
bool decodeTimeSet(Bytes payload, TimeSet& out);
bool decodeReboot(Bytes payload, Reboot& out);
bool decodeSleepNotice(Bytes payload, SleepNotice& out);
bool decodeCli(Bytes payload, Cli& out);
bool decodeLppGet(Bytes payload, LppGet& out);
bool decodeJobBegin(Bytes payload, JobBegin& out);
bool decodePathSet(Bytes payload, Path& out);
bool decodeSendLogin(Bytes payload, SendLogin& out);
bool decodeSendRequest(Bytes payload, SendRequest& out);
bool decodeSendCli(Bytes payload, SendCli& out);
bool decodeSendDiscovery(Bytes payload, SendDiscovery& out);
bool decodeSendTrace(Bytes payload, SendTrace& out);
bool decodeObsPull(Bytes payload, ObsPull& out);

// ----------------------------------------------------------------------------- responses (encode)

struct PingResponse {
  uint32_t nonce = 0;  // echoed
  uint32_t uptime_s = 0;
};

struct StatusResponse {
  uint32_t uptime_s = 0;
  uint8_t reset_reason = kResetUnknown;
  uint8_t time_state = kTimeUnset;
  uint32_t epoch = 0;
  uint16_t flags = 0;
  uint8_t obs_pending = 0;
  uint16_t obs_dropped = 0;   // saturating, since boot
  uint16_t link_errors = 0;   // saturating, since boot
  uint8_t attention = kAttentionNone;
};

struct TimeSetResponse {
  uint32_t prev_epoch = 0;
  uint32_t epoch = 0;
  uint8_t time_state = kTimeUnset;
};

struct TimeGetResponse {
  uint32_t epoch = 0;
  uint8_t time_state = kTimeUnset;
};

struct IdentityResponse {
  const uint8_t* pubkey = nullptr;  // kPubKeyLen bytes
  Bytes name;
};

struct CliResponse {
  Bytes text;  // 0 to kCliReplyMax bytes
};

struct PowerResponse {
  struct Channel {
    uint16_t bus_mv = 0;
    int16_t current_ma = 0;
  };
  uint8_t valid = 0;  // one bit per channel; a channel whose bit is clear reads 0
  Channel channels[kPowerChannels];
};

struct EnvResponse {
  uint8_t valid = 0;
  int16_t temp_centi = 0;
  uint16_t rh_centi = 0;
  uint32_t pressure_pa = 0;
};

struct LppResponse {
  Bytes lpp;  // the Cayenne-LPP bytes a mesh client would have been told
};

// The answer to every one of the five sends (section 6.2).
struct SendResponse {
  uint8_t send = kSendNothing;
  uint32_t tag = 0;
  uint32_t sender_ts = 0;
};

struct ObsPullResponse {
  uint8_t remaining = 0;
  uint16_t dropped = 0;  // lost since the last OBS_PULL
  const Observation* records = nullptr;
  uint8_t count = 0;  // records handed out, oldest first
};

struct MeshCountersResponse {
  uint32_t rx = 0;
  uint32_t tx = 0;
  uint32_t client_tx = 0;  // one per send, and nothing else
  uint32_t adverts = 0;
  uint32_t adverts_dropped = 0;
  uint32_t obs_dropped = 0;
  int16_t noise_floor = 0;
  int16_t last_rssi = 0;
  int8_t last_snr_q4 = 0;
};

struct Attention {
  uint8_t reason = kAttentionNone;
  uint16_t detail = 0;
  uint8_t repeat = 0;  // counting from 0
};

// Any response whose status is not kOk - and the whole payload of the responses that have no body:
// REBOOT, SLEEP_NOTICE, JOB_BEGIN, JOB_END and PATH_SET.
size_t encodeStatus(uint8_t status, uint8_t* out, size_t cap);

size_t encodeHelloResponse(const HelloBody& in, uint8_t* out, size_t cap);
size_t encodePingResponse(const PingResponse& in, uint8_t* out, size_t cap);
size_t encodeStatusResponse(const StatusResponse& in, uint8_t* out, size_t cap);
size_t encodeTimeSetResponse(const TimeSetResponse& in, uint8_t* out, size_t cap);
size_t encodeTimeGetResponse(const TimeGetResponse& in, uint8_t* out, size_t cap);
size_t encodeIdentityResponse(const IdentityResponse& in, uint8_t* out, size_t cap);
size_t encodeCliResponse(const CliResponse& in, uint8_t* out, size_t cap);
size_t encodePowerResponse(const PowerResponse& in, uint8_t* out, size_t cap);
size_t encodeEnvResponse(const EnvResponse& in, uint8_t* out, size_t cap);
size_t encodeLppResponse(const LppResponse& in, uint8_t* out, size_t cap);
size_t encodePathGetResponse(const Path& in, uint8_t* out, size_t cap);
size_t encodeSendResponse(const SendResponse& in, uint8_t* out, size_t cap);
size_t encodeReplyGetResponse(const ReplyRecord& in, uint8_t* out, size_t cap);
size_t encodeObsPullResponse(const ObsPullResponse& in, uint8_t* out, size_t cap);
size_t encodeMeshCountersResponse(const MeshCountersResponse& in, uint8_t* out, size_t cap);

// ----------------------------------------------------------------------------- events (encode)

// EV_BOOT: the reset reason and then the HELLO response body without its status byte (5.1).
size_t encodeEvBoot(uint8_t reset_reason, const HelloBody& in, uint8_t* out, size_t cap);
size_t encodeEvMeshReply(const ReplyRecord& in, uint8_t* out, size_t cap);
size_t encodeEvAttention(const Attention& in, uint8_t* out, size_t cap);

}  // namespace link
}  // namespace twinoak
