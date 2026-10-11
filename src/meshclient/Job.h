// The mesh client's state: one job with one peer (spec/SYSTEM.md S-1, S-4;
// spec/LINK-PROTOCOL.md section 6 and 6.1; docs/ARCHITECTURE.md section 7).
//
// A port of the job half of src/mesh/AgentMesh.{h,cpp} of twinoak/meshmaster-agent-firmware at
// 51faafb427cba41b0de9983736b6621bc155f829 - beginJob, endJob, setDirectPath, forgetPath,
// describePath, sharedSecret - with the state pulled out of the mesh class so that it is
// host-portable: no Arduino, no MeshCore, no heap, no clock of its own. The ECDH secret is
// *calculated* by the board (src/app/TwinOakMesh.cpp, which has the node's private key) but it
// *lives* here, so that the one rule about it - wiped, not dropped, when the job ends - can be
// read back by a test (test/test_meshclient) the way the agent firmware's credential test reads
// its buffer back.
//
// What a job holds:
//
//   - **the peer's key**, 32 bytes, copied from the request;
//   - **the path** - none (the next send floods), direct (zero hops, sent direct with an empty
//     path), or hops with a hash size - held in MeshCore's own encoding, the `path_len` byte
//     `(hash_size - 1) << 6 | hop_count` (src/Packet.h at the pin: getPathHashSize,
//     getPathHashCount, setPathHashSizeAndCount), and validated exactly as LINK-PROTOCOL 6.1
//     bounds it: a hash of 1 to 3 bytes, 1 to 63 hops, at most 64 bytes of hops, and a hop count
//     that agrees with the bytes given. Upstream's own Packet::isValidPathLen (src/Packet.cpp:13)
//     checks only hash_count * hash_size <= MAX_PATH_SIZE; the link's bounds are inside that;
//   - **a reply awaited**: its kind (a response, a text, a trace), its tag (0 = the next from the
//     peer) and its deadline. Task 02 arms it when it sends; this file defines it and its
//     clearing, because "a reply is awaited" is the one condition under which the extra peer is
//     in front of upstream's (src/app/TwinOakMesh.cpp);
//   - **the shared secret**, once the board has calculated it (once per job, on first need);
//   - **the silence rule**: the RAK ends a job by itself when 120 seconds pass with no request on
//     the link while a job is begun - a Walter that crashed must not leave a peer open (6.1).
//     Every valid request on the link restarts the count (noteLinkRequest), and loop() is where
//     the job ends.
//
// Every method that needs the time takes the millisecond clock from its caller, as the attention
// machine does, so that test/test_meshclient winds a fake one.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace twinoak {
namespace meshclient {

// ----------------------------------------------------------------------------- the numbers

constexpr uint8_t kPeerKeyLen = 32;    // MeshCore's PUB_KEY_SIZE (src/MeshCore.h:8 at the pin)
constexpr uint8_t kSecretLen = 32;     // an ECDH shared secret is PUB_KEY_SIZE bytes too
constexpr uint8_t kPathBytesMax = 64;  // MeshCore's MAX_PATH_SIZE (src/MeshCore.h:22)
// src/app/TwinOakMesh.cpp static_asserts the three above against upstream's own macros.

// LINK-PROTOCOL.md 6.1's bounds on a path. src/meshclient/LinkMeshClient.h static_asserts them
// against the link's own constants.
constexpr uint8_t kHashSizeMin = 1;
constexpr uint8_t kHashSizeMax = 3;
constexpr uint8_t kHopCountMin = 1;
constexpr uint8_t kHopCountMax = 63;

// "The RAK also ends a job by itself when 120 seconds pass with no request on the link while a
// job is begun" (6.1). The link's kJobIdleEndMs, and the same static_assert holds them together.
constexpr uint32_t kIdleEndMs = 120000;

// A path's mode, with the wire's own values (6.1).
enum PathMode : uint8_t {
  kPathNone = 0,    // the next send floods
  kPathDirect = 1,  // zero hops, sent direct with an empty path
  kPathHops = 2,    // hash_size x hop_count bytes of hops
};

// What a reply awaited is, if any. The words are LINK-PROTOCOL 6.2's: a login, a request and a
// discovery await a response; a CLI line awaits a text; a trace awaits a trace.
enum AwaitKind : uint8_t {
  kAwaitNone = 0,
  kAwaitResponse = 1,
  kAwaitText = 2,
  kAwaitTrace = 3,
};

// ----------------------------------------------------------------------------- the encoding

// MeshCore's path_len byte: bits 7-6 the hash size minus one, bits 5-0 the hop count
// (src/Packet.h:79-83 at the pin).
inline uint8_t encodePathLen(uint8_t hash_size, uint8_t hop_count) {
  return uint8_t(((hash_size - 1) << 6) | (hop_count & 63));
}
inline uint8_t pathHashSize(uint8_t path_len) { return uint8_t((path_len >> 6) + 1); }
inline uint8_t pathHopCount(uint8_t path_len) { return uint8_t(path_len & 63); }

// Whether a path of hops is one LINK-PROTOCOL 6.1 allows: a hash of 1 to 3 bytes, 1 to 63 hops,
// at most 64 bytes, and exactly hash_size x hop_count bytes given.
bool pathIsLegal(uint8_t hash_size, uint8_t hop_count, size_t byte_count);

// ----------------------------------------------------------------------------- the job

class Job {
 public:
  Job();

  // --- the job ----------------------------------------------------------------------------

  // JOB_BEGIN (6.1): ends any job in progress first, then makes this key the peer and this path
  // the job's. `peer_key` is kPeerKeyLen bytes and is copied. A path that is not legal is
  // refused and **nothing changes** - the job in progress, if any, is not ended either.
  bool begin(const uint8_t* peer_key, uint8_t mode, uint8_t hash_size, uint8_t hop_count,
             const uint8_t* hashes, size_t hashes_len, uint32_t now_ms);
  // JOB_END (6.1): forgets the peer, the path and any awaited reply, and wipes the shared secret
  // - through a volatile pointer, so that the compiler cannot elide the writes (the agent
  // firmware's endJob does the same).
  void end();

  bool begun() const { return begun_; }
  const uint8_t* peerKey() const { return peer_; }

  // --- the path ---------------------------------------------------------------------------

  // PATH_SET (6.1), and what a returned path becomes in task 02. Replaces the job's path; mode
  // kPathNone forgets it. A path that is not legal is refused and the path is unchanged. Needs a
  // job; without one it is refused too (the caller answers ERR_STATE).
  bool setPath(uint8_t mode, uint8_t hash_size, uint8_t hop_count, const uint8_t* hashes,
               size_t hashes_len);

  uint8_t pathMode() const { return mode_; }
  // The encoded path_len byte MeshCore's sendDirect() takes: 0 for none and for direct (an
  // empty path), (hash_size - 1) << 6 | hop_count for hops.
  uint8_t pathLen() const { return path_len_; }
  const uint8_t* pathBytes() const { return path_; }
  uint8_t pathByteCount() const;
  uint8_t pathHashSize() const { return mode_ == kPathHops ? meshclient::pathHashSize(path_len_) : 0; }
  uint8_t pathHopCount() const { return mode_ == kPathHops ? meshclient::pathHopCount(path_len_) : 0; }

  // --- a reply awaited --------------------------------------------------------------------

  // Task 02 arms this when it has queued a packet: "the RAK is awaiting from the moment the
  // packet is queued until a matching reply arrives or the time is up" (6.2). While it is armed
  // and the job's peer matches a packet's source hash, the extra peer is in front of upstream's
  // (src/app/TwinOakMesh.cpp). It does not need a job: a trace does not either.
  void arm(uint8_t kind, uint32_t tag, uint32_t deadline_ms);
  void clearAwait();

  bool awaiting() const { return await_kind_ != kAwaitNone; }
  uint8_t awaitKind() const { return await_kind_; }
  uint32_t awaitTag() const { return await_tag_; }
  uint32_t awaitDeadline() const { return await_deadline_; }
  bool awaitExpired(uint32_t now_ms) const {
    return awaiting() && int32_t(now_ms - await_deadline_) >= 0;
  }

  // --- the shared secret ------------------------------------------------------------------

  // The board calculates the ECDH secret into this buffer once per job, on first need, and says
  // so with markSecret(); end() wipes it. kSecretLen bytes.
  uint8_t* secretBuffer() { return secret_; }
  const uint8_t* secretBuffer() const { return secret_; }
  bool hasSecret() const { return have_secret_; }
  void markSecret() { have_secret_ = true; }

  // --- the silence rule -------------------------------------------------------------------

  // A valid request arrived on the link (any type, a retry included): the count starts again.
  void noteLinkRequest(uint32_t now_ms);
  // Once a pass: ends the job when kIdleEndMs have passed since the last request while a job is
  // begun. Returns true when it did.
  bool loop(uint32_t now_ms);
  // Milliseconds since the last request on the link, as the rule counts them.
  uint32_t idleMs(uint32_t now_ms) const { return now_ms - last_request_ms_; }
  // How many jobs the silence rule has ended since boot.
  uint32_t endedForSilence() const { return ended_for_silence_; }

 private:
  void takePath(uint8_t mode, uint8_t hash_size, uint8_t hop_count, const uint8_t* hashes,
                size_t hashes_len);
  void forgetPath();
  void wipeSecret();

  uint8_t peer_[kPeerKeyLen];
  bool begun_;

  uint8_t mode_;
  uint8_t path_len_;
  uint8_t path_[kPathBytesMax];

  uint8_t await_kind_;
  uint32_t await_tag_;
  uint32_t await_deadline_;

  uint8_t secret_[kSecretLen];
  bool have_secret_;

  uint32_t last_request_ms_;
  uint32_t ended_for_silence_;
};

}  // namespace meshclient
}  // namespace twinoak
