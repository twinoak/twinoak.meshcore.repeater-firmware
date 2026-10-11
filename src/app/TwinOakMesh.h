// Our application: upstream's repeater, subclassed (spec/SYSTEM.md S-2, docs/ARCHITECTURE.md
// section 2).
//
// MyMesh is MeshCore's examples/simple_repeater/MyMesh.{h,cpp}, compiled from the pin by
// build_as_lib.py (-D BUILD_EXAMPLE=simple_repeater) with only its main.cpp left out
// (-D EXCLUDE_FROM_EXAMPLE=main.cpp). Not one line of it is edited: everything of ours hangs on the
// virtuals below. Local management and power reach upstream elsewhere (src/link/BoardLink.cpp,
// src/board/target.h); what is here is the clock keeper's four hooks and the mesh client's peer
// and sends.
//
// **The clock.** Four of the overrides tell the clock keeper that something carrying this node's
// clock has gone, or may have gone, on the air - which is what makes a backwards TIME_SET unsafe
// (spec/LINK-PROTOCOL.md section 5.4, src/clock/ClockKeeper.h). The four places are the four
// places upstream can put our clock into a packet:
//
//   - `sendSelfAdvertisement`, which createAdvert() stamps with getCurrentTime()
//     (src/Mesh.cpp:418). Ours is the main.cpp that sends the boot advert, so that one comes
//     through here too, and so does a console `advert` / `advert.zerohop`
//     (src/helpers/CommonCLI.cpp).
//   - `onAnonDataRecv`, `onPeerDataRecv` and `onPeerPathRecv`: a packet addressed to us that
//     verified, which upstream answers with a reply that carries our clock - the login reply and
//     the path return take it from getCurrentTimeUnique() (MyMesh.cpp:133 and :743), the anonymous
//     regions / owner / clock replies from getCurrentTime() (MyMesh.cpp:148-200), and
//     handleRequest's own telemetry from the sensors. Upstream decides whether to reply at all,
//     so these three are deliberately read as "may have": a packet from a stranger that upstream
//     rate-limits away still counts, which errs towards refusing a backwards set. A packet the
//     mesh client's own peer takes (below) is not one of them: upstream never sees it and
//     answers nothing.
//
// And a fifth place, ours: **every packet the mesh client sends** carries this node's unique
// second as its timestamp or tag (spec/LINK-PROTOCOL.md 6.2), so the MeshPort below tells the
// keeper before it hands the packet to upstream's sendDirect/sendFlood.
//
// Upstream's *periodic* adverts are upstream's own (MyMesh::loop, MyMesh.cpp:1294-1305) and are
// not hooked: the first of them cannot fire before two minutes (updateAdvertTimer at the default
// advert_interval of 1, which is 2 minutes, and 47 hours for the flood one), by which time the
// advert hold has lifted and the boot advert has already said so.
//
// **The mesh client's peer** (ARCHITECTURE section 2, "an extra peer in front"; phase 2's task
// 01). The job the Walter begins over the link (src/meshclient/Job.h) has one peer, and while a
// reply from it is awaited the four peer overrides put that peer **in front of** upstream's own
// matches: index 0 is ours, every other index is upstream's shifted down by one
// (src/meshclient/PeerSlot.h). With no job, or a job with no reply awaited, each of the four is
// upstream's result and nothing else - TwinOakMesh.cpp says so above each, and that sentence is
// the proof that the repeater is undisturbed.
//
// **The mesh client's sends and replies** (phase 2's task 02; src/meshclient/LinkMeshClient.h is
// the client, src/meshclient/MeshPort.h the seam). This class is the board's half of that seam:
// it builds one packet from the plaintext the client hands it, with upstream's own
// createAnonDatagram / createDatagram / createTrace, and queues it with sendDirect or sendFlood -
// two-byte path hashes on every flood (S-12). The replies come back through the peer overrides
// (a RESPONSE or a TXT_MSG of CLI data in onPeerDataRecv; a returned path, with the response
// riding in it, in onPeerPathRecv, which learns the path into the job and returns **false** -
// no reciprocal path, one packet per send) and through onTraceRecv for a completed trace.
//
// Every override below carries `override`, so a pin bump that makes one of these private, renames
// it or changes its signature is a compile error on the bump's own pull request and not a method
// that silently stops being called (docs/upstream.md). Above each, the upstream declaration it
// matches and the upstream definition it interposes on, at the pin d9296435 (repeater-v1.17.1).

#pragma once

#include <stdint.h>

#include "../meshclient/Job.h"
#include "../meshclient/LinkMeshClient.h"
#include "../meshclient/MeshPort.h"
#include "UpstreamMyMesh.h"

class TwinOakMesh : public MyMesh, public twinoak::meshclient::MeshPort {
 public:
  // The same six arguments as MyMesh's (examples/simple_repeater/MyMesh.h, under `public:`;
  // defined at MyMesh.cpp:861).
  using MyMesh::MyMesh;

  // The mesh client's state - the job, its path, what is awaited, the shared secret, the reply -
  // and the box link's view of it (src/link/BoardLink.cpp hands meshClient() to the session, which
  // is what puts MESH_CLIENT in HELLO's caps).
  twinoak::meshclient::Job& job() { return job_; }
  const twinoak::meshclient::Job& job() const { return job_; }
  twinoak::meshclient::LinkMeshClient& meshClient() { return client_; }

  // --- MeshPort: the board's half of the five sends (src/meshclient/MeshPort.h) ---------------
  //
  // Each of the three send functions builds exactly one packet with the primitive upstream's own
  // BaseChatMesh uses for the same thing, and queues it once. TwinOakMesh.cpp cites each against
  // the pin.
  uint32_t uniqueTime() override;
  void randomBytes(uint8_t* dest, size_t len) override;
  bool sendAnonRequest(const uint8_t* plain, size_t len,
                       const twinoak::meshclient::Route& route) override;
  bool sendDatagram(uint8_t payload_type, const uint8_t* plain, size_t len,
                    const twinoak::meshclient::Route& route) override;
  bool sendTrace(uint32_t tag, uint32_t auth_code, uint8_t flags, const uint8_t* path,
                 uint8_t path_bytes) override;
  void radioCounters(twinoak::meshclient::RadioCounters& out) override;

 protected:
  // --- the peer path: how a packet addressed to us finds the peer that sent it ------------------
  //
  // Mesh::onRecvPacket (src/Mesh.cpp:41; the peer loop is lines 149-186 at the pin) handles a
  // PATH / REQ / RESPONSE / TXT_MSG whose destination hash is ours by calling, in this order:
  //   num = searchPeersByHash(&src_hash);                   how many known peers match the hash
  //   for j in 0..num-1:  getPeerSharedSecret(secret, j);   then MACThenDecrypt with it
  //     the first j that decrypts:  onPeerPathRecv(pkt, j, ...)        for PAYLOAD_TYPE_PATH
  //                                 onPeerDataRecv(pkt, type, j, ...)  for the other three
  //     then break.
  // The order - index 0 is tried first, the first MAC that verifies wins - is what the mesh
  // client's "extra peer in front" rests on. It cannot be checked by the compiler, which is why
  // the function is cited here with the pin, and why docs/upstream.md lists it among the places
  // a bump re-reads.

  // Declared `int searchPeersByHash(const uint8_t* hash) override;` under `protected:` in
  // MyMesh.h; virtual in mesh::Mesh (src/Mesh.h:78). Upstream's definition (MyMesh.cpp:623) fills
  // its private matching_peer_indexes[] from the ACL and returns the count. Ours: upstream's
  // count, plus one in front when a reply is awaited and the job's peer matches the hash.
  int searchPeersByHash(const uint8_t* hash) override;

  // Declared `void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override;` under
  // `protected:` in MyMesh.h; virtual in mesh::Mesh (src/Mesh.h:85). Upstream (MyMesh.cpp:633)
  // copies the ACL entry's pre-computed shared secret for matching_peer_indexes[peer_idx]. Ours:
  // the job's ECDH secret for our index, upstream's for the rest with the index shifted.
  void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override;

  // Declared `void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const
  // uint8_t* secret, uint8_t* data, size_t len) override;` under `protected:` in MyMesh.h; virtual
  // in mesh::Mesh (src/Mesh.h:95). Upstream (MyMesh.cpp:663) runs a REQ from a known client
  // through handleRequest() and answers it, and runs a TXT_MSG from an admin through the CLI.
  // Ours: for our index, a RESPONSE or a TXT_MSG of CLI data is the job's reply
  // (LinkMeshClient::onResponse / onText); upstream's for the rest with the index shifted.
  void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret,
                      uint8_t* data, size_t len) override;

  // Declared `bool onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret,
  // uint8_t* path, uint8_t path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len)
  // override;` under `protected:` in MyMesh.h; virtual in mesh::Mesh (src/Mesh.h:116). Upstream
  // (MyMesh.cpp:766) stores the path on the client's ACL entry and returns false (no reciprocal
  // path). Ours: for our index, the path is learned into the job and the response riding in
  // `extra` is taken, and **false** is returned; upstream's for the rest with the index shifted.
  bool onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret, uint8_t* path,
                      uint8_t path_len, uint8_t extra_type, uint8_t* extra,
                      uint8_t extra_len) override;

  // Not overridden by MyMesh at the pin: the base is mesh::Mesh's empty `virtual void
  // onTraceRecv(Packet* packet, uint32_t tag, uint32_t auth_code, uint8_t flags, const uint8_t*
  // path_snrs, const uint8_t* path_hashes, uint8_t path_len) { }` (src/Mesh.h:107), called by
  // Mesh::onRecvPacket (src/Mesh.cpp:57) for a direct TRACE that has reached the end of its path:
  // `path_snrs` is the packet's path, one SNR byte appended per hop (src/Mesh.cpp:61),
  // `path_hashes` the round-trip hashes as sent, `path_len` their byte count. Ours: a completed
  // trace with the awaited tag is the job's reply (LinkMeshClient::onTrace).
  void onTraceRecv(mesh::Packet* packet, uint32_t tag, uint32_t auth_code, uint8_t flags,
                   const uint8_t* path_snrs, const uint8_t* path_hashes,
                   uint8_t path_len) override;

  // Declared `void onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret, const
  // mesh::Identity& sender, uint8_t* data, size_t len) override;` under `protected:` in MyMesh.h;
  // virtual in mesh::Mesh (src/Mesh.h:130). Upstream (MyMesh.cpp:571) runs an anonymous request -
  // a login, or the regions / owner / clock requests a stranger may ask - and answers it with a
  // reply that carries our clock.
  void onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret, const mesh::Identity& sender,
                      uint8_t* data, size_t len) override;

  // --- what the radio hears and sends ---------------------------------------------------------

  // Declared `void onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t
  // timestamp, const uint8_t* app_data, size_t app_data_len);` under `protected:` in MyMesh.h -
  // without the override keyword, but virtual all the same through mesh::Mesh (src/Mesh.h:122).
  // Upstream (MyMesh.cpp:650) chains to Mesh and puts a zero-hop repeater advert in its
  // neighbour table. Called by Mesh::onRecvPacket (src/Mesh.cpp:284) once the signature verified.
  void onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp,
                    const uint8_t* app_data, size_t app_data_len) override;

  // Declared `void logRx(mesh::Packet* pkt, int len, float score) override;` under `protected:`
  // in MyMesh.h; virtual in mesh::Dispatcher (src/Dispatcher.h:161). Upstream (MyMesh.cpp:479)
  // appends a line to the packet log file when logging is on. Called while the packet is still
  // the one the radio just received, so its RSSI is this packet's (ARCHITECTURE section 7).
  void logRx(mesh::Packet* pkt, int len, float score) override;

  // Declared `void logTx(mesh::Packet* pkt, int len) override;` under `protected:` in MyMesh.h;
  // virtual in mesh::Dispatcher (src/Dispatcher.h:162). Upstream (MyMesh.cpp:505) logs likewise.
  void logTx(mesh::Packet* pkt, int len) override;

 public:
  // Declared `void sendSelfAdvertisement(int delay_millis, bool flood) override;` under
  // `public:` in MyMesh.h; pure virtual in CommonCLICallbacks (src/helpers/CommonCLI.h:204).
  // Upstream
  // (MyMesh.cpp:1031) builds the advert with createSelfAdvert() - which stamps it with the
  // clock - and sends it zero-hop or flooded.
  void sendSelfAdvertisement(int delay_millis, bool flood) override;

 private:
  // The job's ECDH secret, calculated once per job on first need from this node's private key
  // (mesh::LocalIdentity::calcSharedSecret, src/Identity.h:81) into the job's own buffer, which
  // is where it is wiped when the job ends.
  const uint8_t* sharedSecret();
  // One packet of the client's, queued along its route: direct along the job's path, or flooded
  // with two-byte hashes. Tells the clock keeper first. False when `pkt` is null (the pool was
  // empty and nothing was built).
  bool dispatch(mesh::Packet* pkt, const twinoak::meshclient::Route& route);
  // The reply's SNR as the record carries it: quarter-decibels, from the packet.
  static int8_t snrQ4Of(const mesh::Packet* pkt);

  twinoak::meshclient::Job job_;
  twinoak::meshclient::LinkMeshClient client_{job_, *this};
  // Whether the extra peer is in front for the packet Mesh::onRecvPacket is handling now: set by
  // searchPeersByHash() - the first of the four calls, every time - and read by the three that
  // follow it, exactly as upstream's own matching_peer_indexes[] is filled by its
  // searchPeersByHash() and read by its three. False at every moment no reply is awaited.
  bool extra_ = false;
};
