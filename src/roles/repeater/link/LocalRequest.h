// LOCAL_REQUEST (link 1.2, spec/LINK-PROTOCOL.md 5.9) as this board has it: the node's status,
// its access list, a page of its neighbour table or its owner info, answered by **upstream's own
// request handler** and carried over the box link, with nothing on the air.
//
// The repeater's own Walter reads its repeater this way (meshmaster.dk's *local door*). Until 1.2
// the link carried that repeater's CLI lines and its telemetry and nothing that yielded these
// four answers, so the first unit's repeater stayed *stale* on the server and had none of the
// graphs a status read feeds (tasks/phase-3.8/FOLLOWUPS.md, settled by Thomas on 7 November).
//
// This is the only file of the firmware that names `MyMesh::handleRequest` or
// `MyMesh::reply_data`, both **private** at the pin 81dbae59. How it reaches them without editing
// one file of upstream's is src/common/cxx/PrivateMember.h, and `docs/upstream.md` lists both
// names among the places a bump re-reads. `src/roles/repeater/link/BoardLink.cpp` hands the
// object below to the session, which is what puts `LOCAL_REQUEST` in HELLO's `caps`.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "common/link/session/Interfaces.h"
#include "roles/repeater/app/UpstreamMyMesh.h"

namespace twinoak {
namespace link {

// The session's LocalRequestAccess over upstream's handler. One of these exists
// (src/roles/repeater/link/BoardLink.cpp); its buffers are members and not locals, because a
// request handler runs on the main loop's 2 KB stack.
class BoardLocalRequest : public LocalRequestAccess {
 public:
  // The node. Before the first request; nothing is called before then.
  void attach(MyMesh* mesh) { mesh_ = mesh; }

  // spec/LINK-PROTOCOL.md 5.9. `req_type` is one of section 5.9's four (the session refused
  // every other value before this call) and `data` is at most kLocalRequestMaxData bytes.
  int localRequest(uint8_t req_type, Bytes data, uint8_t* out, size_t cap) override;

 private:
  // The request as upstream's handler reads it: `req_type` then `data`, **zeroed past that**.
  // Section 5.9 says "every byte past `data` reads as zero", and it has to: upstream's
  // handleRequest does not look at `payload_len` at all - its neighbours branch reads
  // payload[1] to payload[6] whatever the request's length was
  // (examples/simple_repeater/MyMesh.cpp:277-290 at the pin) - so a request of one byte leaves
  // it reading five bytes this buffer must have made zero. The buffer is as wide as the
  // plaintext a mesh request could hand the same function, so no pin can read past it into
  // something that is not zero.
  uint8_t payload_[MAX_PACKET_PAYLOAD] = {0};

  // The client upstream's handler is told is asking: `PERM_ACL_ADMIN`, and **a local object that
  // is not in the ACL** - nothing is added to the access list and nothing is saved
  // (helpers/ClientACL.h at the pin: `putClient` is the only thing that adds one, and this is
  // not it). Admin for the reason section 5.7 runs a CLI line as the console runs it: the Walter
  // already holds the SWD lines. A member and not a local: ClientInfo is some 190 bytes. It is
  // zeroed with memset before every call, which is what upstream's own ClientACL constructor
  // does to the whole table (helpers/ClientACL.h at the pin).
  ClientInfo sender_;

  MyMesh* mesh_ = nullptr;
};

}  // namespace link
}  // namespace twinoak
