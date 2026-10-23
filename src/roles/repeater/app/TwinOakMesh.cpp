#include "roles/repeater/app/TwinOakMesh.h"

#include <string.h>

#include "roles/repeater/clock/BoardClock.h"
#include "roles/repeater/link/BoardLink.h"
#include "roles/repeater/meshclient/PeerSlot.h"

// The mesh client's portable constants are upstream's own numbers, held together here - the one
// file that sees both - so that a pin bump which changes one cannot leave the other behind.
static_assert(twinoak::meshclient::kPeerKeyLen == PUB_KEY_SIZE, "a peer's key is PUB_KEY_SIZE");
static_assert(twinoak::meshclient::kSecretLen == PUB_KEY_SIZE, "a shared secret is PUB_KEY_SIZE");
static_assert(twinoak::meshclient::kPathBytesMax == MAX_PATH_SIZE, "a path is MAX_PATH_SIZE");
static_assert(twinoak::meshclient::kMaxPacketPayload == MAX_PACKET_PAYLOAD,
              "a packet's payload is MAX_PACKET_PAYLOAD");
static_assert(twinoak::meshclient::kMaxDatagramPlaintext ==
                  MAX_PACKET_PAYLOAD - CIPHER_MAC_SIZE - (CIPHER_BLOCK_SIZE - 1),
              "Mesh::createDatagram's bound on a plaintext (src/Mesh.cpp:490)");
static_assert(twinoak::meshclient::kReplyDataMax == MAX_PACKET_PAYLOAD,
              "a response's data is at most a packet");
static_assert(twinoak::meshclient::kTraceSnrsMax == MAX_PATH_SIZE,
              "a trace's SNRs live in Packet::path");
static_assert(twinoak::meshclient::kPayloadTypeReq == PAYLOAD_TYPE_REQ, "PAYLOAD_TYPE_REQ");
static_assert(twinoak::meshclient::kPayloadTypeTxtMsg == PAYLOAD_TYPE_TXT_MSG,
              "PAYLOAD_TYPE_TXT_MSG");
static_assert(twinoak::meshclient::kPayloadTypeAnonReq == PAYLOAD_TYPE_ANON_REQ,
              "PAYLOAD_TYPE_ANON_REQ");
static_assert(twinoak::meshclient::kTxtTypeCliData == TXT_TYPE_CLI_DATA, "TXT_TYPE_CLI_DATA");
static_assert(twinoak::meshclient::kTxtTypePlain == TXT_TYPE_PLAIN, "TXT_TYPE_PLAIN");
// The observation ring's three (src/roles/repeater/meshclient/ObservationRing.h). The app_data cap is the one
// that matters: Mesh::onRecvPacket clamps an advert's app_data to MAX_ADVERT_DATA_SIZE before
// onAdvertRecv sees it (src/Mesh.cpp:269), and spec/LINK-PROTOCOL.md section 7 caps a record's
// app_len at 32 - so while the two agree nothing is ever truncated, and a bump that raises
// upstream's is a compile error here rather than a silently shortened record.
static_assert(twinoak::meshclient::kObsAppDataMax == MAX_ADVERT_DATA_SIZE,
              "an advert's app_data is MAX_ADVERT_DATA_SIZE");
static_assert(twinoak::meshclient::kObsPathBytesMax == MAX_PATH_SIZE,
              "a record's path is MAX_PATH_SIZE");
// The note table is as long as the packet pool, so a note cannot be pushed out while its own
// packet still waits in the inbound queue (examples/simple_repeater/MyMesh.cpp:864 at the pin
// constructs `new StaticPoolPacketManager(32)`; the number is not a macro, so it is cited and
// held here as a literal).
static_assert(twinoak::meshclient::kRssiNotes == 32, "the packet pool of simple_repeater");
// REQ_TYPE_GET_TELEMETRY_DATA is a define of MyMesh.cpp alone (examples/simple_repeater/
// MyMesh.cpp:48), not of a header, so it is cited and not asserted: 0x03. TELEM_PERM_BASE is
// src/helpers/SensorManager.h's.
static_assert(twinoak::meshclient::kTelemPermBase == TELEM_PERM_BASE, "TELEM_PERM_BASE");

// Every body calls its base and, in the four that can put this node's clock on the air, tells the
// clock keeper so first - before the base call, so that there is no window in which upstream has
// replied and the keeper does not know (TwinOakMesh.h says which four and why).
//
// The four peer overrides are where the mesh client's peer sits in front of upstream's. The
// shape is the same in each, and it is the shape that makes the repeater provably undisturbed:
//
//     if (!extra_) return upstream's;                       // no reply awaited: nothing of ours
//     if (ours(idx)) take it;  else upstream's(idx - 1);    // a reply awaited: the shift
//
// `extra_` is decided once per packet, in searchPeersByHash(), which Mesh::onRecvPacket calls
// before the other three (src/Mesh.cpp:149, then :154, :173, :181 at the pin 81dbae59) - the
// same contract upstream's own MyMesh relies on for its matching_peer_indexes[].
//
// The bound. Upstream's match array is `int matching_peer_indexes[MAX_CLIENTS]`
// (examples/simple_repeater/MyMesh.h:112), written only by MyMesh::searchPeersByHash
// (MyMesh.cpp:623-631, one entry per ACL client that matches, so n <= acl.getNumClients() <=
// MAX_CLIENTS) and read only by MyMesh::getPeerSharedSecret, onPeerDataRecv and onPeerPathRecv
// with the index they are given (MyMesh.cpp:634, :665, :770, each range-checked against
// acl.getNumClients()). Those three only ever see an index this file has shifted back into
// 0..n-1, so the extra peer is never an index into that array. The count we return, n + 1, is a
// loop bound in Mesh::onRecvPacket and nothing else: its loop (src/Mesh.cpp:152-186) keeps a
// `secret[PUB_KEY_SIZE]` and a `data[MAX_PACKET_PAYLOAD]` per iteration and no array sized by the
// count, and the "(max 4 matches supported ATM)" of its comment (:148) is a comment - nothing in
// the function bounds `num`. MAX_CLIENTS itself is 32 in this build: since repeater-v1.18.0 it
// is defined once, in helpers/ClientACL.h:44-45 (MyMesh.h's own `#define MAX_CLIENTS 32` is
// gone - upstream's #3233), and nothing in platformio.ini defines it. At repeater-v1.17.1 it was
// 20, ClientACL.h's 20 winning over MyMesh.h's 32 by include order; tasks/phase-2/reports/01.md
// quotes those lines. Nothing here depends on the number: the count we return is a loop bound
// and the shift is by one, whatever the array's length.

int TwinOakMesh::searchPeersByHash(const uint8_t* hash) {
  const int n = MyMesh::searchPeersByHash(hash);
  // With no job, or a job with no reply awaited - which is every moment of a repeater's life but
  // the seconds between a send of the mesh client's and its reply - `extra_` is false, this
  // function returns upstream's count and nothing else, and the three overrides below hand
  // every index to upstream unchanged. That sentence is the proof that the repeater is
  // undisturbed.
  //
  // mesh::Identity::isHashMatch (src/Identity.h:27) compares the PATH_HASH_SIZE byte(s) of a key
  // with the packet's source hash, which is how upstream's own ACL answers the same question
  // (MyMesh.cpp:626); the 32-byte copy into a temporary Identity is the price of asking it the
  // same way, and it is paid only while a reply is awaited.
  extra_ = job_.begun() && job_.awaiting() && mesh::Identity(job_.peerKey()).isHashMatch(hash);
  return twinoak::meshclient::peerSlotTotal(n, extra_);
}

void TwinOakMesh::getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) {
  if (!extra_) {
    MyMesh::getPeerSharedSecret(dest_secret, peer_idx);  // upstream's, untouched
    return;
  }
  if (twinoak::meshclient::peerSlotIsOurs(peer_idx, extra_)) {
    memcpy(dest_secret, sharedSecret(), PUB_KEY_SIZE);
    return;
  }
  MyMesh::getPeerSharedSecret(dest_secret,
                              twinoak::meshclient::peerSlotUpstreamIndex(peer_idx, extra_));
}

int8_t TwinOakMesh::snrQ4Of(const mesh::Packet* pkt) {
  // Packet::_snr is the radio's SNR times four, stored when the packet was received
  // (src/Dispatcher.cpp:206 at the pin); getSNR() divides it back, and the agent firmware's
  // record multiplies again. The same number, taken the same way.
  return int8_t(pkt->getSNR() * 4.0f);
}

int16_t TwinOakMesh::rssiOf(const mesh::Packet* pkt) {
  // The note logRx took, taken back once (src/roles/repeater/meshclient/RssiNotes.h). There is none for a
  // packet this node built itself, and none for one whose note was pushed out of the table -
  // which the record then says with 0x7FFF, as section 6.3 and section 7 both allow.
  int16_t rssi = 0;
  return rssi_notes_.take(pkt, rssi) ? rssi : twinoak::meshclient::kReplyRssiUnknown;
}

void TwinOakMesh::onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx,
                                 const uint8_t* secret, uint8_t* data, size_t len) {
  if (extra_ && twinoak::meshclient::peerSlotIsOurs(sender_idx, extra_)) {
    // A packet from the job's peer that verified with the job's secret, while a reply is
    // awaited: a RESPONSE (to a login, a request or a discovery) or a TXT_MSG (the reply to a CLI
    // line). The client decides whether it is the awaited one (src/roles/repeater/meshclient/Reply.h); whatever
    // it is, upstream never sees it - a REQ from the peer in this window is dropped here too,
    // which is the price a single-peer client pays (ARCHITECTURE section 2) - and so the clock
    // keeper is *not* told: nothing carrying our clock is answered.
    //
    // The RSSI is the note logRx took against this packet (task 03): taken once, here, because
    // this is where the reply is made of it. A packet with no note - one that arrived before the
    // table had room - reads 0x7FFF, which section 6.3 names as "not known".
    (void)secret;
    const int16_t rssi = rssiOf(packet);
    if (type == PAYLOAD_TYPE_RESPONSE) {
      client_.onResponse(data, len, /*via_path=*/false, snrQ4Of(packet), rssi, millis());
    } else if (type == PAYLOAD_TYPE_TXT_MSG) {
      client_.onText(data, len, snrQ4Of(packet), rssi, millis());
    }
    return;
  }
  twinoak::clock::noteTimestampEmitted();
  MyMesh::onPeerDataRecv(packet, type,
                         twinoak::meshclient::peerSlotUpstreamIndex(sender_idx, extra_), secret,
                         data, len);
}

bool TwinOakMesh::onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret,
                                 uint8_t* path, uint8_t path_len, uint8_t extra_type,
                                 uint8_t* extra, uint8_t extra_len) {
  if (extra_ && twinoak::meshclient::peerSlotIsOurs(sender_idx, extra_)) {
    // A returned path from the job's peer: the answer to a flooded send, with the response riding
    // in `extra` (MyMesh::onPeerDataRecv builds it so at MyMesh.cpp:680-683, and onAnonDataRecv
    // at :590-594, with PAYLOAD_TYPE_RESPONSE as the extra type). The path is learned first - "the
    // job's path becomes that path before the event is sent" (6.3) - then the reply is taken.
    //
    // **false**: no reciprocal path is ever sent for the client's peer - one packet per send is
    // the rail (S-12), and Mesh::onRecvPacket (src/Mesh.cpp:173-178) sends one only when this
    // returns true. The repeater that answered us has already learnt what it needs from the
    // request it answered.
    (void)secret;
    client_.onReturnedPath(path_len, path);
    if (extra_type == PAYLOAD_TYPE_RESPONSE && extra != nullptr) {
      client_.onResponse(extra, extra_len, /*via_path=*/true, snrQ4Of(packet), rssiOf(packet),
                         millis());
    }
    return false;
  }
  twinoak::clock::noteTimestampEmitted();
  return MyMesh::onPeerPathRecv(packet,
                                twinoak::meshclient::peerSlotUpstreamIndex(sender_idx, extra_),
                                secret, path, path_len, extra_type, extra, extra_len);
}

void TwinOakMesh::onTraceRecv(mesh::Packet* packet, uint32_t tag, uint32_t auth_code,
                              uint8_t flags, const uint8_t* path_snrs,
                              const uint8_t* path_hashes, uint8_t path_len) {
  // A direct TRACE that reached the end of its path here (src/Mesh.cpp:56-57): `path_len` is the
  // byte count of the round-trip hashes, and the number of hops - one SNR byte each in
  // `path_snrs` - is that count over the hash size the flags byte states as an exponent (:50,
  // :55), as the agent firmware's onTraceRecv reads it. The client takes it only while a trace
  // with this tag is awaited; a stranger's trace that ends here is dropped unread.
  (void)auth_code;
  const size_t hops = size_t(path_len) >> (flags & 0x03);
  client_.onTrace(tag, flags, path_hashes, path_len, path_snrs, hops, snrQ4Of(packet),
                  rssiOf(packet), millis());
  MyMesh::onTraceRecv(packet, tag, auth_code, flags, path_snrs, path_hashes, path_len);
}

const uint8_t* TwinOakMesh::sharedSecret() {
  if (!job_.hasSecret()) {
    // Once per job, on first need: the X25519 secret between this node's key and the peer's
    // (mesh::LocalIdentity::calcSharedSecret(uint8_t* secret, const uint8_t* other_pub_key),
    // src/Identity.h:81; what upstream's ACL pre-computes per client at MyMesh.cpp's putClient).
    // It goes into the job's own buffer so that Job::end() is the one place it is wiped.
    self_id.calcSharedSecret(job_.secretBuffer(), job_.peerKey());
    job_.markSecret();
  }
  return job_.secretBuffer();
}

// ----------------------------------------------------------------------------- the sends

uint32_t TwinOakMesh::uniqueTime() {
  // "All timestamps the client sends come from the RAK's clock through MeshCore's unique-time
  // source" (6.2): RTCClock::getCurrentTimeUnique (src/MeshCore.h:119-125 at the pin), the same
  // call BaseChatMesh::sendLogin/sendRequest make (src/helpers/BaseChatMesh.cpp:608, :669) and
  // the same one the repeater stamps its own replies with.
  return getRTCClock()->getCurrentTimeUnique();
}

void TwinOakMesh::randomBytes(uint8_t* dest, size_t len) { getRNG()->random(dest, len); }

bool TwinOakMesh::dispatch(mesh::Packet* pkt, const twinoak::meshclient::Route& route) {
  if (pkt == nullptr) return false;  // the pool was empty: nothing was built, nothing is sent
  // The packet carries this node's unique second: a backwards TIME_SET is unsafe from here on
  // (src/roles/repeater/clock/ClockKeeper.h), and the keeper is told before the packet is in the queue.
  twinoak::clock::noteTimestampEmitted();
  if (route.direct) {
    // Mesh::sendDirect (src/Mesh.cpp:693-715): ROUTE_TYPE_DIRECT, the path copied into the
    // packet (path_len 0 is zero hops), marked seen, queued at priority 0.
    sendDirect(pkt, route.path, route.path_len);
  } else {
    // Mesh::sendFlood (src/Mesh.cpp:637-662): ROUTE_TYPE_FLOOD with the path hash size and a
    // count of 0 (:649), marked seen, queued at priority 1. **Two-byte hashes** (S-12): upstream
    // defaults the argument to 1 (src/Mesh.h), the agent firmware passes 2, and so does this.
    sendFlood(pkt, 0, twinoak::meshclient::kFloodPathHashSize);
  }
  return true;
}

bool TwinOakMesh::sendAnonRequest(const uint8_t* plain, size_t len,
                                  const twinoak::meshclient::Route& route) {
  // Mesh::createAnonDatagram (src/Mesh.cpp:512-538): PAYLOAD_TYPE_ANON_REQ, the peer's hash,
  // this node's **public key** in the clear, then the plaintext encrypted and MACed with the
  // ECDH secret of the two - which the receiving repeater recomputes from that key
  // (Mesh::onRecvPacket, :208-211) before MyMesh::onAnonDataRecv reads the login. The same
  // primitive BaseChatMesh::sendLogin uses (src/helpers/BaseChatMesh.cpp:621).
  mesh::Packet* pkt = createAnonDatagram(PAYLOAD_TYPE_ANON_REQ, self_id,
                                         mesh::Identity(job_.peerKey()), sharedSecret(), plain,
                                         len);
  return dispatch(pkt, route);
}

bool TwinOakMesh::sendDatagram(uint8_t payload_type, const uint8_t* plain, size_t len,
                               const twinoak::meshclient::Route& route) {
  // Mesh::createDatagram (src/Mesh.cpp:488-510): the type (REQ or TXT_MSG here), the peer's hash,
  // this node's hash, then the plaintext encrypted and MACed with the job's secret - which the
  // repeater has from the login (MyMesh::handleLoginReq copies it into the ACL entry,
  // MyMesh.cpp:122). The same primitive BaseChatMesh::sendRequest and sendCommandData use
  // (src/helpers/BaseChatMesh.cpp:673, :501).
  mesh::Packet* pkt =
      createDatagram(payload_type, mesh::Identity(job_.peerKey()), sharedSecret(), plain, len);
  return dispatch(pkt, route);
}

bool TwinOakMesh::sendTrace(uint32_t tag, uint32_t auth_code, uint8_t flags, const uint8_t* path,
                            uint8_t path_bytes) {
  // Mesh::createTrace (src/Mesh.cpp:605-619): a 9-byte payload - tag, auth code, flags - to which
  // sendDirect appends the path (:698-701): "for TRACE packets, path is appended to end of
  // PAYLOAD (path is used for SNR's)", and the packet's own path_len is 0 for the hops to fill
  // with their SNRs. Always direct: sendFlood refuses a TRACE outright (:638-641).
  mesh::Packet* pkt = createTrace(tag, auth_code, flags);
  twinoak::meshclient::Route route;
  route.direct = true;
  route.path = path;
  route.path_len = path_bytes;  // a byte count here, not the encoded path_len: sendDirect's TRACE
                                // branch reads it as one (:700)
  return dispatch(pkt, route);
}

void TwinOakMesh::radioCounters(twinoak::meshclient::RadioCounters& out) {
  // RadioLibWrapper's own counters (src/helpers/radiolib/RadioLibWrappers.h at the pin):
  // getPacketsRecv/getPacketsSent count every packet the radio received or transmitted, forwards
  // included; getNoiseFloor is its calibrated floor in dBm; getLastRSSI/getLastSNR are the last
  // packet's, which is what the repeater's own `status` reports too (MyMesh.cpp:231).
  out.rx = radio_driver.getPacketsRecv();
  out.tx = radio_driver.getPacketsSent();
  out.noise_floor = int16_t(radio_driver.getNoiseFloor());
  out.last_rssi = int16_t(radio_driver.getLastRSSI());
  out.last_snr_q4 = int8_t(radio_driver.getLastSNR() * 4.0f);
}

// ----------------------------------------------------------------------------- the rest

void TwinOakMesh::onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret,
                                 const mesh::Identity& sender, uint8_t* data, size_t len) {
  twinoak::clock::noteTimestampEmitted();
  MyMesh::onAnonDataRecv(packet, secret, sender, data, len);
}

void TwinOakMesh::onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id,
                               uint32_t timestamp, const uint8_t* app_data,
                               size_t app_data_len) {
  // **The base first, before anything of ours.** Upstream's neighbour table is the repeater's
  // own - MyMesh::onAdvertRecv chains to mesh::Mesh and puts a zero-hop repeater advert in it
  // (MyMesh.cpp:650-660 at the pin 81dbae59) - and it must see every advert exactly as it did
  // before this firmware existed. The ring is what we add after it, and nothing here can change
  // what upstream made of the advert.
  MyMesh::onAdvertRecv(packet, id, timestamp, app_data, app_data_len);

  // An advert from this node's own key is not recorded: it is this node's voice coming back, not
  // an observation of anybody. Mesh::onRecvPacket never delivers one - it drops a self advert
  // before the signature check (src/Mesh.cpp:263, `self_id.matches(id.pub_key)`) - so this is the
  // guard that says so in our own code rather than resting on upstream's.
  if (self_id.matches(id.pub_key)) return;

  twinoak::meshclient::HeardAdvert heard;
  memcpy(heard.pubkey, id.pub_key, PUB_KEY_SIZE);
  heard.adv_ts = timestamp;  // the advertiser's own clock, not ours
  heard.heard_ms = millis();
  heard.snr_q4 = snrQ4Of(packet);
  heard.rssi = rssiOf(packet);
  // The path as it arrived, in MeshCore's own encoding: Packet::path_len is the byte, and
  // getPathByteLen() is getPathHashCount() * getPathHashSize() (src/Packet.h:79-81). Empty for a
  // direct neighbour. Dispatcher::tryParsePacket has already refused a packet whose path is
  // longer than MAX_PATH_SIZE (src/Dispatcher.cpp:172-176), so the copy below cannot be clamped
  // on this pin; the clamp is what makes a bump that widens the path a counted drop instead.
  heard.path_len = packet->path_len;
  uint8_t path_bytes = packet->getPathByteLen();
  if (path_bytes > twinoak::meshclient::kObsPathBytesMax) {
    path_bytes = twinoak::meshclient::kObsPathBytesMax;  // the record then fails its own check
  }
  if (path_bytes > 0) memcpy(heard.path, packet->path, path_bytes);
  // The advert's application data: flags, position and **the name a stranger chose**. It is
  // copied and nothing else - never parsed and never printed (section 7, S-12). Mesh.cpp:269 has
  // already held it to MAX_ADVERT_DATA_SIZE, which the static_assert above holds to the record's.
  size_t app_len = app_data_len;
  if (app_len > twinoak::meshclient::kObsAppDataMax) app_len = twinoak::meshclient::kObsAppDataMax;
  if (app_len > 0) memcpy(heard.app_data, app_data, app_len);
  heard.app_len = uint8_t(app_len);

  // Into the ring, and section 8's reason 5 raised when the ring has just reached three quarters
  // full with `obs.wake` on and the Walter believed asleep (src/roles/repeater/link/BoardLink.cpp).
  twinoak::link::noteAdvert(heard);
}

void TwinOakMesh::logRx(mesh::Packet* pkt, int len, float score) {
  MyMesh::logRx(pkt, len, score);
  // The one moment the radio's "last RSSI" still belongs to this packet: Dispatcher::checkRecv
  // calls this immediately after parsing the frame (src/Dispatcher.cpp:238) and only then decides
  // whether to hold a flood packet back in the inbound queue, for up to MAX_RX_DELAY_MILLIS -
  // 32 seconds - before onRecvPacket sees it (:243-253). So the number is noted here against the
  // packet and taken back where the advert or the reply is made of it
  // (src/roles/repeater/meshclient/RssiNotes.h; the agent firmware's AgentMesh::logRx does the same).
  rssi_notes_.note(pkt, int16_t(radio_driver.getLastRSSI()));
}

void TwinOakMesh::logTx(mesh::Packet* pkt, int len) {
  MyMesh::logTx(pkt, len);
}

void TwinOakMesh::sendSelfAdvertisement(int delay_millis, bool flood) {
  twinoak::clock::noteTimestampEmitted();
  MyMesh::sendSelfAdvertisement(delay_millis, flood);
}

// --- the first boot's one write (TwinOakMesh.h says why) ---------------------------------------

void TwinOakMesh::savePrefs() {
  if (prefs_save_held_) {
    prefs_save_pending_ = true;  // one write, at releasePrefsSaves()
    return;
  }
  MyMesh::savePrefs();
}

void TwinOakMesh::holdPrefsSaves() {
  prefs_save_held_ = true;
  prefs_save_pending_ = false;
}

bool TwinOakMesh::releasePrefsSaves() {
  prefs_save_held_ = false;
  if (!prefs_save_pending_) return false;
  prefs_save_pending_ = false;
  MyMesh::savePrefs();
  return true;
}
