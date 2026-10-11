#include "roles/repeater/meshclient/Job.h"

#include <string.h>

namespace twinoak {
namespace meshclient {

bool pathIsLegal(uint8_t hash_size, uint8_t hop_count, size_t byte_count) {
  if (hash_size < kHashSizeMin || hash_size > kHashSizeMax) return false;
  if (hop_count < kHopCountMin || hop_count > kHopCountMax) return false;
  const size_t bytes = size_t(hash_size) * size_t(hop_count);
  if (bytes > kPathBytesMax) return false;
  return byte_count == bytes;
}

Job::Job()
    : begun_(false),
      mode_(kPathNone),
      path_len_(0),
      await_kind_(kAwaitNone),
      await_tag_(0),
      await_deadline_(0),
      have_secret_(false),
      last_request_ms_(0),
      ended_for_silence_(0) {
  memset(peer_, 0, sizeof peer_);
  memset(path_, 0, sizeof path_);
  memset(secret_, 0, sizeof secret_);
}

// ----------------------------------------------------------------------------- the job

bool Job::begin(const uint8_t* peer_key, uint8_t mode, uint8_t hash_size, uint8_t hop_count,
                const uint8_t* hashes, size_t hashes_len, uint32_t now_ms) {
  if (peer_key == nullptr) return false;
  // Judged before anything is touched: a request that is refused changes nothing.
  if (mode > kPathHops) return false;
  if (mode == kPathHops && (hashes == nullptr || !pathIsLegal(hash_size, hop_count, hashes_len))) {
    return false;
  }
  // "It ends any job in progress first" (6.1): the secret of the old peer is wiped here, before
  // the new key is copied in, so that no moment has the new peer and the old secret together.
  end();
  memcpy(peer_, peer_key, kPeerKeyLen);
  begun_ = true;
  takePath(mode, hash_size, hop_count, hashes, hashes_len);
  last_request_ms_ = now_ms;  // the JOB_BEGIN itself is a request on the link
  return true;
}

void Job::end() {
  begun_ = false;
  memset(peer_, 0, sizeof peer_);
  forgetPath();
  clearAwait();
  wipeSecret();
}

// ----------------------------------------------------------------------------- the path

bool Job::setPath(uint8_t mode, uint8_t hash_size, uint8_t hop_count, const uint8_t* hashes,
                  size_t hashes_len) {
  if (!begun_) return false;
  if (mode > kPathHops) return false;
  if (mode == kPathHops && (hashes == nullptr || !pathIsLegal(hash_size, hop_count, hashes_len))) {
    return false;
  }
  takePath(mode, hash_size, hop_count, hashes, hashes_len);
  return true;
}

uint8_t Job::pathByteCount() const {
  if (mode_ != kPathHops) return 0;
  return uint8_t(meshclient::pathHashSize(path_len_) * meshclient::pathHopCount(path_len_));
}

// Only ever called with a path begin() or setPath() has already judged legal.
void Job::takePath(uint8_t mode, uint8_t hash_size, uint8_t hop_count, const uint8_t* hashes,
                   size_t hashes_len) {
  forgetPath();
  mode_ = mode;
  if (mode != kPathHops) return;  // none and direct: path_len 0 and no bytes
  path_len_ = encodePathLen(hash_size, hop_count);
  memcpy(path_, hashes, hashes_len);
}

void Job::forgetPath() {
  mode_ = kPathNone;
  path_len_ = 0;
  memset(path_, 0, sizeof path_);
}

// ----------------------------------------------------------------------------- a reply awaited

void Job::arm(uint8_t kind, uint32_t tag, uint32_t deadline_ms) {
  await_kind_ = kind;
  await_tag_ = tag;
  await_deadline_ = deadline_ms;
}

void Job::clearAwait() {
  await_kind_ = kAwaitNone;
  await_tag_ = 0;
  await_deadline_ = 0;
}

// ----------------------------------------------------------------------------- the secret

void Job::wipeSecret() {
  // The ECDH secret is the one thing here that is worth wiping rather than dropping, and a plain
  // store to a buffer that is never read again is a store the compiler may remove. Through a
  // volatile pointer every write is kept (the agent firmware's AgentMesh::endJob does exactly
  // this).
  volatile uint8_t* p = secret_;
  for (size_t i = 0; i < sizeof secret_; i++) p[i] = 0;
  have_secret_ = false;
}

// ----------------------------------------------------------------------------- silence

void Job::noteLinkRequest(uint32_t now_ms) { last_request_ms_ = now_ms; }

bool Job::loop(uint32_t now_ms) {
  if (!begun_) return false;
  if (idleMs(now_ms) < kIdleEndMs) return false;
  end();
  ended_for_silence_++;
  return true;
}

}  // namespace meshclient
}  // namespace twinoak
