// The job as the box link's MeshClientAccess (spec/LINK-PROTOCOL.md sections 5.3 and 6.1).
//
// Thirty lines between two portable layers, the way src/clock/LinkClock.h is for the clock
// keeper, and the one place that holds the job's numbers against the wire's with static_asserts:
// the path modes, the bounds of 6.1 and the 120-second silence rule. src/app/TwinOakMesh.h owns
// one of these beside its Job, and src/link/BoardLink.cpp hands it to the session - which is what
// puts MESH_CLIENT in HELLO's caps, because a capability is an interface that is there
// (src/link/session/Interfaces.h).
//
// Host-portable: no Arduino, no MeshCore. test/test_meshclient drives a real Session through it.

#pragma once

#include <stdint.h>

#include "../link/codec/Messages.h"
#include "../link/codec/Numbers.h"
#include "../link/session/Interfaces.h"
#include "Job.h"

namespace twinoak {
namespace meshclient {

static_assert(uint8_t(kPathNone) == link::kPathNone, "PathMode must be LINK-PROTOCOL 6.1's path_mode");
static_assert(uint8_t(kPathDirect) == link::kPathDirect, "PathMode must be 6.1's path_mode");
static_assert(uint8_t(kPathHops) == link::kPathHops, "PathMode must be 6.1's path_mode");
static_assert(kPeerKeyLen == link::kPubKeyLen, "a peer's key is the link's pubkey");
static_assert(kHashSizeMin == link::kPathHashSizeMin, "6.1's bounds, in one place");
static_assert(kHashSizeMax == link::kPathHashSizeMax, "6.1's bounds, in one place");
static_assert(kHopCountMin == link::kPathHopCountMin, "6.1's bounds, in one place");
static_assert(kHopCountMax == link::kPathHopCountMax, "6.1's bounds, in one place");
static_assert(kPathBytesMax == link::kPathHashesMax, "6.1's bounds, in one place");
static_assert(kIdleEndMs == link::kJobIdleEndMs, "the 120-second rule, in one place");

class LinkMeshClient : public link::MeshClientAccess {
 public:
  explicit LinkMeshClient(Job& job) : job_(job) {}

  bool jobBegun() const override { return job_.begun(); }
  bool replyAwaited() const override { return job_.awaiting(); }

  uint8_t beginJob(const link::JobBegin& m, uint32_t now_ms) override;
  void endJob() override { job_.end(); }
  uint8_t setPath(const link::Path& p) override;
  uint8_t getPath(link::Path& out) override;
  void noteLinkRequest(uint32_t now_ms) override { job_.noteLinkRequest(now_ms); }

  // Once a pass, from the board's link loop: the silence rule (Job::loop).
  void loop(uint32_t now_ms) { job_.loop(now_ms); }

  Job& job() { return job_; }

 private:
  Job& job_;
};

}  // namespace meshclient
}  // namespace twinoak
