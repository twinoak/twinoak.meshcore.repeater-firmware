#include "TwinOakMesh.h"

#include <string.h>

#include "../clock/BoardClock.h"
#include "../meshclient/PeerSlot.h"

// The job's portable constants are upstream's own sizes, held together here - the one file that
// sees both - so that a pin bump which changes one cannot leave the other behind.
static_assert(twinoak::meshclient::kPeerKeyLen == PUB_KEY_SIZE, "a peer's key is PUB_KEY_SIZE");
static_assert(twinoak::meshclient::kSecretLen == PUB_KEY_SIZE, "a shared secret is PUB_KEY_SIZE");
static_assert(twinoak::meshclient::kPathBytesMax == MAX_PATH_SIZE, "a path is MAX_PATH_SIZE");

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
// before the other three (src/Mesh.cpp:149, then :154, :173, :181 at the pin d9296435) - the
// same contract upstream's own MyMesh relies on for its matching_peer_indexes[].
//
// The bound. Upstream's match array is `int matching_peer_indexes[MAX_CLIENTS]`
// (examples/simple_repeater/MyMesh.h:116), written only by MyMesh::searchPeersByHash
// (MyMesh.cpp:623-631, one entry per ACL client that matches, so n <= acl.getNumClients() <=
// MAX_CLIENTS) and read only by MyMesh::getPeerSharedSecret, onPeerDataRecv and onPeerPathRecv
// with the index they are given (MyMesh.cpp:634, :665, :770, each range-checked against
// acl.getNumClients()). Those three only ever see an index this file has shifted back into
// 0..n-1, so the extra peer is never an index into that array. The count we return, n + 1, is a
// loop bound in Mesh::onRecvPacket and nothing else: its loop (src/Mesh.cpp:152-186) keeps a
// `secret[PUB_KEY_SIZE]` and a `data[MAX_PACKET_PAYLOAD]` per iteration and no array sized by the
// count, and the "(max 4 matches supported ATM)" of its comment (:148) is a comment - nothing in
// the function bounds `num`. MAX_CLIENTS itself is 20 in this build: MyMesh.h includes
// helpers/ClientACL.h (its line 29) before its own `#ifndef MAX_CLIENTS / #define MAX_CLIENTS 32`
// (line 62), so ClientACL.h:37's 20 is the one in force, and nothing in platformio.ini defines
// it. tasks/phase-2/reports/01.md quotes the lines.

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

void TwinOakMesh::onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx,
                                 const uint8_t* secret, uint8_t* data, size_t len) {
  if (extra_ && twinoak::meshclient::peerSlotIsOurs(sender_idx, extra_)) {
    // A packet from the job's peer that verified with the job's secret: a reply, for task 02 to
    // read. Today it is counted and nothing else - and the clock keeper is *not* told, because
    // upstream never sees this packet and so answers nothing that carries our clock.
    (void)packet;
    (void)type;
    (void)secret;
    (void)data;
    (void)len;
    peer_data_taken_++;
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
    // A returned path from the job's peer, with a reply riding in it: task 02 learns the path
    // and takes the reply. Today it is counted and nothing else. **false**: no reciprocal path
    // is ever sent for the client's peer - one packet per send is the rail (S-12), and
    // Mesh::onRecvPacket (src/Mesh.cpp:173-178) sends one only when this returns true.
    (void)packet;
    (void)secret;
    (void)path;
    (void)path_len;
    (void)extra_type;
    (void)extra;
    (void)extra_len;
    peer_path_taken_++;
    return false;
  }
  twinoak::clock::noteTimestampEmitted();
  return MyMesh::onPeerPathRecv(packet,
                                twinoak::meshclient::peerSlotUpstreamIndex(sender_idx, extra_),
                                secret, path, path_len, extra_type, extra, extra_len);
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

void TwinOakMesh::onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret,
                                 const mesh::Identity& sender, uint8_t* data, size_t len) {
  twinoak::clock::noteTimestampEmitted();
  MyMesh::onAnonDataRecv(packet, secret, sender, data, len);
}

void TwinOakMesh::onTraceRecv(mesh::Packet* packet, uint32_t tag, uint32_t auth_code,
                              uint8_t flags, const uint8_t* path_snrs,
                              const uint8_t* path_hashes, uint8_t path_len) {
  MyMesh::onTraceRecv(packet, tag, auth_code, flags, path_snrs, path_hashes, path_len);
}

void TwinOakMesh::onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id,
                               uint32_t timestamp, const uint8_t* app_data,
                               size_t app_data_len) {
  MyMesh::onAdvertRecv(packet, id, timestamp, app_data, app_data_len);
}

void TwinOakMesh::logRx(mesh::Packet* pkt, int len, float score) {
  MyMesh::logRx(pkt, len, score);
}

void TwinOakMesh::logTx(mesh::Packet* pkt, int len) {
  MyMesh::logTx(pkt, len);
}

void TwinOakMesh::sendSelfAdvertisement(int delay_millis, bool flood) {
  twinoak::clock::noteTimestampEmitted();
  MyMesh::sendSelfAdvertisement(delay_millis, flood);
}
