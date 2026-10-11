// src/meshclient/: the mesh client's portable state (spec/LINK-PROTOCOL.md section 6.1,
// docs/ARCHITECTURE.md sections 2 and 7) - PeerSlot's index arithmetic, exhaustively, and the
// Job's path, its awaited reply, its silence rule and the wipe of its shared secret.
//
// The peer overrides of TwinOakMesh are not here: they need MeshCore, which a host build does
// not have. What is here is the two pieces those overrides are made of - the arithmetic
// (PeerSlot) and the state (Job) - each of which this task keeps separate for exactly that
// reason. The four requests through a real Session are in test/test_link_session.
//
// What is checked, in the order tasks/phase-2/01-the-job-and-its-peer.md asks for it:
//   - PeerSlot for every upstream count 0..4 and extra yes/no: the total, which index is ours,
//     the shift;
//   - the Job's three path modes; hash sizes 1 to 3; 63 hops; 64 bytes; one byte too many; a hop
//     count that disagrees with the bytes; the encoding byte for each;
//   - the 120-second silence rule against a fake clock: silence ends the job, any request
//     restarts the count;
//   - the wipe: after end() the buffer that held the secret is zero (read back, as the agent
//     firmware's credential test does).

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "meshclient/Job.h"
#include "meshclient/PeerSlot.h"

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

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
