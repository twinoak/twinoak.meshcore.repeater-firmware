// The observation ring as the box link's ObservationAccess: OBS_PULL (spec/LINK-PROTOCOL.md
// section 7), STATUS's two observation fields (5.3), MESH_COUNTERS' three (6.4), and attention
// reason 5 (section 8).
//
// Three things are decided here and nowhere else, which is why this file is not twenty lines
// like src/clock/LinkClock.h:
//
//   - **how many records fit one payload.** "The RAK returns as many as fit in one payload and no
//     more than max" (section 7). A record is 45 fixed bytes plus its path and its app_data, so
//     between one and five of them fit the 235 bytes a response has after its four-byte header -
//     and which it is depends on the records themselves. The fitter below walks them oldest
//     first, adding a record only while it fits, so a ring whose oldest record is the longest
//     section 7 allows hands out exactly one.
//   - **what a pull costs the ring.** The records handed out leave it, and `dropped` - "lost
//     since the last OBS_PULL" - is read and then reset. A link-level retry never gets here: the
//     session replays its remembered response (section 3), which is what makes a retry give the
//     same records and drain the ring once.
//   - **when the Walter is woken for adverts** (section 8, reason 5). A unit on a busy mesh must
//     not wake its box every hour for adverts alone, so the threshold is a setting - `obs.wake`,
//     **off by default**, and the default therefore means "collect them when the Walter is awake
//     anyway". With it on, reason 5 is owed the first time the ring sits at three quarters full
//     while the Walter is believed asleep, and again only after a pull has taken it back below.
//     The hourly limit on top of that is the attention machine's (src/link/attention/).
//
// Host-portable: no Arduino, no MeshCore. The raise itself is the caller's - src/link/BoardLink.cpp
// has the machine and the millisecond clock - so test/test_meshclient can hold the threshold to
// its rules without either.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../link/codec/Messages.h"
#include "../link/codec/Numbers.h"
#include "../link/session/Interfaces.h"
#include "ObservationRing.h"

namespace twinoak {
namespace meshclient {

// The ring's numbers are the link's own, by value and not by accident.
static_assert(kObsAppDataMax == link::kObsAppDataMax, "section 7's app_len cap, in one place");
static_assert(kObsRssiUnknown == link::kRssiUnknown, "the unknown RSSI, in one place");
static_assert(kObsPubKeyLen == link::kPubKeyLen, "a record's key is the link's pubkey");
static_assert(kObsCapacity >= 32, "section 7: a ring of at least 32 records");
static_assert(kObsCapacity <= 255, "STATUS's obs_pending is a u8");

// An OBS_PULL response before its records: status, remaining, dropped, count (section 7).
// test/test_link_codec holds encodeObsPullResponse() to exactly this many bytes for an empty
// answer, so the fitter below cannot drift from the encoder.
constexpr size_t kObsPullHeaderBytes = 5;
// One record's fixed part: pubkey 32, adv_ts 4, age_s 4, snr_q4 1, rssi 2, path_len 1, app_len 1.
constexpr size_t kObsRecordFixedBytes = 45;
// What is left of a payload for records, and therefore the most records one answer can hold -
// five, when every one of them is as short as a record gets.
constexpr size_t kObsPullRoom = size_t(link::kMaxPayload) - kObsPullHeaderBytes;
constexpr uint8_t kObsPullMaxRecords = uint8_t(kObsPullRoom / kObsRecordFixedBytes);
static_assert(kObsPullMaxRecords >= 1, "one record of any legal size must fit a payload");
static_assert(kObsRecordFixedBytes + kObsPathBytesMax + kObsAppDataMax <= kObsPullRoom,
              "the longest record section 7 allows must fit one payload");

class LinkObservations : public link::ObservationAccess {
 public:
  explicit LinkObservations(ObservationRing& ring) : ring_(ring) {}

  // --- ObservationAccess ------------------------------------------------------------------

  uint8_t pending() const override { return ring_.size(); }
  uint16_t droppedSinceBoot() const override { return ring_.droppedSinceBootSaturating(); }
  uint32_t adverts() const override { return ring_.adverts(); }
  uint32_t advertsDropped() const override { return ring_.advertsDropped(); }
  uint32_t observationsDropped() const override { return ring_.observationsDropped(); }

  void pull(uint8_t max, link::ObsPullResponse& out, uint32_t now_ms) override;

  // --- the board's side -------------------------------------------------------------------

  // One advert heard, after its signature verified (src/app/TwinOakMesh.cpp fills the record from
  // the packet). True when section 8's **reason 5** is now owed: `obs.wake` is on, the Walter is
  // believed asleep, and the ring stands at three quarters full with nothing asked since it last
  // was below that. The caller raises it, with wakeWaiting() as the event's `detail`, so that the
  // hourly limit stays the attention machine's business.
  bool noteAdvert(const HeardAdvert& advert, bool walter_asleep);

  // EV_ATTENTION's `detail` for reason 5: "records waiting" (section 8).
  uint16_t wakeWaiting() const { return ring_.size(); }

  // The `obs.wake` setting (src/app/Settings.h), off at boot.
  bool wakeEnabled() const { return wake_; }
  void setWakeEnabled(bool on) { wake_ = on; }

  ObservationRing& ring() { return ring_; }
  const ObservationRing& ring() const { return ring_; }

 private:
  ObservationRing& ring_;
  // The records of the answer being built, as views into the ring's own storage. They are handed
  // to the encoder inside the same handler call that filled them, before anything can push a new
  // advert over the slots they point at (the mesh's loop and the link's are the same loop).
  link::Observation staged_[kObsPullMaxRecords];
  bool wake_ = false;   // obs.wake
  bool armed_ = true;   // whether reason 5 is still owed for this filling of the ring
};

}  // namespace meshclient
}  // namespace twinoak
