#include "roles/repeater/meshclient/LinkObservations.h"

namespace twinoak {
namespace meshclient {

void LinkObservations::pull(uint8_t max, link::ObsPullResponse& out, uint32_t now_ms) {
  // Oldest first, and only while the next record still fits the payload (section 7). The length
  // is computed with the codec's own obsPathBytes(), which is the same function
  // encodeObsPullResponse() checks each record's path against - so a record this loop admits is
  // one the encoder will take.
  uint8_t count = 0;
  size_t room = kObsPullRoom;
  while (count < max && count < kObsPullMaxRecords && count < ring_.size()) {
    const HeardAdvert& a = ring_.at(count);
    const uint8_t path_bytes = link::obsPathBytes(a.path_len);
    const size_t bytes = kObsRecordFixedBytes + path_bytes + a.app_len;
    if (bytes > room) break;
    room -= bytes;

    link::Observation& o = staged_[count];
    o.pubkey = a.pubkey;
    o.adv_ts = a.adv_ts;
    // Measured, not read off the clock: a TIME_SET between hearing this advert and handing it
    // over must not change its age (section 7).
    o.age_s = ageSeconds(a, now_ms);
    o.snr_q4 = a.snr_q4;
    o.rssi = a.rssi;
    o.path_len = a.path_len;
    o.path = link::bytesOf(a.path, path_bytes);
    // The advert's application data, untouched - flags, position, name. Copied, never parsed.
    o.app_data = link::bytesOf(a.app_data, a.app_len);
    count++;
  }

  out.records = staged_;
  out.count = count;
  // "dropped: lost since the last OBS_PULL" - read here and reset here, so the next pull reports
  // only what this one did not. A pull that asked for nothing is still a pull and still clears
  // it; a retry of this request does not reach this function at all (section 3).
  out.dropped = ring_.droppedSincePull();
  ring_.notePulled();
  // The records handed out leave the ring.
  ring_.consume(count);
  out.remaining = ring_.size();

  // Room again: the next time the ring fills, reason 5 is owed afresh.
  if (ring_.size() < kObsWakeThreshold) armed_ = true;
}

bool LinkObservations::noteAdvert(const HeardAdvert& advert, bool walter_asleep) {
  ring_.push(advert);  // a record the ring refuses is counted as an advert dropped, and that is all
  if (ring_.size() < kObsWakeThreshold) {
    // Below three quarters: nothing is owed, and whatever happens next is a fresh filling.
    armed_ = true;
    return false;
  }
  // At or past it. Only while the Walter is believed asleep - an awake one collects them itself
  // and an attention costs the battery an LTE attach - and only once per filling.
  if (!wake_ || !walter_asleep || !armed_) return false;
  armed_ = false;
  return true;
}

}  // namespace meshclient
}  // namespace twinoak
