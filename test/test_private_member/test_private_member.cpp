// src/common/cxx/PrivateMember.h: the idiom that lets link 1.2's LOCAL_REQUEST be answered by
// upstream's own private MyMesh::handleRequest out of upstream's own private reply_data, with not
// one file of MeshCore's tree edited (spec/SYSTEM.md S-2 and its exception of 7 November 2026).
//
// The native environment compiles no MeshCore, so what is held here is the **mechanism**, against
// a stand-in class whose shape is the same as MyMesh's in the two ways that matter: a private
// member function that writes into a private member array, and a private member array that the
// caller has to read out of. src/roles/repeater/link/LocalRequest.cpp is the idiom's only real
// use and is compiled by the board environments; the two tags there are the same two moves this
// file makes, against upstream's names instead of these.
//
// If this suite fails, the idiom is wrong and the board build is not where to find out
// (tasks/phase-3.9/02-the-local-request.md: "it must build in the native environment and for the
// board").

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "common/cxx/PrivateMember.h"

// ----------------------------------------------------------------------------- the stand-in

// A named namespace and not an anonymous one: the tags below are in twinoak::cxx and name this
// class, and a template argument with internal linkage is a thing to avoid where it costs
// nothing to.
namespace standin {

// A class nobody may edit, standing in for upstream's MyMesh. Its body begins with no access
// specifier, so everything above `public:` is private - exactly as MyMesh's does
// (examples/simple_repeater/MyMesh.h at the pin 81dbae59).
class Upstream {
  uint8_t reply_data[16];

  int handleRequest(int* sender, uint32_t tag, uint8_t* payload, size_t payload_len) {
    // The same shape as upstream's: the tag reflected into the first four bytes, the answer
    // after them, the length of both returned. `payload_len` is read here and is not by
    // upstream, which is the whole reason its payload buffer has to be zeroed.
    memcpy(reply_data, &tag, 4);
    if (payload[0] != 1) return 0;  // "unknown command": no answer at all
    reply_data[4] = uint8_t(payload[1]);
    reply_data[5] = uint8_t(payload_len);
    *sender += 1;  // something it does to the caller's object, as a real handler does
    return 6;
  }

 public:
  Upstream() { memset(reply_data, 0xEE, sizeof(reply_data)); }

  // What a test can see without the idiom, to prove the idiom read the same bytes.
  uint8_t firstByteOfReply() const { return reply_data[0]; }
};

}  // namespace standin

// ----------------------------------------------------------------------------- the two moves

namespace twinoak {
namespace cxx {

// Move 1: a tag per member, deriving from PrivateMemberOf so that the friend is declared in an
// associated class of the tag. In this namespace, beside the template, because the declaration
// and the definition have to be one function (PrivateMember.h says why).
typedef int (standin::Upstream::*HandleRequestPtr)(int*, uint32_t, uint8_t*, size_t);
typedef uint8_t (standin::Upstream::*ReplyDataPtr)[16];

struct UpstreamHandleRequest : PrivateMemberOf<UpstreamHandleRequest, HandleRequestPtr> {};
struct UpstreamReplyData : PrivateMemberOf<UpstreamReplyData, ReplyDataPtr> {};

// Move 2: the explicit instantiations. These two lines are the only place either private name is
// written, and [temp.explicit] is what makes them legal.
template struct TakePrivateMember<UpstreamHandleRequest, &standin::Upstream::handleRequest>;
template struct TakePrivateMember<UpstreamReplyData, &standin::Upstream::reply_data>;

}  // namespace cxx
}  // namespace twinoak

namespace {

// ----------------------------------------------------------------------------- the tests

using standin::Upstream;

TEST(PrivateMember, APrivateMemberFunctionCanBeCalledThroughTheIdiom) {
  Upstream thing;
  int sender = 0;
  uint8_t payload[4] = {1, 0x5A, 0, 0};

  const int n = (thing.*privateMemberOf(twinoak::cxx::UpstreamHandleRequest()))(
      &sender, 0x11223344u, payload, sizeof(payload));

  EXPECT_EQ(6, n);
  EXPECT_EQ(1, sender) << "the handler ran on this object and not on a copy of it";
  EXPECT_EQ(0x44, thing.firstByteOfReply()) << "it wrote into this object's own buffer";
}

TEST(PrivateMember, APrivateMemberArrayCanBeReadThroughTheIdiom) {
  Upstream thing;
  int sender = 0;
  uint8_t payload[4] = {1, 0x5A, 0, 0};

  const int n = (thing.*privateMemberOf(twinoak::cxx::UpstreamHandleRequest()))(
      &sender, 0x11223344u, payload, sizeof(payload));
  ASSERT_EQ(6, n);

  // The answer, copied out of the private array the way LocalRequest.cpp copies it out of
  // upstream's reply_data: after the four tag bytes.
  const uint8_t* reply = thing.*privateMemberOf(twinoak::cxx::UpstreamReplyData());
  EXPECT_EQ(0x44, reply[0]);
  EXPECT_EQ(0x33, reply[1]);
  EXPECT_EQ(0x22, reply[2]);
  EXPECT_EQ(0x11, reply[3]);
  EXPECT_EQ(0x5A, reply[4]) << "the payload's second byte, as the handler put it there";
  EXPECT_EQ(4u, reply[5]) << "the payload_len it was given";
}

TEST(PrivateMember, TheArrayTagGivesAPointerIntoTheObjectAndNotACopy) {
  Upstream a;
  Upstream b;
  EXPECT_NE(&(a.*privateMemberOf(twinoak::cxx::UpstreamReplyData()))[0],
            &(b.*privateMemberOf(twinoak::cxx::UpstreamReplyData()))[0])
      << "two objects, two buffers: the pointer is into the object it was taken from";

  // And the length is the member's own, not a pointer's: sizeof through the idiom is the array.
  EXPECT_EQ(16u, sizeof(a.*privateMemberOf(twinoak::cxx::UpstreamReplyData())));
}

TEST(PrivateMember, AHandlerThatAnswersNothingIsVisibleAsZero) {
  // What section 5.9 calls "a request the handler gives no answer to", which the session answers
  // ERR_RANGE: upstream's `return 0` at the end of handleRequest, and the stand-in's.
  Upstream thing;
  int sender = 0;
  uint8_t payload[4] = {9, 0, 0, 0};  // not the one command it knows

  const int n = (thing.*privateMemberOf(twinoak::cxx::UpstreamHandleRequest()))(
      &sender, 0x11223344u, payload, sizeof(payload));
  EXPECT_EQ(0, n);
  EXPECT_EQ(0, sender) << "nothing happened to the object";
}

TEST(PrivateMember, TheFunctionTagsTypeIsTheSignatureAndNotAVoidPointer) {
  // The point of the tag carrying `Type`: the pointer comes back typed, so a pin that changes
  // handleRequest's signature is a compile error here and not a wrong call at run time
  // (docs/upstream.md, the place a bump re-reads).
  typedef int (standin::Upstream::*Want)(int*, uint32_t, uint8_t*, size_t);
  const bool same = std::is_same<Want, twinoak::cxx::UpstreamHandleRequest::Type>::value;
  EXPECT_TRUE(same);
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
