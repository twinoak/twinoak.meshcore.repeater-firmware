// src/meshclient/: the mesh client's portable state (spec/LINK-PROTOCOL.md sections 6.1 to 6.3,
// docs/ARCHITECTURE.md sections 2 and 7) - PeerSlot's index arithmetic, exhaustively, the Job's
// path, its awaited reply, its silence rule and the wipe of its shared secret (task 01), and the
// Reply record with its matching rules against scripted arrivals (task 02).
//
// The peer overrides of TwinOakMesh are not here: they need MeshCore, which a host build does
// not have. What is here is the pieces those overrides are made of - the arithmetic (PeerSlot),
// the state (Job) and the rules (Reply) - each kept separate for exactly that reason. The job
// requests, the five sends, the reply and the counters through a real Session are in
// test/test_link_session.
//
// What is checked, in the order the two task files ask for it:
//   - PeerSlot for every upstream count 0..4 and extra yes/no: the total, which index is ours,
//     the shift;
//   - the Job's three path modes; hash sizes 1 to 3; 63 hops; 64 bytes; one byte too many; a hop
//     count that disagrees with the bytes; the encoding byte for each;
//   - the 120-second silence rule against a fake clock: silence ends the job, any request
//     restarts the count;
//   - the wipe: after end() the buffer that held the secret is zero (read back, as the agent
//     firmware's credential test does);
//   - Reply: the right response; a response with another tag; a response while a text is
//     awaited; a text while a response is awaited; a late reply after the timeout; two replies
//     (the first wins); the timeout itself; and what a CLI reply's plaintext must look like;
//   - the observation ring (task 03): fill, wrap, the loss counted twice, `age_s` measured on the
//     millisecond clock and not on the epoch, and the record the ring refuses;
//   - the pull: fewer than there are, more than there are, zero, and how many fit one payload -
//     including a record with the longest path and the longest application data section 7
//     allows, of which exactly one fits;
//   - the RSSI note table: a pooled packet reused, and a note taken back once;
//   - attention reason 5: off by default, on with `obs.wake`, and only while the Walter is
//     believed asleep;
//   - the send ceiling (task 04) against a fake clock: six in a minute and the seventh refused,
//     the two-second gap, the window sliding, a refused send costing nothing, and the millisecond
//     clock wrapping under it.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "link/codec/Messages.h"
#include "link/codec/Numbers.h"
#include "meshclient/Job.h"
#include "meshclient/LinkObservations.h"
#include "meshclient/ObservationRing.h"
#include "meshclient/PeerSlot.h"
#include "meshclient/Reply.h"
#include "meshclient/RssiNotes.h"
#include "meshclient/SendCeiling.h"

namespace {

using namespace twinoak::meshclient;  // NOLINT: a test of one namespace

// ----------------------------------------------------------------------------- PeerSlot

TEST(PeerSlot, EveryCountAndExtra) {
  for (int count = 0; count <= 4; count++) {
    // No extra peer: every call is upstream's own, the total is its count, no index is ours, and
    // every index is itself.
    EXPECT_EQ(count, peerSlotTotal(count, false)) << "count " << count << ", no extra";
    for (int j = 0; j < count; j++) {
      EXPECT_FALSE(peerSlotIsOurs(j, false)) << "count " << count << ", index " << j;
      EXPECT_EQ(j, peerSlotUpstreamIndex(j, false)) << "count " << count << ", index " << j;
    }

    // The extra peer in front: one more in total, index 0 is ours, and every other index is
    // upstream's shifted down by one - so upstream is only ever given 0..count-1.
    EXPECT_EQ(count + 1, peerSlotTotal(count, true)) << "count " << count << ", extra";
    EXPECT_TRUE(peerSlotIsOurs(0, true)) << "count " << count << ": index 0 is ours";
    for (int j = 1; j <= count; j++) {
      EXPECT_FALSE(peerSlotIsOurs(j, true)) << "count " << count << ", index " << j;
      const int up = peerSlotUpstreamIndex(j, true);
      EXPECT_EQ(j - 1, up) << "count " << count << ", index " << j << ": shifted down by one";
      EXPECT_GE(up, 0);
      EXPECT_LT(up, count) << "upstream is never given an index past its own count";
    }
  }
}

// ----------------------------------------------------------------------------- the encoding

TEST(PathEncoding, TheByteForEachHashSize) {
  // (hash_size - 1) << 6 | hop_count (src/Packet.h at the pin; LINK-PROTOCOL.md section 6.1).
  EXPECT_EQ(0x01u, encodePathLen(1, 1));
  EXPECT_EQ(0x3Fu, encodePathLen(1, 63));
  EXPECT_EQ(0x41u, encodePathLen(2, 1));
  EXPECT_EQ(0x82u, encodePathLen(3, 2));
  EXPECT_EQ(0xBFu, encodePathLen(3, 63));
  // And read back.
  EXPECT_EQ(3u, pathHashSize(0x82));
  EXPECT_EQ(2u, pathHopCount(0x82));
  EXPECT_EQ(1u, pathHashSize(0x3F));
  EXPECT_EQ(63u, pathHopCount(0x3F));
}

TEST(PathLegality, TheBoundsOfSectionSixOne) {
  // Hash sizes 1 to 3, any legal hop count.
  EXPECT_TRUE(pathIsLegal(1, 1, 1));
  EXPECT_TRUE(pathIsLegal(2, 1, 2));
  EXPECT_TRUE(pathIsLegal(3, 1, 3));
  // 63 hops of one byte: 63 bytes, the most hops there are.
  EXPECT_TRUE(pathIsLegal(1, 63, 63));
  // 64 bytes exactly: two-byte hashes, 32 hops - the most bytes there are.
  EXPECT_TRUE(pathIsLegal(2, 32, 64));
  // One byte too many: 66 bytes is past the 64-byte cap, whatever the hop count.
  EXPECT_FALSE(pathIsLegal(3, 22, 66));
  EXPECT_FALSE(pathIsLegal(2, 33, 66));
  // A hash size outside 1 to 3.
  EXPECT_FALSE(pathIsLegal(0, 1, 0));
  EXPECT_FALSE(pathIsLegal(4, 1, 4));
  // A hop count outside 1 to 63.
  EXPECT_FALSE(pathIsLegal(1, 0, 0));
  EXPECT_FALSE(pathIsLegal(1, 64, 64));
  // A hop count that disagrees with the bytes given.
  EXPECT_FALSE(pathIsLegal(2, 2, 3)) << "size 2, 2 hops is 4 bytes, not 3";
  EXPECT_FALSE(pathIsLegal(2, 2, 5));
}

// ----------------------------------------------------------------------------- the job's path

const uint8_t kKey[kPeerKeyLen] = {
    0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07, 0x18, 0x29, 0x3a, 0x4b,
    0x5c, 0x6d, 0x7e, 0x8f, 0x90, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6,
    0x07, 0x18, 0x29, 0x3a, 0x4b, 0x5c, 0x6d, 0x7e, 0x8f, 0x90};

TEST(JobPath, Flood) {
  Job job;
  EXPECT_TRUE(job.begin(kKey, kPathNone, 0, 0, nullptr, 0, 0));
  EXPECT_TRUE(job.begun());
  EXPECT_EQ(kPathNone, job.pathMode());
  EXPECT_EQ(0u, job.pathLen());
  EXPECT_EQ(0u, job.pathByteCount());
  EXPECT_EQ(0, memcmp(kKey, job.peerKey(), kPeerKeyLen));
}

TEST(JobPath, Direct) {
  Job job;
  EXPECT_TRUE(job.begin(kKey, kPathDirect, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(kPathDirect, job.pathMode());
  EXPECT_EQ(0u, job.pathLen()) << "direct is zero hops, sent with an empty path";
  EXPECT_EQ(0u, job.pathByteCount());
}

TEST(JobPath, Hops) {
  Job job;
  const uint8_t hashes[] = {0xa1, 0xb2, 0x7f, 0x3a};  // two hops of two bytes
  EXPECT_TRUE(job.begin(kKey, kPathHops, 2, 2, hashes, sizeof hashes, 0));
  EXPECT_EQ(kPathHops, job.pathMode());
  EXPECT_EQ(encodePathLen(2, 2), job.pathLen());
  EXPECT_EQ(2u, job.pathHashSize());
  EXPECT_EQ(2u, job.pathHopCount());
  EXPECT_EQ(4u, job.pathByteCount());
  EXPECT_EQ(0, memcmp(hashes, job.pathBytes(), sizeof hashes));
}

TEST(JobPath, EveryHashSize) {
  for (uint8_t size = 1; size <= 3; size++) {
    Job job;
    std::vector<uint8_t> hashes(size, 0x5a);  // one hop
    EXPECT_TRUE(job.begin(kKey, kPathHops, size, 1, hashes.data(), hashes.size(), 0))
        << "hash size " << int(size);
    EXPECT_EQ(size, job.pathHashSize());
    EXPECT_EQ(1u, job.pathHopCount());
    EXPECT_EQ(encodePathLen(size, 1), job.pathLen());
  }
}

TEST(JobPath, SixtyThreeHops) {
  Job job;
  std::vector<uint8_t> hashes(63, 0x11);  // size 1, 63 hops
  EXPECT_TRUE(job.begin(kKey, kPathHops, 1, 63, hashes.data(), hashes.size(), 0));
  EXPECT_EQ(63u, job.pathHopCount());
  EXPECT_EQ(63u, job.pathByteCount());
}

TEST(JobPath, SixtyFourBytes) {
  Job job;
  std::vector<uint8_t> hashes(64, 0x22);  // size 2, 32 hops
  EXPECT_TRUE(job.begin(kKey, kPathHops, 2, 32, hashes.data(), hashes.size(), 0));
  EXPECT_EQ(64u, job.pathByteCount());
}

TEST(JobPath, OneByteTooMany) {
  Job job;
  std::vector<uint8_t> hashes(66, 0x33);  // size 3, 22 hops: 66 bytes
  EXPECT_FALSE(job.begin(kKey, kPathHops, 3, 22, hashes.data(), hashes.size(), 0));
  EXPECT_FALSE(job.begun()) << "a refused JOB_BEGIN changes nothing";
}

TEST(JobPath, AHopCountThatDisagreesWithTheBytes) {
  Job job;
  const uint8_t hashes[] = {0xa1, 0xb2, 0x7f};  // 3 bytes, but size 2 x 2 hops wants 4
  EXPECT_FALSE(job.begin(kKey, kPathHops, 2, 2, hashes, sizeof hashes, 0));
  EXPECT_FALSE(job.begun());
}

TEST(JobPath, ASecondBeginEndsTheFirst) {
  Job job;
  const uint8_t hashes[] = {0xa1, 0xb2};
  EXPECT_TRUE(job.begin(kKey, kPathHops, 2, 1, hashes, sizeof hashes, 0));
  const uint8_t other[kPeerKeyLen] = {0x01};
  EXPECT_TRUE(job.begin(other, kPathNone, 0, 0, nullptr, 0, 0));
  EXPECT_EQ(kPathNone, job.pathMode()) << "the new job's path, not the old one's";
  EXPECT_EQ(0, memcmp(other, job.peerKey(), kPeerKeyLen));
}

TEST(JobPath, SetPathNeedsAJobAndReplacesTheRoute) {
  Job job;
  EXPECT_FALSE(job.setPath(kPathDirect, 0, 0, nullptr, 0)) << "no job: refused";
  EXPECT_TRUE(job.begin(kKey, kPathNone, 0, 0, nullptr, 0, 0));
  const uint8_t hashes[] = {0x7f, 0x3a};
  EXPECT_TRUE(job.setPath(kPathHops, 2, 1, hashes, sizeof hashes));
  EXPECT_EQ(kPathHops, job.pathMode());
  EXPECT_EQ(1u, job.pathHopCount());
  // Mode none forgets it.
  EXPECT_TRUE(job.setPath(kPathNone, 0, 0, nullptr, 0));
  EXPECT_EQ(kPathNone, job.pathMode());
  EXPECT_EQ(0u, job.pathByteCount());
  // An illegal path is refused and the route is unchanged.
  EXPECT_TRUE(job.setPath(kPathHops, 2, 1, hashes, sizeof hashes));
  EXPECT_FALSE(job.setPath(kPathHops, 2, 2, hashes, sizeof hashes)) << "3 bytes for 4";
  EXPECT_EQ(1u, job.pathHopCount()) << "unchanged by the refusal";
}

// ----------------------------------------------------------------------------- a reply awaited

TEST(JobAwait, ArmAndClear) {
  Job job;
  EXPECT_TRUE(job.begin(kKey, kPathNone, 0, 0, nullptr, 0, 1000));
  EXPECT_FALSE(job.awaiting());
  job.arm(kAwaitResponse, 0x1234, 1000 + 12000);
  EXPECT_TRUE(job.awaiting());
  EXPECT_EQ(kAwaitResponse, job.awaitKind());
  EXPECT_EQ(0x1234u, job.awaitTag());
  EXPECT_FALSE(job.awaitExpired(1000 + 11999));
  EXPECT_TRUE(job.awaitExpired(1000 + 12000));
  job.clearAwait();
  EXPECT_FALSE(job.awaiting());
  // A fresh job, and JOB_END, both clear it.
  job.arm(kAwaitText, 0, 50);
  job.end();
  EXPECT_FALSE(job.awaiting());
}

// ----------------------------------------------------------------------------- the silence rule

TEST(JobSilence, SilenceEndsTheJobAfterTwoMinutes) {
  Job job;
  EXPECT_TRUE(job.begin(kKey, kPathNone, 0, 0, nullptr, 0, /*now=*/0));
  EXPECT_FALSE(job.loop(kIdleEndMs - 1)) << "one millisecond short";
  EXPECT_TRUE(job.begun());
  EXPECT_TRUE(job.loop(kIdleEndMs)) << "exactly 120 s of silence ends it";
  EXPECT_FALSE(job.begun());
  EXPECT_EQ(1u, job.endedForSilence());
  // And a job never begun is never ended.
  EXPECT_FALSE(job.loop(kIdleEndMs * 10));
}

TEST(JobSilence, AnyRequestRestartsTheCount) {
  Job job;
  EXPECT_TRUE(job.begin(kKey, kPathNone, 0, 0, nullptr, 0, /*now=*/0));
  // A request at 100 s: the count starts again from there.
  job.noteLinkRequest(100000);
  EXPECT_FALSE(job.loop(100000 + kIdleEndMs - 1));
  EXPECT_TRUE(job.begun());
  // Another at 150 s, before the first count would have ended it.
  job.noteLinkRequest(150000);
  EXPECT_FALSE(job.loop(150000 + kIdleEndMs - 1));
  EXPECT_TRUE(job.loop(150000 + kIdleEndMs));
  EXPECT_FALSE(job.begun());
}

TEST(JobSilence, TheClockMayWrap) {
  Job job;
  const uint32_t near_wrap = 0xFFFFFFFFu - 1000;
  EXPECT_TRUE(job.begin(kKey, kPathNone, 0, 0, nullptr, 0, near_wrap));
  // 120 s later, past the uint32 wrap: the signed difference still says two minutes.
  const uint32_t later = near_wrap + kIdleEndMs;  // wraps
  EXPECT_FALSE(job.loop(near_wrap + kIdleEndMs - 1));
  EXPECT_TRUE(job.loop(later));
  EXPECT_FALSE(job.begun());
}

// ----------------------------------------------------------------------------- the wipe

TEST(JobSecret, TheBufferIsZeroedWhenTheJobEnds) {
  Job job;
  EXPECT_TRUE(job.begin(kKey, kPathNone, 0, 0, nullptr, 0, 0));
  EXPECT_FALSE(job.hasSecret());
  // The board writes the ECDH secret into this buffer; stand in for it with a non-zero pattern.
  uint8_t* secret = job.secretBuffer();
  for (uint8_t i = 0; i < kSecretLen; i++) secret[i] = uint8_t(0xA5 ^ i);
  job.markSecret();
  EXPECT_TRUE(job.hasSecret());

  job.end();

  EXPECT_FALSE(job.hasSecret());
  // Read it back, as the agent firmware's credential test reads its buffer back: every byte is 0.
  const uint8_t* after = job.secretBuffer();
  for (uint8_t i = 0; i < kSecretLen; i++) {
    EXPECT_EQ(0, after[i]) << "secret byte " << int(i) << " was not wiped";
  }
}

TEST(JobSecret, ASecondBeginWipesTheFirstsSecret) {
  Job job;
  EXPECT_TRUE(job.begin(kKey, kPathNone, 0, 0, nullptr, 0, 0));
  uint8_t* secret = job.secretBuffer();
  for (uint8_t i = 0; i < kSecretLen; i++) secret[i] = 0x5A;
  job.markSecret();

  const uint8_t other[kPeerKeyLen] = {0x02};
  EXPECT_TRUE(job.begin(other, kPathNone, 0, 0, nullptr, 0, 0));
  EXPECT_FALSE(job.hasSecret()) << "the new job has no secret yet";
  const uint8_t* after = job.secretBuffer();
  for (uint8_t i = 0; i < kSecretLen; i++) {
    EXPECT_EQ(0, after[i]) << "the first job's secret survived into the second";
  }
}

// ----------------------------------------------------------------------------- the reply's rules

TEST(ReplyRules, WhatIsAwaitedAndByWhichTag) {
  // A response is awaited by a login (tag 0: the next from the peer), a request and a discovery
  // (the timestamp); a text by a CLI line; a trace by a trace.
  EXPECT_TRUE(responseIsAwaited(kAwaitResponse, 0, 123456)) << "tag 0 takes the next response";
  EXPECT_TRUE(responseIsAwaited(kAwaitResponse, 777, 777));
  EXPECT_FALSE(responseIsAwaited(kAwaitResponse, 777, 778)) << "another tag";
  EXPECT_FALSE(responseIsAwaited(kAwaitText, 0, 777)) << "a response while a text is awaited";
  EXPECT_FALSE(responseIsAwaited(kAwaitTrace, 777, 777)) << "a response while a trace is awaited";
  EXPECT_FALSE(responseIsAwaited(kAwaitNone, 0, 777)) << "nothing awaited";

  EXPECT_TRUE(textIsAwaited(kAwaitText));
  EXPECT_FALSE(textIsAwaited(kAwaitResponse)) << "a text while a response is awaited";
  EXPECT_FALSE(textIsAwaited(kAwaitTrace));
  EXPECT_FALSE(textIsAwaited(kAwaitNone));

  EXPECT_TRUE(traceIsAwaited(kAwaitTrace, 900, 900));
  EXPECT_TRUE(traceIsAwaited(kAwaitTrace, 0, 900));
  EXPECT_FALSE(traceIsAwaited(kAwaitTrace, 900, 901));
  EXPECT_FALSE(traceIsAwaited(kAwaitResponse, 900, 900));
  EXPECT_FALSE(traceIsAwaited(kAwaitNone, 0, 900));
}

TEST(ReplyRules, ACliReplysPlaintext) {
  // The repeater's TXT_MSG: its timestamp, a flags byte with the text type in its upper six bits,
  // the text, and the zero padding of a cipher block.
  const uint8_t* text = nullptr;
  size_t n = 0;
  const uint8_t cli[] = {1, 2, 3, 4, 0x04, '>', ' ', 'o', 'k', 0, 0, 0, 0, 0, 0, 0};
  EXPECT_TRUE(parseCliText(cli, sizeof cli, text, n));
  ASSERT_EQ(4u, n) << "up to the first NUL";
  EXPECT_EQ(0, memcmp(text, "> ok", 4));
  // A pre-v1.17 repeater says TXT_TYPE_PLAIN, which the agent firmware reads as a CLI reply too.
  const uint8_t plain[] = {1, 2, 3, 4, 0x00, 'o', 'k'};
  EXPECT_TRUE(parseCliText(plain, sizeof plain, text, n));
  EXPECT_EQ(2u, n);
  // Anything else is not a reply this client reads: a signed text, a text with no bytes after
  // its header, a header that is not even whole.
  const uint8_t signed_text[] = {1, 2, 3, 4, uint8_t(2 << 2), 'o', 'k'};
  EXPECT_FALSE(parseCliText(signed_text, sizeof signed_text, text, n));
  const uint8_t empty[] = {1, 2, 3, 4, 0x04};
  EXPECT_FALSE(parseCliText(empty, sizeof empty, text, n));
  EXPECT_FALSE(parseCliText(empty, 3, text, n));
  EXPECT_FALSE(parseCliText(nullptr, 20, text, n));
  // The attempt bits in the low two bits of the flags byte do not change the type.
  const uint8_t attempt[] = {1, 2, 3, 4, uint8_t(0x04 | 0x02), 'x', 0};
  EXPECT_TRUE(parseCliText(attempt, sizeof attempt, text, n));
  EXPECT_EQ(1u, n);
}

namespace {

std::vector<uint8_t> responseOf(uint32_t tag, const std::vector<uint8_t>& rest) {
  std::vector<uint8_t> d = {uint8_t(tag), uint8_t(tag >> 8), uint8_t(tag >> 16),
                            uint8_t(tag >> 24)};
  d.insert(d.end(), rest.begin(), rest.end());
  return d;
}

}  // namespace

TEST(ReplyRecord, TheRightResponseIsTakenAndReported) {
  Reply r;
  EXPECT_EQ(kReplyNone, r.state());
  EXPECT_FALSE(r.eventDue());
  EXPECT_EQ(0u, r.elapsedMs(5000));

  r.arm(/*tag=*/4242, /*now=*/1000);
  EXPECT_EQ(kReplyPending, r.state());
  EXPECT_EQ(4242u, r.tag());
  EXPECT_EQ(1200u, r.elapsedMs(2200)) << "pending: from the send to now";
  EXPECT_FALSE(r.eventDue());

  const std::vector<uint8_t> data = responseOf(4242, {0xaa, 0xbb, 0xcc});
  EXPECT_TRUE(r.takeResponse(kAwaitResponse, 4242, data.data(), data.size(), false, 25, -97, 3100));
  EXPECT_EQ(kReplyResponse, r.state());
  EXPECT_EQ(4242u, r.tag());
  EXPECT_FALSE(r.viaPath());
  EXPECT_EQ(25, r.snrQ4());
  EXPECT_EQ(-97, r.rssi());
  EXPECT_EQ(2100u, r.elapsedMs(9999)) << "an outcome: from the send to the reply, fixed";
  ASSERT_EQ(3u, r.dataLen());
  EXPECT_EQ(0, memcmp(r.data(), "\xaa\xbb\xcc", 3)) << "the response after its four tag bytes";
  EXPECT_TRUE(r.eventDue());
  r.eventTaken();
  EXPECT_FALSE(r.eventDue());
  EXPECT_EQ(kReplyResponse, r.state()) << "the record is kept after the event";
}

TEST(ReplyRecord, AResponseWithAnotherTagIsDropped) {
  Reply r;
  r.arm(4242, 1000);
  const std::vector<uint8_t> other = responseOf(4243, {0x01});
  EXPECT_FALSE(r.takeResponse(kAwaitResponse, 4242, other.data(), other.size(), false, 0, 0, 1500));
  EXPECT_EQ(kReplyPending, r.state()) << "still waiting";
  EXPECT_FALSE(r.eventDue());
  // Tag 0 awaited: the same response is taken, and the record's tag becomes the reply's own.
  Reply login;
  login.arm(0, 1000);
  EXPECT_TRUE(login.takeResponse(kAwaitResponse, 0, other.data(), other.size(), false, 0, 0, 1500));
  EXPECT_EQ(4243u, login.tag());
}

TEST(ReplyRecord, AResponseWhileATextIsAwaitedIsDroppedAndSoIsTheReverse) {
  Reply r;
  r.arm(0, 1000);  // a CLI line: a text is awaited
  const std::vector<uint8_t> resp = responseOf(0, {0x01});
  EXPECT_FALSE(r.takeResponse(kAwaitText, 0, resp.data(), resp.size(), false, 0, 0, 1500));
  EXPECT_EQ(kReplyPending, r.state());
  const uint8_t text[] = {1, 2, 3, 4, 0x04, 'o', 'k', 0};
  EXPECT_TRUE(r.takeText(kAwaitText, text, sizeof text, 31, -80, 3890));
  EXPECT_EQ(kReplyText, r.state());
  EXPECT_EQ(0u, r.tag()) << "a CLI reply carries no tag";
  ASSERT_EQ(2u, r.dataLen());
  EXPECT_EQ(0, memcmp(r.data(), "ok", 2));
  EXPECT_EQ(2890u, r.elapsedMs(0));

  Reply q;
  q.arm(77, 1000);  // a request: a response is awaited
  EXPECT_FALSE(q.takeText(kAwaitResponse, text, sizeof text, 0, 0, 1500));
  EXPECT_EQ(kReplyPending, q.state());
  const uint8_t not_cli[] = {1, 2, 3, 4, uint8_t(2 << 2), 'o', 'k'};
  Reply c;
  c.arm(0, 1000);
  EXPECT_FALSE(c.takeText(kAwaitText, not_cli, sizeof not_cli, 0, 0, 1500))
      << "a text that is not CLI data";
  EXPECT_EQ(kReplyPending, c.state());
}

TEST(ReplyRecord, TheFirstReplyWinsAndALateOneIsDropped) {
  Reply r;
  r.arm(10, 1000);
  const std::vector<uint8_t> first = responseOf(10, {0x01});
  const std::vector<uint8_t> second = responseOf(10, {0x02});
  EXPECT_TRUE(r.takeResponse(kAwaitResponse, 10, first.data(), first.size(), false, 0, 0, 1100));
  EXPECT_FALSE(r.takeResponse(kAwaitResponse, 10, second.data(), second.size(), false, 0, 0, 1200))
      << "the second is dropped";
  ASSERT_EQ(1u, r.dataLen());
  EXPECT_EQ(0x01, r.data()[0]) << "the first's bytes";
  EXPECT_EQ(100u, r.elapsedMs(0));

  // After the timeout the record is not pending, so a late reply is dropped too.
  Reply t;
  t.arm(11, 1000);
  EXPECT_TRUE(t.timeOut(1000 + 8000));
  EXPECT_EQ(kReplyTimeout, t.state());
  EXPECT_TRUE(t.eventDue());
  const std::vector<uint8_t> late = responseOf(11, {0x03});
  EXPECT_FALSE(t.takeResponse(kAwaitResponse, 11, late.data(), late.size(), false, 0, 0, 9500));
  EXPECT_EQ(kReplyTimeout, t.state());
  EXPECT_EQ(0u, t.dataLen());
}

TEST(ReplyRecord, TheTimeoutItself) {
  Reply r;
  EXPECT_FALSE(r.timeOut(5000)) << "nothing pending: nothing times out";
  r.arm(1791720055, 1000);
  EXPECT_TRUE(r.timeOut(1000 + 8000));
  EXPECT_EQ(kReplyTimeout, r.state());
  EXPECT_EQ(1791720055u, r.tag()) << "the awaited tag";
  EXPECT_EQ(8000u, r.elapsedMs(0)) << "exactly the timeout the send asked for";
  EXPECT_EQ(kReplyRssiUnknown, r.rssi());
  EXPECT_EQ(0, r.snrQ4());
  EXPECT_FALSE(r.viaPath());
  EXPECT_TRUE(r.eventDue());
  EXPECT_FALSE(r.timeOut(20000)) << "once";
  r.eventTaken();
  EXPECT_FALSE(r.eventDue());
  // clear() lets it go.
  r.clear();
  EXPECT_EQ(kReplyNone, r.state());
  EXPECT_EQ(0u, r.tag());
}

TEST(ReplyRecord, ATraceWithTheAwaitedTag) {
  Reply r;
  r.arm(500, 1000);
  const uint8_t hashes[] = {0x7f, 0x3a, 0xa1, 0xb2, 0x7f, 0x3a, 0x46, 0xf6};
  const uint8_t snrs[] = {0x1d, 0xf6, 0x14, 0x16};
  EXPECT_FALSE(r.takeTrace(kAwaitTrace, 500, 501, 1, hashes, 8, snrs, 4, 22, -101, 7100))
      << "another tag";
  EXPECT_FALSE(r.takeTrace(kAwaitResponse, 500, 500, 1, hashes, 8, snrs, 4, 22, -101, 7100))
      << "a trace while a response is awaited";
  EXPECT_TRUE(r.takeTrace(kAwaitTrace, 500, 500, 1, hashes, 8, snrs, 4, 22, -101, 7100));
  EXPECT_EQ(kReplyTrace, r.state());
  EXPECT_EQ(500u, r.tag());
  EXPECT_EQ(1, r.traceFlags());
  EXPECT_EQ(22, r.finalSnrQ4());
  EXPECT_EQ(22, r.snrQ4());
  EXPECT_EQ(-101, r.rssi());
  EXPECT_EQ(6100u, r.elapsedMs(0));
  ASSERT_EQ(8u, r.traceHashLen());
  EXPECT_EQ(0, memcmp(r.traceHashes(), hashes, 8));
  ASSERT_EQ(4u, r.traceSnrCount());
  EXPECT_EQ(0, memcmp(r.traceSnrs(), snrs, 4));
  // Lengths past the record's buffers are cut, never written past.
  Reply big;
  big.arm(1, 0);
  std::vector<uint8_t> many(300, 0x55);
  EXPECT_TRUE(big.takeTrace(kAwaitTrace, 1, 1, 0, many.data(), many.size(), many.data(),
                            many.size(), 0, 0, 10));
  EXPECT_EQ(size_t(kTraceHashesMax), big.traceHashLen());
  EXPECT_EQ(size_t(kTraceSnrsMax), big.traceSnrCount());
  Reply wide;
  wide.arm(2, 0);
  std::vector<uint8_t> resp = responseOf(2, std::vector<uint8_t>(300, 0x66));
  EXPECT_TRUE(wide.takeResponse(kAwaitResponse, 2, resp.data(), resp.size(), true, 0, 0, 10));
  EXPECT_EQ(size_t(kReplyDataMax), wide.dataLen());
  EXPECT_TRUE(wide.viaPath());
  // A response too short to carry a tag is not one.
  Reply shortr;
  shortr.arm(3, 0);
  const uint8_t three[] = {1, 2, 3};
  EXPECT_FALSE(shortr.takeResponse(kAwaitResponse, 0, three, 3, false, 0, 0, 10));
}

// ----------------------------------------------------------------------------- the ring

// A record of section 7, with everything but the fields a test is about left plain.
HeardAdvert advertOf(uint8_t key_byte, uint32_t adv_ts, uint32_t heard_ms) {
  HeardAdvert a;
  for (uint8_t i = 0; i < kObsPubKeyLen; i++) a.pubkey[i] = uint8_t(key_byte + i);
  a.adv_ts = adv_ts;
  a.heard_ms = heard_ms;
  a.snr_q4 = 29;
  a.rssi = -84;
  a.path_len = 0;  // a direct neighbour: no hops
  a.app_len = 3;
  a.app_data[0] = 0x82;
  a.app_data[1] = 'h';
  a.app_data[2] = 'i';
  return a;
}

TEST(TheRing, ThePathByteCountIsTheCodecsOwnForEveryPathLen) {
  // The ring judges a record's path with the job's encoding and the codec encodes it with
  // Numbers.h's. They are the same arithmetic, held together here for all 256 values so that
  // neither can drift.
  for (int i = 0; i < 256; i++) {
    const uint8_t path_len = uint8_t(i);
    EXPECT_EQ(twinoak::link::obsPathBytes(path_len), obsPathByteCount(path_len))
        << "path_len " << i;
  }
}

TEST(TheRing, FillWrapAndTheLossCountedTwice) {
  ObservationRing ring;
  EXPECT_TRUE(ring.empty());
  EXPECT_EQ(0u, ring.size());
  EXPECT_EQ(0u, ring.adverts());

  // Fill it exactly: nothing is lost.
  for (uint8_t i = 0; i < kObsCapacity; i++) {
    EXPECT_TRUE(ring.push(advertOf(i, 1000u + i, 1000u * i)));
  }
  EXPECT_TRUE(ring.full());
  EXPECT_EQ(kObsCapacity, ring.size());
  EXPECT_EQ(uint32_t(kObsCapacity), ring.adverts());
  EXPECT_EQ(0u, ring.observationsDropped());
  EXPECT_EQ(0u, ring.droppedSincePull());
  EXPECT_EQ(1000u, ring.at(0).adv_ts) << "0 is the oldest";
  EXPECT_EQ(1000u + kObsCapacity - 1, ring.at(kObsCapacity - 1).adv_ts);

  // Three more: the three oldest go, and the loss is counted since boot and since the last pull.
  for (uint8_t i = 0; i < 3; i++) {
    EXPECT_TRUE(ring.push(advertOf(uint8_t(0x80 + i), 2000u + i, 0)));
  }
  EXPECT_EQ(kObsCapacity, ring.size()) << "a full ring stays full";
  EXPECT_EQ(3u, ring.observationsDropped());
  EXPECT_EQ(3u, ring.droppedSincePull());
  EXPECT_EQ(3u, ring.droppedSinceBootSaturating());
  EXPECT_EQ(uint32_t(kObsCapacity) + 3u, ring.adverts());
  EXPECT_EQ(1003u, ring.at(0).adv_ts) << "the three oldest were the ones overwritten";
  EXPECT_EQ(2002u, ring.at(kObsCapacity - 1).adv_ts) << "and the newest is the last one in";

  // A pull reads the since-last count and resets it; the since-boot count keeps going.
  ring.notePulled();
  EXPECT_EQ(0u, ring.droppedSincePull());
  EXPECT_EQ(3u, ring.observationsDropped());

  // Out of range is the oldest, as the agent firmware's at() answers it.
  EXPECT_EQ(ring.at(0).adv_ts, ring.at(kObsCapacity).adv_ts);

  // And the ring wraps cleanly: consume some, push some, the order holds.
  ring.consume(10);
  EXPECT_EQ(kObsCapacity - 10, ring.size());
  EXPECT_EQ(1013u, ring.at(0).adv_ts);
  for (uint8_t i = 0; i < 10; i++) EXPECT_TRUE(ring.push(advertOf(i, 3000u + i, 0)));
  EXPECT_TRUE(ring.full());
  EXPECT_EQ(1013u, ring.at(0).adv_ts);
  EXPECT_EQ(3009u, ring.at(kObsCapacity - 1).adv_ts);
  EXPECT_EQ(3u, ring.observationsDropped()) << "nothing more was lost";

  // consume(more than there are) is all there are; clear() keeps the counters.
  ring.consume(255);
  EXPECT_TRUE(ring.empty());
  ring.push(advertOf(1, 4000, 0));
  ring.clear();
  EXPECT_TRUE(ring.empty());
  EXPECT_EQ(uint32_t(kObsCapacity) + 14u, ring.adverts()) << "it still heard what it heard";
}

TEST(TheRing, ARecordItCannotCarryIsCountedAndNotKept) {
  ObservationRing ring;
  // A path_len whose hash size is 4 - which MeshCore reserves and Dispatcher::tryParsePacket
  // refuses, so it cannot arrive on this pin - would need 4 x 63 bytes of path. The ring refuses
  // it, counts it as an advert dropped, and keeps nothing.
  HeardAdvert bad = advertOf(1, 1000, 0);
  bad.path_len = uint8_t(0xFF);
  EXPECT_EQ(4u, pathHashSize(bad.path_len));
  EXPECT_FALSE(ring.push(bad));
  EXPECT_TRUE(ring.empty());
  EXPECT_EQ(1u, ring.adverts());
  EXPECT_EQ(1u, ring.advertsDropped());
  EXPECT_EQ(0u, ring.observationsDropped()) << "a refused record is not a ring that overflowed";

  // So is an app_len past section 7's cap.
  HeardAdvert wide = advertOf(2, 1001, 0);
  wide.app_len = kObsAppDataMax + 1;
  EXPECT_FALSE(ring.push(wide));
  EXPECT_EQ(2u, ring.advertsDropped());

  // The longest record section 7 does allow is kept: 32 hops of two bytes, 32 bytes of app data.
  HeardAdvert longest = advertOf(3, 1002, 0);
  longest.path_len = encodePathLen(2, 32);
  longest.app_len = kObsAppDataMax;
  EXPECT_TRUE(ring.push(longest));
  EXPECT_EQ(1u, ring.size());
  EXPECT_EQ(kObsPathBytesMax, obsPathByteCount(longest.path_len));
}

TEST(TheRing, AgeIsMeasuredOnTheMillisecondClockAndNotOnTheEpoch) {
  // "age_s: seconds since this node heard it - measured, so it is right whatever the clock said"
  // (section 7). The record holds the advertiser's timestamp and the millisecond clock, and
  // nothing in it moves when somebody sets the epoch.
  HeardAdvert a = advertOf(1, 1791719000u, 10000u);
  EXPECT_EQ(0u, ageSeconds(a, 10000u));
  EXPECT_EQ(41u, ageSeconds(a, 10000u + 41999u)) << "whole seconds, rounded down";
  EXPECT_EQ(300u, ageSeconds(a, 10000u + 300000u));
  // A TIME_SET of years changes nothing here: there is no epoch in this arithmetic at all.
  EXPECT_EQ(1791719000u, a.adv_ts);
  EXPECT_EQ(300u, ageSeconds(a, 10000u + 300000u));
  // And across a millis() wrap it is still one subtraction.
  HeardAdvert late = advertOf(1, 1791719000u, 0xFFFFF000u);
  EXPECT_EQ(8u, ageSeconds(late, 0xFFFFF000u + 8000u));
}

// ----------------------------------------------------------------------------- the pull

// What encodeObsPullResponse() makes of a pull, so that the fitter is held against the encoder
// and not against an arithmetic repeated in the test.
size_t encodedSize(const twinoak::link::ObsPullResponse& out) {
  uint8_t buf[twinoak::link::kMaxPayload];
  return twinoak::link::encodeObsPullResponse(out, buf, sizeof(buf));
}

TEST(ThePull, TheHeaderIsTheFiveBytesTheFitterAssumes) {
  twinoak::link::ObsPullResponse empty;
  EXPECT_EQ(kObsPullHeaderBytes, encodedSize(empty))
      << "status, remaining, dropped, count (section 7)";
}

TEST(ThePull, FewerThanThereAreMoreThanThereAreAndZero) {
  ObservationRing ring;
  LinkObservations obs(ring);
  for (uint8_t i = 0; i < 7; i++) EXPECT_TRUE(ring.push(advertOf(i, 1000u + i, 1000u * i)));
  EXPECT_EQ(7u, obs.pending());

  // Zero: a pull that asks for nothing takes nothing and drains nothing - but it is still a pull,
  // so `dropped` is reported and reset.
  twinoak::link::ObsPullResponse none;
  obs.pull(0, none, 10000u);
  EXPECT_EQ(0u, none.count);
  EXPECT_EQ(7u, none.remaining);
  EXPECT_EQ(0u, none.dropped);
  EXPECT_EQ(7u, obs.pending());

  // Fewer than there are: the oldest three, oldest first, and they leave the ring.
  twinoak::link::ObsPullResponse three;
  obs.pull(3, three, 10000u);
  ASSERT_EQ(3u, three.count);
  EXPECT_EQ(1000u, three.records[0].adv_ts);
  EXPECT_EQ(1001u, three.records[1].adv_ts);
  EXPECT_EQ(1002u, three.records[2].adv_ts);
  EXPECT_EQ(10u, three.records[0].age_s) << "heard at 0 ms, pulled at 10000 ms";
  EXPECT_EQ(8u, three.records[2].age_s);
  EXPECT_EQ(4u, three.remaining);
  EXPECT_EQ(4u, obs.pending());
  EXPECT_LE(encodedSize(three), size_t(twinoak::link::kMaxPayload));

  // More than there are: what is left, and no more.
  twinoak::link::ObsPullResponse many;
  obs.pull(255, many, 10000u);
  ASSERT_EQ(4u, many.count) << "four left, and four of these fit one payload";
  EXPECT_EQ(1003u, many.records[0].adv_ts);
  EXPECT_EQ(0u, many.remaining);
  EXPECT_TRUE(ring.empty());

  // An empty ring answers nothing and says so.
  twinoak::link::ObsPullResponse after;
  obs.pull(10, after, 11000u);
  EXPECT_EQ(0u, after.count);
  EXPECT_EQ(0u, after.remaining);
}

TEST(ThePull, NoMoreThanFitOnePayloadAndTheDroppedCountIsSinceTheLastPull) {
  ObservationRing ring;
  LinkObservations obs(ring);
  // Every record as short as one gets (no path, no app data): five of them fill the payload.
  for (uint8_t i = 0; i < 12; i++) {
    HeardAdvert a = advertOf(i, 1000u + i, 0);
    a.app_len = 0;
    EXPECT_TRUE(ring.push(a));
  }
  twinoak::link::ObsPullResponse out;
  obs.pull(255, out, 0);
  EXPECT_EQ(kObsPullMaxRecords, out.count);
  EXPECT_EQ(5u, out.count) << "235 bytes of payload over 45 bytes a record";
  EXPECT_EQ(7u, out.remaining);
  const size_t encoded = encodedSize(out);
  EXPECT_LE(encoded, size_t(twinoak::link::kMaxPayload));
  EXPECT_GT(encoded + kObsRecordFixedBytes, size_t(twinoak::link::kMaxPayload))
      << "and one more of them would not have fitted";

  // Overflow the ring between two pulls: `dropped` counts what was lost since the last one and
  // then starts again.
  for (uint8_t i = 0; i < kObsCapacity + 2; i++) {
    HeardAdvert a = advertOf(i, 5000u + i, 0);
    a.app_len = 0;
    ring.push(a);
  }
  twinoak::link::ObsPullResponse second;
  obs.pull(1, second, 0);
  EXPECT_EQ(1u, second.count);
  EXPECT_EQ(9u, second.dropped) << "seven left over plus two past the ring's own capacity";
  twinoak::link::ObsPullResponse third;
  obs.pull(1, third, 0);
  EXPECT_EQ(0u, third.dropped) << "lost since the last OBS_PULL, and that was the last one";
  EXPECT_EQ(9u, obs.observationsDropped()) << "the since-boot count is untouched by a pull";
  EXPECT_EQ(9u, obs.droppedSinceBoot());
}

TEST(ThePull, OneRecordWithTheLongestPathAndTheLongestAppDataFitsAndOnlyOne) {
  ObservationRing ring;
  LinkObservations obs(ring);
  // The longest record section 7 allows: 64 bytes of path (32 hops of two bytes) and 32 bytes of
  // application data - 141 bytes encoded. Two of them would be 282, so one payload holds one.
  for (uint8_t n = 0; n < 3; n++) {
    HeardAdvert a = advertOf(n, 1000u + n, 0);
    a.path_len = encodePathLen(2, 32);
    for (uint8_t i = 0; i < kObsPathBytesMax; i++) a.path[i] = uint8_t(0xA0 + i);
    a.app_len = kObsAppDataMax;
    for (uint8_t i = 0; i < kObsAppDataMax; i++) a.app_data[i] = uint8_t(0x40 + i);
    EXPECT_TRUE(ring.push(a));
  }

  twinoak::link::ObsPullResponse out;
  obs.pull(255, out, 0);
  ASSERT_EQ(1u, out.count) << "only one record this long fits a 240-byte payload";
  EXPECT_EQ(2u, out.remaining);
  EXPECT_EQ(kObsPathBytesMax, out.records[0].path.len);
  EXPECT_EQ(kObsAppDataMax, out.records[0].app_data.len);
  const size_t encoded = encodedSize(out);
  EXPECT_EQ(kObsPullHeaderBytes + kObsRecordFixedBytes + kObsPathBytesMax + kObsAppDataMax,
            encoded);
  EXPECT_LE(encoded, size_t(twinoak::link::kMaxPayload));

  // And the next pull hands out the next one, in order.
  twinoak::link::ObsPullResponse next;
  obs.pull(255, next, 0);
  ASSERT_EQ(1u, next.count);
  EXPECT_EQ(1001u, next.records[0].adv_ts);
}

// ----------------------------------------------------------------------------- the RSSI notes

TEST(TheRssiNotes, APooledPacketReusedAndANoteTakenBackOnce) {
  RssiNotes notes;
  // The addresses stand for Packets; nothing here dereferences them, which is what makes the
  // table portable (src/meshclient/RssiNotes.h).
  uint8_t pool[kRssiNotes + 1] = {0};
  EXPECT_EQ(0u, notes.held());

  notes.note(&pool[0], -97);
  notes.note(&pool[1], -84);
  EXPECT_EQ(2u, notes.held());

  int16_t rssi = 0;
  EXPECT_TRUE(notes.take(&pool[1], rssi));
  EXPECT_EQ(-84, rssi);
  // Once: the pool will hand that Packet out again, and the next frame's note is the next
  // frame's.
  rssi = 0;
  EXPECT_FALSE(notes.take(&pool[1], rssi));
  EXPECT_EQ(0, rssi) << "and the caller's value is untouched";
  EXPECT_EQ(1u, notes.held());

  // A pooled packet reused: the same address noted again is the new frame's number, and the old
  // note is not left behind as a second entry.
  notes.note(&pool[0], -60);
  EXPECT_EQ(1u, notes.held());
  EXPECT_TRUE(notes.take(&pool[0], rssi));
  EXPECT_EQ(-60, rssi);
  EXPECT_EQ(0u, notes.held());

  // A packet nobody noted has no note, and nullptr is not a packet.
  EXPECT_FALSE(notes.take(&pool[5], rssi));
  notes.note(nullptr, -1);
  EXPECT_EQ(0u, notes.held());
  EXPECT_FALSE(notes.take(nullptr, rssi));

  // The table is as long as the packet pool. One frame more than that pushes out the oldest note,
  // which is what makes a record say "not known" rather than say a wrong number.
  for (uint8_t i = 0; i < kRssiNotes; i++) notes.note(&pool[i], int16_t(-100 + i));
  EXPECT_EQ(kRssiNotes, notes.held());
  notes.note(&pool[kRssiNotes], -50);
  EXPECT_EQ(kRssiNotes, notes.held());
  EXPECT_FALSE(notes.take(&pool[0], rssi)) << "the oldest note was the one pushed out";
  EXPECT_TRUE(notes.take(&pool[kRssiNotes], rssi));
  EXPECT_EQ(-50, rssi);
  EXPECT_TRUE(notes.take(&pool[1], rssi));
  EXPECT_EQ(-99, rssi);
}

// ----------------------------------------------------------------------------- reason 5

TEST(TheWakeThreshold, OffByDefaultAndOnWithObsWake) {
  ObservationRing ring;
  LinkObservations obs(ring);
  EXPECT_FALSE(obs.wakeEnabled()) << "obs.wake is off at boot (section 8, reason 5)";
  EXPECT_EQ(48u, kObsWakeThreshold) << "three quarters of 64";

  // Off: the ring fills right up and nothing is ever owed. "Collect them when the Walter is awake
  // anyway" is the default on purpose - an attention costs the battery an LTE attach.
  for (uint8_t i = 0; i < kObsCapacity; i++) {
    EXPECT_FALSE(obs.noteAdvert(advertOf(i, 1000u + i, 0), true)) << "advert " << int(i);
  }
  EXPECT_TRUE(ring.full());

  // On, with the ring already past the threshold: the next advert asks, once.
  obs.setWakeEnabled(true);
  EXPECT_TRUE(obs.noteAdvert(advertOf(1, 2000, 0), true));
  EXPECT_EQ(kObsCapacity, obs.wakeWaiting()) << "EV_ATTENTION's detail";
  EXPECT_FALSE(obs.noteAdvert(advertOf(2, 2001, 0), true)) << "and not again for this filling";
}

TEST(TheWakeThreshold, ItIsTheCrossingAndOnlyWhileTheWalterIsBelievedAsleep) {
  ObservationRing ring;
  LinkObservations obs(ring);
  obs.setWakeEnabled(true);

  // Below three quarters: nothing is owed however many arrive.
  for (uint8_t i = 0; i < kObsWakeThreshold - 1; i++) {
    EXPECT_FALSE(obs.noteAdvert(advertOf(i, 1000u + i, 0), true)) << "advert " << int(i);
  }
  EXPECT_EQ(kObsWakeThreshold - 1, obs.pending());

  // The one that reaches it, while the Walter is believed awake: it collects them itself, so
  // nothing is asked - and nothing is spent, so the ask is still owed.
  EXPECT_FALSE(obs.noteAdvert(advertOf(50, 2000, 0), false));
  EXPECT_EQ(kObsWakeThreshold, obs.pending());

  // Then it sleeps: the next advert asks, with the records waiting as the detail.
  EXPECT_TRUE(obs.noteAdvert(advertOf(51, 2001, 0), true));
  EXPECT_EQ(kObsWakeThreshold + 1, obs.wakeWaiting());
  // Once per filling, whatever else arrives.
  for (uint8_t i = 0; i < 5; i++) {
    EXPECT_FALSE(obs.noteAdvert(advertOf(i, 3000u + i, 0), true));
  }

  // A pull that takes it back below the threshold arms it again.
  twinoak::link::ObsPullResponse out;
  while (obs.pending() >= kObsWakeThreshold) obs.pull(kObsPullMaxRecords, out, 0);
  EXPECT_LT(obs.pending(), kObsWakeThreshold);
  while (obs.pending() + 1 < kObsWakeThreshold) {
    EXPECT_FALSE(obs.noteAdvert(advertOf(7, 4000, 0), true));
  }
  EXPECT_TRUE(obs.noteAdvert(advertOf(8, 4001, 0), true)) << "a fresh filling, a fresh ask";
}


// ----------------------------------------------------------------------------- the send ceiling

// The ceiling is a clock and a ring of six timestamps, so every test of it is "sends at these
// millisecond values" (src/meshclient/SendCeiling.h). The constants are the firmware's own, read
// from the header rather than written out, so that a build which moves one moves these with it.

TEST(TheCeiling, SixInAMinuteAndTheSeventhIsRefused) {
  SendCeiling c;
  uint32_t now = 100000;
  // Six sends, each as early as the gap allows: that is the whole minute's allowance spent in
  // twelve seconds, which is what a Walter in a loop would do.
  for (uint8_t i = 0; i < kCeilingSends; i++) {
    ASSERT_TRUE(c.admits(now)) << "send " << int(i) << " at " << now;
    c.noteSent(now);
    EXPECT_EQ(int(i) + 1, int(c.inWindow(now)));
    now += kCeilingGapMs;
  }
  EXPECT_FALSE(c.admits(now)) << "the seventh inside the window";
  EXPECT_EQ(0u, c.refused()) << "admits() judges; the client is what counts a refusal";

  // And it stays refused for the rest of the window, however long the gap has been.
  EXPECT_FALSE(c.admits(100000 + kCeilingWindowMs - 1));
  EXPECT_EQ(int(kCeilingSends), int(c.inWindow(100000 + kCeilingWindowMs - 1)));
}

TEST(TheCeiling, TheTwoSecondGap) {
  SendCeiling c;
  const uint32_t t = 5000;
  EXPECT_TRUE(c.admits(t)) << "the first send of a boot is never refused";
  c.noteSent(t);
  EXPECT_FALSE(c.admits(t)) << "the same millisecond";
  EXPECT_FALSE(c.admits(t + 1));
  EXPECT_FALSE(c.admits(t + kCeilingGapMs - 1)) << "a millisecond short of the gap";
  EXPECT_TRUE(c.admits(t + kCeilingGapMs)) << "the gap, exactly";

  // The gap is measured from the last send and not from the first: a send at the gap restarts it.
  c.noteSent(t + kCeilingGapMs);
  EXPECT_FALSE(c.admits(t + kCeilingGapMs + 1));
  EXPECT_TRUE(c.admits(t + 2 * kCeilingGapMs));
}

TEST(TheCeiling, TheWindowSlides) {
  SendCeiling c;
  const uint32_t first = 1000;
  uint32_t now = first;
  for (uint8_t i = 0; i < kCeilingSends; i++) {
    c.noteSent(now);
    now += kCeilingGapMs;
  }
  // At the last moment of the oldest send's window the ceiling is still reached; a window after
  // that send it has left, and exactly one more is allowed.
  EXPECT_FALSE(c.admits(first + kCeilingWindowMs - 1));
  EXPECT_TRUE(c.admits(first + kCeilingWindowMs));
  EXPECT_EQ(int(kCeilingSends) - 1, int(c.inWindow(first + kCeilingWindowMs)));

  // And because the other five leave in the same two-second steps the burst re-forms - which is
  // what "at most six in **any** sixty seconds" means, rather than "six per fixed minute": six
  // more go, and then the seventh is refused again.
  uint32_t t = first + kCeilingWindowMs;
  for (uint8_t i = 0; i < kCeilingSends; i++) {
    ASSERT_TRUE(c.admits(t)) << "the re-formed burst, send " << int(i) << " at " << t;
    c.noteSent(t);
    t += kCeilingGapMs;
  }
  EXPECT_FALSE(c.admits(t)) << "six of the new ones are inside the window now";
  EXPECT_EQ(int(kCeilingSends), int(c.inWindow(t)));

  // A minute of quiet is a clean sheet: the ring holds six times and none of them is inside the
  // window any more.
  EXPECT_EQ(0, int(c.inWindow(t + kCeilingWindowMs)));
  EXPECT_TRUE(c.admits(t + kCeilingWindowMs));
}

TEST(TheCeiling, ARefusedSendCostsNothing) {
  // A refusal is not a send: it does not enter the window and it does not restart the gap, so a
  // Walter that hammers the link does not push its own next legitimate send further away.
  SendCeiling c;
  uint32_t now = 0;
  for (uint8_t i = 0; i < kCeilingSends; i++) {
    c.noteSent(now);
    now += kCeilingGapMs;
  }
  const int before = int(c.inWindow(now));
  for (int i = 0; i < 1000; i++) {
    EXPECT_FALSE(c.admits(now));
    c.noteRefused();
  }
  EXPECT_EQ(before, int(c.inWindow(now))) << "a thousand refusals changed the window not at all";
  EXPECT_EQ(1000u, c.refused());

  // And the window opens exactly when it would have without them.
  EXPECT_TRUE(c.admits(kCeilingWindowMs));
}

TEST(TheCeiling, TheCountSaturatesAndTheClockMayWrap) {
  SendCeiling c;
  for (int i = 0; i < 5; i++) c.noteRefused();
  EXPECT_EQ(5u, c.refused());

  // millis() wraps every 49 days and a repeater is meant to run for months: a send just before
  // the wrap and one just after it are two seconds apart, and the ceiling must say so.
  SendCeiling w;
  const uint32_t before_wrap = 0xFFFFFFFFu - 500;
  EXPECT_TRUE(w.admits(before_wrap));
  w.noteSent(before_wrap);
  EXPECT_FALSE(w.admits(before_wrap + 1000)) << "500 ms past the wrap is inside the gap";
  EXPECT_TRUE(w.admits(before_wrap + kCeilingGapMs));
  EXPECT_EQ(1, int(w.inWindow(before_wrap + kCeilingGapMs)));
  EXPECT_EQ(0, int(w.inWindow(before_wrap + kCeilingWindowMs)));
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
