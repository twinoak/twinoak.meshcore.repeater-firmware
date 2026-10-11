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
//     (the first wins); the timeout itself; and what a CLI reply's plaintext must look like.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "meshclient/Job.h"
#include "meshclient/PeerSlot.h"
#include "meshclient/Reply.h"

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

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
