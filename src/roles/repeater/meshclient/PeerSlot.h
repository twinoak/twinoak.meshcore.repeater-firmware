// The index arithmetic of "an extra peer in front" (docs/ARCHITECTURE.md section 2), as pure
// functions - four lines, separate, so that the part which must never be wrong can be tested
// exhaustively (test/test_meshclient) and read in one glance.
//
// Mesh::onRecvPacket (src/Mesh.cpp:149-186 at the pin 81dbae59) asks searchPeersByHash() how many
// known peers match a packet's source hash, then tries index 0, 1, ... in that order and the
// first shared secret whose MAC verifies wins. Upstream's MyMesh answers from its ACL and keeps
// its matches in a private array indexed by that number. The mesh client's one peer is added
// **in front** - at index 0, while a reply is awaited and the job's peer matches - and every
// index upstream is then given is shifted down by one, so that upstream's own array is never
// indexed by anything but 0..n-1 of its own count.
//
// In front and not behind, because of the one case where it matters: a peer that is also an
// admin in our own ACL has the same shared secret in both entries, and the first one tried wins.
// Behind, a CLI *reply* from that peer would be run by upstream as a CLI *command*; in front it
// is read as the reply it is.
//
// Host-portable: no Arduino, no MeshCore.

#pragma once

namespace twinoak {
namespace meshclient {

// How many peers Mesh::onRecvPacket is told to try: upstream's own count, plus one when the extra
// peer is in front.
inline int peerSlotTotal(int upstream_count, bool extra) { return upstream_count + (extra ? 1 : 0); }

// Whether an index upstream hands back is the extra peer's: only ever index 0, and only while
// the extra peer is in front.
inline bool peerSlotIsOurs(int index, bool extra) { return extra && index == 0; }

// The index upstream's own functions are given for an index that is not ours: shifted down by
// one while the extra peer is in front, upstream's own otherwise.
inline int peerSlotUpstreamIndex(int index, bool extra) { return extra ? index - 1 : index; }

}  // namespace meshclient
}  // namespace twinoak
