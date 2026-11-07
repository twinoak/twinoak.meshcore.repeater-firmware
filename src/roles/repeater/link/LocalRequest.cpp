#include "roles/repeater/link/LocalRequest.h"

#include <string.h>

#include "common/cxx/PrivateMember.h"

// ------------------------------------------------------------------ reaching upstream's handler
//
// The two members this file needs are **private** in `examples/simple_repeater/MyMesh.h` at the
// pin 81dbae59 (repeater-v1.18.0): the class body begins with no access specifier, so everything
// above its `protected:` is private.
//
//   uint8_t reply_data[MAX_PACKET_PAYLOAD];                                       (MyMesh.h:90)
//   int handleRequest(ClientInfo* sender, uint32_t sender_timestamp,
//                     uint8_t* payload, size_t payload_len);                     (MyMesh.h:124)
//
// `spec/SYSTEM.md` S-2 says not one file of upstream's tree is edited, and its *exception* of
// 7 November 2026 names this task. The four moves below are the whole of the reach, and
// src/common/cxx/PrivateMember.h explains the idiom and why it is the standard's own way
// ([temp.explicit]: access checking does not apply to the names of an explicit instantiation).
// `test/test_private_member` holds the mechanism against a stand-in class in the native
// environment, where no board is needed.
//
// The two tags and the two explicit instantiations are in `twinoak::cxx` beside the template,
// which is what makes the friend's declaration and its definition one function and not two
// (PrivateMember.h, move 1).
//
// **A pin bump re-reads both names and the signature** (`docs/upstream.md`, place 9): a rename
// is a link error, a changed signature is a compile error here, and either is caught on the
// bump's own pull request rather than on the air.

namespace twinoak {
namespace cxx {

typedef int (MyMesh::*MyMeshHandleRequestPtr)(ClientInfo*, uint32_t, uint8_t*, size_t);
typedef uint8_t (MyMesh::*MyMeshReplyDataPtr)[MAX_PACKET_PAYLOAD];

struct MyMeshHandleRequest : PrivateMemberOf<MyMeshHandleRequest, MyMeshHandleRequestPtr> {};
struct MyMeshReplyData : PrivateMemberOf<MyMeshReplyData, MyMeshReplyDataPtr> {};

template struct TakePrivateMember<MyMeshHandleRequest, &MyMesh::handleRequest>;
template struct TakePrivateMember<MyMeshReplyData, &MyMesh::reply_data>;

}  // namespace cxx
}  // namespace twinoak

namespace twinoak {
namespace link {

// The answer never needs more room than this link's response payload has behind its status byte,
// which is what spec/LINK-PROTOCOL.md 5.9 says ("the answer is at most what MeshCore's own reply
// buffer holds, which is smaller than this link's payload") - 180 against 239. A pin that grew
// `reply_data` past that would make the clamp in localRequest() start truncating silently, so it
// fails here instead.
static_assert(size_t(MAX_PACKET_PAYLOAD) - kLocalRequestTagBytes <= size_t(kMaxPayload) - 1,
              "upstream's reply_data no longer fits in one LOCAL_REQUEST response");

// And the request: section 5.9's 32 bytes of data plus its `req_type` byte, inside the buffer
// upstream's handler is given.
static_assert(size_t(kLocalRequestMaxData) + 1 <= size_t(MAX_PACKET_PAYLOAD),
              "upstream's payload buffer is narrower than a LOCAL_REQUEST");

int BoardLocalRequest::localRequest(uint8_t req_type, Bytes data, uint8_t* out, size_t cap) {
  if (mesh_ == nullptr) return kLocalRequestNoAnswer;
  if (data.len > kLocalRequestMaxData) return kLocalRequestNoAnswer;  // the session refused it

  // The request, in a zeroed buffer (LocalRequest.h says why it must be zeroed and this wide).
  memset(payload_, 0, sizeof(payload_));
  payload_[0] = req_type;
  if (data.len > 0 && data.ptr != nullptr) memcpy(&payload_[1], data.ptr, data.len);

  // The asker: admin, and not on the access list.
  memset(&sender_, 0, sizeof(sender_));
  sender_.permissions = PERM_ACL_ADMIN;
  sender_.out_path_len = OUT_PATH_UNKNOWN;

  // `sender_timestamp` 0, which upstream reflects into the four tag bytes of its reply
  // (MyMesh.cpp:214, `memcpy(reply_data, &sender_timestamp, 4)`) - and which this link drops,
  // because section 5.9's `data` is the answer *after* those four. Zero is also what the CLI
  // over this link passes for the same field (src/roles/repeater/link/BoardLink.cpp's BoardCli:
  // "there is no sender_timestamp via serial"), and nothing in the status, access-list,
  // neighbours or owner-info branches reads it.
  //
  // **Upstream builds its own replies in the same `reply_data`, and this call cannot meet one
  // half-built.** Every one of upstream's own users of that buffer - `handleLoginReq`, the three
  // anonymous handlers and `handleRequest` itself - is reached from `Mesh::onRecvPacket` inside
  // `the_mesh.loop()`, fills the buffer and has copied it into a packet (`createDatagram` /
  // `createPathReturn`) before returning; nothing of upstream's holds a pointer into it across a
  // return to `loop()`. And `src/roles/repeater/main.cpp` runs `the_mesh.loop()` and then
  // `twinoak::link::linkLoop()`, one after the other on one thread, with no interrupt that
  // reaches either: the link's turn of the loop therefore finds `reply_data` idle. The answer is
  // copied out below, before this function returns, so the next turn of the mesh's loop may have
  // the buffer back.
  const int reply_len = (mesh_->*privateMemberOf(cxx::MyMeshHandleRequest()))(
      &sender_, /*sender_timestamp=*/0, payload_, size_t(data.len) + 1);

  // "A request the handler gives no answer to is ERR_RANGE" (section 5.9): upstream's `return 0`
  // at the end of handleRequest, and its fall-through for a neighbours request of a version it
  // does not know or an access-list request whose reserved bytes are not zero. A reply shorter
  // than its own four tag bytes is not a shape upstream can produce at the pin; it is treated as
  // no answer too, because the alternative would be reading before the buffer.
  if (reply_len <= 0 || reply_len < int(kLocalRequestTagBytes)) return kLocalRequestNoAnswer;

  size_t n = size_t(reply_len) - kLocalRequestTagBytes;
  if (n > cap) n = cap;  // cannot happen: the static_assert above is why
  if (n > 0) {
    const uint8_t* reply = mesh_->*privateMemberOf(cxx::MyMeshReplyData());
    memcpy(out, reply + kLocalRequestTagBytes, n);
  }
  return int(n);
}

}  // namespace link
}  // namespace twinoak
