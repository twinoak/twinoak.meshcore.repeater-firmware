// What the mesh client needs of the board, as one small interface: the clock's unique second,
// random bytes, the three ways one packet is built and queued, and the radio's own counters.
//
// The client (src/roles/repeater/meshclient/LinkMeshClient.{h,cpp}) decides *what* goes on the air - which of
// the five sends, the plaintext's layout, the route, the refusals, the wait - and is portable and
// tested against a fake of this. The board (src/roles/repeater/app/TwinOakMesh.cpp) decides *how*: MeshCore's
// own createAnonDatagram / createDatagram / createTrace and sendDirect / sendFlood, cited there
// against the pin. That is the seam docs/ARCHITECTURE.md section 7 draws between "sends (board)"
// and "state (portable)", and it is what lets the five sends be tested byte for byte on a host.
//
// Every number here that is MeshCore's is held against upstream's macro by a static_assert in
// src/roles/repeater/app/TwinOakMesh.cpp, so that a pin bump which moves one cannot leave this file behind.
//
// Host-portable: no Arduino, no MeshCore.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace twinoak {
namespace meshclient {

// MeshCore's payload types the client originates (src/Packet.h at the pin 81dbae59).
constexpr uint8_t kPayloadTypeReq = 0x00;      // a request: timestamp, type, data
constexpr uint8_t kPayloadTypeTxtMsg = 0x02;   // a text message: timestamp, flags, text
constexpr uint8_t kPayloadTypeAnonReq = 0x07;  // an anonymous request: a login

// MeshCore's packet, as the plaintext a datagram may carry sees it: Mesh::createDatagram refuses
// a plaintext longer than MAX_PACKET_PAYLOAD - CIPHER_MAC_SIZE - (CIPHER_BLOCK_SIZE - 1), which is
// 184 - 2 - 15 (src/Mesh.cpp:490).
constexpr size_t kMaxPacketPayload = 184;
constexpr size_t kMaxDatagramPlaintext = 167;
// A trace's payload is its 9-byte header (tag, auth code, flags; Mesh::createTrace) and then the
// round-trip path, which sendDirect appends to the payload without a bound of its own
// (src/Mesh.cpp:698-701) - so the bound is this file's.
constexpr size_t kTraceHeaderBytes = 9;
constexpr size_t kTraceHashesMaxBytes = kMaxPacketPayload - kTraceHeaderBytes;  // 175

// Every flood this client originates uses two-byte path hashes, never one (spec/SYSTEM.md S-12;
// meshmaster.dk D-33; the agent firmware's AgentMesh::kFloodPathHashSize).
constexpr uint8_t kFloodPathHashSize = 2;

// How a packet leaves: direct along a path in MeshCore's own path_len encoding (0 is zero hops,
// sent direct with an empty path), or as a flood with two-byte hashes.
//
// `scoped` is link 1.3's one addition (spec/LINK-PROTOCOL.md 6.5): what the port has to know of a
// flood beyond its hash size. True asks for the flood to go out exactly as the node's own flooded
// advert does - through upstream's sendFloodScoped with upstream's own default scope - and it is
// true only when the link said so (Session.h's floodScopedNow: the capability, a session of 1.3
// or later, and FLOOD_SCOPE's mode) and the route is a flood. A direct send and a trace are never
// scoped.
struct Route {
  bool direct = false;
  const uint8_t* path = nullptr;
  uint8_t path_len = 0;  // (hash_size - 1) << 6 | hop_count; the Job's pathLen()
  bool scoped = false;   // 1.3: this flood takes the node's default scope
};

// What one send came to. `queued` is what the three sends used to return: a packet was built and
// put in the queue, false when the pool was empty and nothing was built. `scoped` is what
// actually went on the packet and not what the Route asked for - "a radio with no default scope
// answers 0 to a flood that was asked scoped" (6.2's `scoped` row, 6.5) - and it is what the
// link's answer carries. A port that cannot scope anything answers false to both questions at
// once by leaving it alone.
struct PacketSent {
  bool queued = false;
  bool scoped = false;
};

// What MESH_COUNTERS reads from the radio (6.4).
struct RadioCounters {
  uint32_t rx = 0;           // every packet the radio received
  uint32_t tx = 0;           // every packet it transmitted, forwards included
  int16_t noise_floor = 0;   // dBm
  int16_t last_rssi = 0;     // dBm
  int8_t last_snr_q4 = 0;    // quarter-decibels
};

struct MeshPort {
  virtual ~MeshPort() {}

  // MeshCore's unique-time source: never the same second twice, which is what the peer judges
  // replays by (6.2; RTCClock::getCurrentTimeUnique, src/MeshCore.h:119).
  virtual uint32_t uniqueTime() = 0;
  // Random bytes, for a discovery's blob and a trace's auth code.
  virtual void randomBytes(uint8_t* dest, size_t len) = 0;

  // Each builds **one** packet to the job's peer from the plaintext given, queues it along the
  // route, and answers with a PacketSent: `queued` false when no packet could be built (the pool
  // is empty), and `scoped` whether a transport code of the default scope went on it. The
  // plaintext is the caller's buffer and the caller wipes it; what the packet holds is ciphertext.
  virtual PacketSent sendAnonRequest(const uint8_t* plain, size_t len, const Route& route) = 0;
  virtual PacketSent sendDatagram(uint8_t payload_type, const uint8_t* plain, size_t len,
                                  const Route& route) = 0;
  // One TRACE packet, always direct, along `path_bytes` bytes of round-trip hashes whose size the
  // flags byte states as an exponent (Mesh::onRecvPacket, the TRACE branch). Never scoped: a
  // trace is not a flood.
  virtual PacketSent sendTrace(uint32_t tag, uint32_t auth_code, uint8_t flags,
                               const uint8_t* path, uint8_t path_bytes) = 0;

  virtual void radioCounters(RadioCounters& out) = 0;
};

}  // namespace meshclient
}  // namespace twinoak
