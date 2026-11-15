#include "roles/repeater/link/FloodScope.h"

#include <string.h>

#include "common/cxx/PrivateMember.h"
#include "roles/repeater/meshclient/MeshPort.h"

// ------------------------------------------------------------------ reaching upstream's scope
//
// The two members this file needs are **private** in `examples/simple_repeater/MyMesh.h` at the
// pin 81dbae59 (repeater-v1.18.0):
//
//   RegionMap region_map, temp_map;                                              (MyMesh.h:94)
//   TransportKey default_scope;                                                  (MyMesh.h:97)
//
// `spec/SYSTEM.md` S-2 says not one file of upstream's tree is edited, and its *second exception*
// of 14 November 2026 names this task. `src/common/cxx/PrivateMember.h` explains the idiom and
// why it is the standard's own way ([temp.explicit]: access checking does not apply to the names
// of an explicit instantiation), and `test/test_private_member` holds the mechanism against a
// stand-in class in the native environment.
//
// The two tags and the two explicit instantiations are in `twinoak::cxx` beside the template,
// which is what makes the friend's declaration and its definition one function and not two
// (PrivateMember.h, move 1).
//
// **A pin bump re-reads both names and both types** (`docs/upstream.md`, place 9): a rename is a
// link error, a changed type is a compile error here, and either is caught on the bump's own pull
// request rather than on the air. `region_map` is reached and not `temp_map`, which is upstream's
// scratch map while a `region load` is in progress and holds no default region of its own.

namespace twinoak {
namespace cxx {

typedef RegionMap(MyMesh::*MyMeshRegionMapPtr);
typedef TransportKey(MyMesh::*MyMeshDefaultScopePtr);

struct MyMeshRegionMap : PrivateMemberOf<MyMeshRegionMap, MyMeshRegionMapPtr> {};
struct MyMeshDefaultScope : PrivateMemberOf<MyMeshDefaultScope, MyMeshDefaultScopePtr> {};

template struct TakePrivateMember<MyMeshRegionMap, &MyMesh::region_map>;
template struct TakePrivateMember<MyMeshDefaultScope, &MyMesh::default_scope>;

}  // namespace cxx
}  // namespace twinoak

namespace twinoak {
namespace link {

// A region's name is `char name[31]` (`src/helpers/RegionMap.h` at the pin), so the longest one
// upstream can hold is 30 characters - which is exactly section 6.5's `scope`, 1 to 30 bytes. A
// pin that widened the field would make the answer encodable only by truncation
// (`encodeFloodScopeResponse` refuses a longer name outright), so it fails here instead.
static_assert(sizeof(RegionEntry::name) - 1 <= size_t(kScopeNameMax),
              "upstream's region name no longer fits in one FLOOD_SCOPE response");

Bytes BoardFloodScope::scopeName() const {
  Bytes out;  // empty: "or empty when the RAK holds none" (6.5)
  if (mesh_ == nullptr) return out;

  // Upstream's own map, and upstream's own idea of which region is the default
  // (`RegionMap::getDefaultRegion`, public, NULL when `default_id` is 0 - which is what
  // `region default <null>` sets and what a `region load` leaves behind). Nothing is written and
  // no region is created: section 6.5's deliverable says in as many words that `FLOOD_SCOPE` is
  // **not** `region default` run through `handleCommand`, because while a `region load` is in
  // progress upstream reads every line as a region's line (`MyMesh::handleCommand`'s
  // `region_load_active` branch, MyMesh.cpp:1198-1230) and a `FLOOD_SCOPE` that arrived then
  // would have added a region to somebody's half-loaded map.
  RegionMap& map = mesh_->*privateMemberOf(cxx::MyMeshRegionMap());
  const RegionEntry* region = map.getDefaultRegion();
  if (region == nullptr) return out;

  // "The default scope's name as `region default` prints it, without a leading `#`" (6.5).
  // Upstream prints `def->name` (`src/helpers/CommonCLI.cpp`'s ` default scope is %s`), and a
  // stored name may begin with `#` - `RegionMap::is_name_char` accepts it and the map's own
  // `skip_hash` drops it when matching, so the hash is decoration and not part of the name.
  const char* name = region->name;
  if (*name == '#') name++;
  // `strnlen` and not `strlen`: `RegionMap::load` reads the 31 bytes of a name straight out of
  // `/regions2`, so a truncated or corrupted file can hand us a field with no terminator.
  size_t len = strnlen(name, sizeof(region->name));
  if (len > size_t(kScopeNameMax)) len = size_t(kScopeNameMax);  // the static_assert is why not
  out.ptr = reinterpret_cast<const uint8_t*>(name);
  out.len = uint8_t(len);
  return out;
}

bool floodInDefaultScope(MyMesh& mesh, mesh::Packet* pkt) {
  // Read at the send and never cached, so that a `region default` which changed since the last
  // one is in force for this one (6.5).
  const TransportKey& scope = mesh.*privateMemberOf(cxx::MyMeshDefaultScope());
  // What will go on the packet, decided before the packet is handed over: upstream's
  // `sendFloodScoped` is a plain `sendFlood` for a null key and computes a transport code from
  // the key and the packet otherwise (`MyMesh.cpp`:999-1008 at the pin). `TransportKey::isNull`
  // is upstream's own test, the same one that branch makes.
  const bool scoped = !scope.isNull();
  // `MyMesh::sendFloodScoped(const TransportKey&, mesh::Packet*, uint32_t delay_millis,
  // uint8_t path_hash_size)`, public at the pin (MyMesh.h:198). No delay, as the client's floods
  // always had none, and **two-byte path hashes** (S-12) - where upstream's own flooded advert
  // passes `_prefs.path_hash_mode + 1` (MyMesh.cpp:1037), which the first-boot profile sets to
  // the same two.
  mesh.sendFloodScoped(scope, pkt, 0, twinoak::meshclient::kFloodPathHashSize);
  return scoped;
}

}  // namespace link
}  // namespace twinoak
