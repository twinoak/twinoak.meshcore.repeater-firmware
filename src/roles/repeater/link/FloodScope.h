// The flood scope (link 1.3, spec/LINK-PROTOCOL.md 6.5) as this board has it: the mesh client's
// floods leave **exactly as the node's own flooded advert does** - through upstream's
// `MyMesh::sendFloodScoped` with upstream's own `default_scope` - and `FLOOD_SCOPE` answers the
// default region's name out of upstream's own `region_map`.
//
// Thomas closed `spec/SYSTEM.md` 10's open point 4 on 14 November 2026: a job sent through a
// Walter carries the unit's default scope too, because the Danish network's repeaters hold `dk`
// and a flood with no transport code gets only what a wildcard allows. Before this file the
// client's floods were plain `sendFlood` and nothing else.
//
// This is the only file of the firmware that names `MyMesh::default_scope` or
// `MyMesh::region_map`, both **private** at the pin 81dbae59 (`examples/simple_repeater/MyMesh.h`:
// the class body opens with no access specifier, so everything above its `protected:` is
// private). `spec/SYSTEM.md` S-2's *second exception*, of 14 November 2026, is what allows
// reaching them; how it is done without editing one file of upstream's is
// `src/common/cxx/PrivateMember.h`, the same idiom `src/roles/repeater/link/LocalRequest.cpp`
// uses for link 1.2's two, and `docs/upstream.md`'s place 9 lists all four as names a pin bump
// re-reads. **Both are read and neither is ever written**: no key is derived here, no transport
// code is computed here (6.5: "the RAK derives no key and computes no code of its own"), and
// upstream's own behaviour is untouched.
//
// `test/test_rails`' rule 4 holds the two halves a grep can hold: that those two names are in
// this file and nowhere else, and that the client has one `sendFlood` call and one
// `sendFloodScoped` call, both at `kFloodPathHashSize`.

#pragma once

#include "common/link/session/Interfaces.h"
#include "roles/repeater/app/UpstreamMyMesh.h"

namespace twinoak {
namespace link {

// The session's FloodScopeAccess over upstream's region map. One of these exists
// (`src/roles/repeater/link/BoardLink.cpp`), and its presence is what puts `0x100` in HELLO's
// `caps` (session/Interfaces.h, capsOf).
class BoardFloodScope : public FloodScopeAccess {
 public:
  // The node. Before the first request; nothing is called before then.
  void attach(MyMesh* mesh) { mesh_ = mesh; }

  // The default region's name, without a leading `#`, or empty when the node holds none. A view
  // into upstream's own `RegionEntry`, which lives as long as the node does; read afresh at every
  // request, because a `region default` taken over the mesh or on this link is in force for the
  // next send (6.5).
  Bytes scopeName() const override;

 private:
  MyMesh* mesh_ = nullptr;
};

// One flood of the mesh client's, sent the way the node's own flooded advert is sent: upstream's
// `sendFloodScoped(default_scope, pkt, 0, kFloodPathHashSize)`, where `MyMesh::sendSelfAdvertisement`
// passes `_prefs.path_hash_mode + 1` and S-12 makes ours two. The scope is read out of upstream
// at the moment of the send and no copy of it is kept.
//
// Returns **what went on the packet**: true when the default scope's key was not null and a
// transport code was therefore computed, false when it was null - in which case upstream's own
// `sendFloodScoped` is a plain `sendFlood` and the packet is byte for byte the one this firmware
// sent before link 1.3 (`MyMesh.cpp`:999-1008 at the pin). That answer is the link's `scoped`
// byte (6.2).
bool floodInDefaultScope(MyMesh& mesh, mesh::Packet* pkt);

}  // namespace link
}  // namespace twinoak
