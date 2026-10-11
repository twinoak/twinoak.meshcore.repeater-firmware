#include "ObservationRing.h"

#include <string.h>

namespace twinoak {
namespace meshclient {

bool observationIsWellFormed(const HeardAdvert& a) {
  if (obsPathByteCount(a.path_len) > kObsPathBytesMax) return false;
  if (a.app_len > kObsAppDataMax) return false;
  return true;
}

ObservationRing::ObservationRing()
    : first_(0),
      size_(0),
      adverts_(0),
      adverts_dropped_(0),
      dropped_(0),
      dropped_since_pull_(0) {}

bool ObservationRing::push(const HeardAdvert& advert) {
  // Every call is an advert whose signature upstream checked, which is section 6.4's "adverts
  // that verified" - counted before anything can refuse it, so the two counts add up to what the
  // radio heard.
  adverts_++;
  if (!observationIsWellFormed(advert)) {
    adverts_dropped_++;
    return false;
  }
  if (size_ == kObsCapacity) {
    // Full: the oldest goes, and the loss is counted twice - since boot, and since the last pull
    // (section 7's `dropped`). Both saturate rather than wrap, because a count that wrapped
    // would read as "nothing was lost".
    first_ = uint8_t((first_ + 1) % kObsCapacity);
    size_--;
    if (dropped_ != 0xFFFFFFFFu) dropped_++;
    if (dropped_since_pull_ != 0xFFFFu) dropped_since_pull_++;
  }
  items_[(first_ + size_) % kObsCapacity] = advert;
  size_++;
  return true;
}

const HeardAdvert& ObservationRing::at(uint8_t index) const {
  if (index >= size_) index = 0;
  return items_[(first_ + index) % kObsCapacity];
}

void ObservationRing::consume(uint8_t count) {
  if (count > size_) count = size_;
  first_ = uint8_t((first_ + count) % kObsCapacity);
  size_ = uint8_t(size_ - count);
}

void ObservationRing::clear() {
  first_ = 0;
  size_ = 0;
}

uint16_t ObservationRing::droppedSinceBootSaturating() const {
  return dropped_ > 0xFFFFu ? uint16_t(0xFFFFu) : uint16_t(dropped_);
}

}  // namespace meshclient
}  // namespace twinoak
