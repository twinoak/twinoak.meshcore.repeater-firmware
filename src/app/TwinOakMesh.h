// Our application: upstream's repeater, subclassed (spec/SYSTEM.md S-2, docs/ARCHITECTURE.md
// section 2).
//
// MyMesh is MeshCore's examples/simple_repeater/MyMesh.{h,cpp}, compiled from the pin by
// build_as_lib.py (-D BUILD_EXAMPLE=simple_repeater) with only its main.cpp left out
// (-D EXCLUDE_FROM_EXAMPLE=main.cpp). Not one line of it is edited: everything of ours hangs on the
// virtuals below. Local management, power and the mesh client are added here later.
//
// What they do today is tell the clock keeper that something carrying this node's clock has gone,
// or may have gone, on the air - which is what makes a backwards TIME_SET unsafe
// (spec/LINK-PROTOCOL.md section 5.4, src/clock/ClockKeeper.h). The four places are the four
// places upstream can put our clock into a packet, and each calls its base and nothing else:
//
//   - `sendSelfAdvertisement`, which createAdvert() stamps with getCurrentTime()
//     (src/Mesh.cpp:418). Ours is the main.cpp that sends the boot advert, so that one comes
//     through here too, and so does a console `advert` / `advert.zerohop`
//     (src/helpers/CommonCLI.cpp).
//   - `onAnonDataRecv`, `onPeerDataRecv` and `onPeerPathRecv`: a packet addressed to us that
//     verified, which upstream answers with a reply that carries our clock - the login reply and
//     the path return take it from getCurrentTimeUnique() (MyMesh.cpp:135 and :743), the anonymous
//     regions / owner / clock replies from getCurrentTime() (MyMesh.cpp:148-200), and
//     handleRequest's own telemetry from the sensors. Upstream decides whether to reply at all,
//     so these three are deliberately read as "may have": a packet from a stranger that upstream
//     rate-limits away still counts, which errs towards refusing a backwards set.
//
// Upstream's *periodic* adverts are upstream's own (MyMesh::loop, MyMesh.cpp:1294-1305) and are
// not hooked: the first of them cannot fire before two minutes (updateAdvertTimer at the default
// advert_interval of 1, which is 2 minutes, and 47 hours for the flood one), by which time the
// advert hold has lifted and the boot advert has already said so.
//
// Every override below carries `override`, so a pin bump that makes one of these private, renames
// it or changes its signature is a compile error on the bump's own pull request and not a method
// that silently stops being called (docs/upstream.md). Above each, the upstream declaration it
// matches and the upstream definition it interposes on, at the pin d9296435 (repeater-v1.17.1).

#pragma once

#include "UpstreamMyMesh.h"

class TwinOakMesh : public MyMesh {
 public:
  // The same six arguments as MyMesh's (examples/simple_repeater/MyMesh.h, under `public:`;
  // defined at MyMesh.cpp:861).
  using MyMesh::MyMesh;

 protected:
  // --- the peer path: how a packet addressed to us finds the peer that sent it ------------------
  //
  // Mesh::onRecvPacket (src/Mesh.cpp:41; the peer loop is lines 147-186 at the pin) handles a
  // PATH / REQ / RESPONSE / TXT_MSG whose destination hash is ours by calling, in this order:
  //   num = searchPeersByHash(&src_hash);                   how many known peers match the hash
  //   for j in 0..num-1:  getPeerSharedSecret(secret, j);   then MACThenDecrypt with it
  //     the first j that decrypts:  onPeerPathRecv(pkt, j, ...)        for PAYLOAD_TYPE_PATH
  //                                 onPeerDataRecv(pkt, type, j, ...)  for the other three
  //     then break.
  // The order - index 0 is tried first, the first MAC that verifies wins - is what the mesh
  // client's "extra peer in front" (ARCHITECTURE section 2, phase 2) rests on. It cannot be
  // checked by the compiler, which is why the function is cited here with the pin.

  // Declared `int searchPeersByHash(const uint8_t* hash) override;` under `protected:` in
  // MyMesh.h; virtual in mesh::Mesh (src/Mesh.h:78). Upstream's definition (MyMesh.cpp:623) fills
  // its private matching_peer_indexes[] from the ACL and returns the count.
  int searchPeersByHash(const uint8_t* hash) override;

  // Declared `void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override;` under
  // `protected:` in MyMesh.h; virtual in mesh::Mesh (src/Mesh.h:85). Upstream (MyMesh.cpp:633)
  // copies the ACL entry's pre-computed shared secret for matching_peer_indexes[peer_idx].
  void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override;

  // Declared `void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const
  // uint8_t* secret, uint8_t* data, size_t len) override;` under `protected:` in MyMesh.h; virtual
  // in mesh::Mesh (src/Mesh.h:95). Upstream (MyMesh.cpp:663) runs a REQ from a known client
  // through handleRequest() and answers it.
  void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret,
                      uint8_t* data, size_t len) override;

  // Declared `bool onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret,
  // uint8_t* path, uint8_t path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len)
  // override;` under `protected:` in MyMesh.h; virtual in mesh::Mesh (src/Mesh.h:116). Upstream
  // (MyMesh.cpp:766) stores the path on the client's ACL entry and returns false (no reciprocal
  // path).
  bool onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret, uint8_t* path,
                      uint8_t path_len, uint8_t extra_type, uint8_t* extra,
                      uint8_t extra_len) override;

  // Not overridden by MyMesh at the pin: the base is mesh::Mesh's empty `virtual void
  // onTraceRecv(Packet* packet, uint32_t tag, uint32_t auth_code, uint8_t flags, const uint8_t*
  // path_snrs, const uint8_t* path_hashes, uint8_t path_len) { }` (src/Mesh.h:107), called by
  // Mesh::onRecvPacket (src/Mesh.cpp:57) for a direct TRACE that has reached the end of its path.
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
};
