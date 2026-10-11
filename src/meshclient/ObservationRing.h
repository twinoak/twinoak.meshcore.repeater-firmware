// The observation ring: every advert this node heard, kept for the Walter to collect
// (spec/LINK-PROTOCOL.md section 7; docs/ARCHITECTURE.md section 7).
//
// A port of src/agent/Observations.{h,cpp} of twinoak/meshmaster-agent-firmware at
// 51faafb427cba41b0de9983736b6621bc155f829 - ObservationRing's push / at / consume / dropped, and
// HeardAdvert - with one difference that is the whole of why this repeater's ring is not that
// one's:
//
//   **the record is section 7's record, and the advert's application data is copied whole and
//   never parsed.** That firmware decodes the advert there and then (AdvertDataParser: the type,
//   the position, the name) because it posts JSON to a server. This one hands the bytes to the
//   Walter and lets the agent on the other box decode them - so nothing here has to know what an
//   advert's app_data means, nothing here can be wrong about it when MeshCore's builder changes,
//   and **the name a stranger chose is never parsed and never printed on this side** (section 7,
//   "Neither end logs it"; spec/SYSTEM.md S-12). test/test_rails holds that last one.
//
// What a record holds and why each field is what it is:
//
//   - **the key**, 32 bytes, the advertising node's;
//   - **adv_ts**, the timestamp inside the advert - the advertiser's clock, not ours;
//   - **heard_ms**, the millisecond clock when it was heard. Section 7's `age_s` is *measured*,
//     "so it is right whatever the clock said": a TIME_SET between hearing an advert and handing
//     it over moves the epoch and must not move the age, so the age is computed from this
//     monotonic stamp at the moment of the pull (ageSeconds below);
//   - **snr_q4**, quarter-decibels, which is how MeshCore keeps a packet's SNR;
//   - **rssi**, whole dBm, or kObsRssiUnknown (0x7FFF) when the note taken in logRx was not
//     there (src/meshclient/RssiNotes.h says why there is a note at all);
//   - **path_len and path**: MeshCore's own encoded byte and the hops as they arrived, up to
//     MAX_PATH_SIZE bytes. Empty for a direct neighbour, which is what makes "hops 0 and an
//     RSSI" the thing to look for on the bench;
//   - **app_data**, up to 32 bytes, untouched.
//
// Full: the **oldest** is overwritten and the loss counted, twice - once since boot (STATUS's
// obs_dropped and MESH_COUNTERS' obs_dropped) and once since the last pull (section 7's
// `dropped`). "The RAK keeps observations in the ring (dropping the oldest), and never retries
// anything on the link" (section 9) is the whole of what it does with nobody listening.
//
// Host-portable: no Arduino, no MeshCore, no heap, no clock of its own. Every method that needs
// the time takes the millisecond clock from its caller. src/meshclient/LinkObservations.h is this
// ring as the box link's ObservationAccess; src/app/TwinOakMesh.cpp is the dozen lines that fill
// a record from a packet.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Job.h"  // pathHashSize / pathHopCount: MeshCore's path_len encoding, in one place

namespace twinoak {
namespace meshclient {

// ----------------------------------------------------------------------------- the numbers

// "A ring of at least 32 records" (section 7); this firmware keeps **64**, as the agent
// firmware's does. The RAM it costs is in tasks/phase-2/reports/03.md.
constexpr uint8_t kObsCapacity = 64;

constexpr uint8_t kObsPubKeyLen = kPeerKeyLen;       // 32, MeshCore's PUB_KEY_SIZE
constexpr uint8_t kObsPathBytesMax = kPathBytesMax;  // 64, MeshCore's MAX_PATH_SIZE
// MeshCore's MAX_ADVERT_DATA_SIZE, which is also section 7's cap on app_len: Mesh::onRecvPacket
// clamps an advert's app_data to it before onAdvertRecv sees it (src/Mesh.cpp:269 at the pin
// d9296435), so on this build the two agree exactly and nothing is ever truncated here.
// src/app/TwinOakMesh.cpp static_asserts it against upstream's macro and against the link's own.
constexpr uint8_t kObsAppDataMax = 32;

// The RSSI a record does not know (section 7: "dBm, or 0x7FFF"). The link's kRssiUnknown.
constexpr int16_t kObsRssiUnknown = 0x7FFF;

// "The observation ring is nearly full" (section 8, reason 5) is three quarters of it.
constexpr uint8_t kObsWakeThreshold = uint8_t(kObsCapacity - kObsCapacity / 4);  // 48 of 64

// How many bytes of `path` a record with this path_len carries: MeshCore's encoding, read with
// the job's own two functions so that the ring and the job cannot disagree about it.
// src/link/codec/Numbers.h's obsPathBytes() is the same arithmetic on the codec's side, and
// test/test_meshclient holds the two together for every one of the 256 values.
inline uint8_t obsPathByteCount(uint8_t path_len) {
  return uint8_t(uint16_t(pathHashSize(path_len)) * pathHopCount(path_len));
}

// ----------------------------------------------------------------------------- a record

// The fields are in descending alignment, so that the only padding the struct has is what its
// own alignment adds at the end.
struct HeardAdvert {
  uint32_t adv_ts = 0;    // the timestamp inside the advert
  uint32_t heard_ms = 0;  // the millisecond clock when it was heard; `age_s` is measured from it
  uint8_t pubkey[kObsPubKeyLen] = {0};
  uint8_t path[kObsPathBytesMax] = {0};
  uint8_t app_data[kObsAppDataMax] = {0};
  int16_t rssi = kObsRssiUnknown;
  int8_t snr_q4 = 0;
  uint8_t path_len = 0;  // MeshCore's encoded byte: (hash_size - 1) << 6 | hop_count
  uint8_t app_len = 0;   // 0 to kObsAppDataMax
};

// Section 7's `age_s`: seconds since this node heard it, measured on the millisecond clock, so
// it is right whatever the epoch clock says and across a millis() wrap.
inline uint32_t ageSeconds(const HeardAdvert& a, uint32_t now_ms) {
  return uint32_t(now_ms - a.heard_ms) / 1000u;
}

// Whether a record is one section 7 can carry: its path bytes fit the record's own storage and
// its app_data is within the cap. On this build every record the board builds passes - the
// static_asserts and MeshCore's own parser see to that - so this is the backstop that makes a pin
// bump which widens either bound a counted drop rather than a bad frame.
bool observationIsWellFormed(const HeardAdvert& a);

// ----------------------------------------------------------------------------- the ring

class ObservationRing {
 public:
  ObservationRing();

  // One advert heard, after its signature verified. True when it was kept; false when the record
  // is not one section 7 can carry, and then nothing is kept and the advert is counted as
  // dropped.
  //
  // When the ring is full the **oldest** is overwritten and the loss counted.
  bool push(const HeardAdvert& advert);

  uint8_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  bool full() const { return size_ == kObsCapacity; }

  // 0 is the oldest, size() - 1 the newest. Out of range is the oldest, as the agent firmware's
  // at() answers it.
  const HeardAdvert& at(uint8_t index) const;

  // Forget the `count` oldest: what a pull handed out. More than there are is all there are.
  void consume(uint8_t count);

  // Everything forgotten, the counters kept: a ring that has been emptied has still heard what
  // it heard.
  void clear();

  // --- the counts (section 6.4) -----------------------------------------------------------

  // Every advert pushed, whether it was kept or not - which is "adverts that verified", because
  // this is only ever called for an advert whose signature upstream checked.
  uint32_t adverts() const { return adverts_; }
  // Adverts push() refused, since boot.
  uint32_t advertsDropped() const { return adverts_dropped_; }
  // Observations lost to a full ring, since boot.
  uint32_t observationsDropped() const { return dropped_; }
  // The same count as STATUS carries it: u16, saturating (section 5.3).
  uint16_t droppedSinceBootSaturating() const;

  // Section 7's `dropped`, "lost since the last OBS_PULL", saturating. notePulled() is what a
  // pull calls when it has read it.
  uint16_t droppedSincePull() const { return dropped_since_pull_; }
  void notePulled() { dropped_since_pull_ = 0; }

 private:
  HeardAdvert items_[kObsCapacity];
  uint8_t first_;
  uint8_t size_;

  uint32_t adverts_;
  uint32_t adverts_dropped_;
  uint32_t dropped_;
  uint16_t dropped_since_pull_;
};

}  // namespace meshclient
}  // namespace twinoak
