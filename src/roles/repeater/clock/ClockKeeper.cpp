#include "roles/repeater/clock/ClockKeeper.h"

namespace twinoak {
namespace clock {
namespace {

// Both records are the same shape: a four-byte magic, a four-byte value, one byte of flags and
// one byte of checksum. The magics differ so that one cannot be read as the other.
const uint32_t kFileMagic = 0x314B4C43u;      // "CLK1", little-endian
const uint32_t kRetainedMagic = 0x3152414Du;  // "MAR1" - memory across a reset, version 1

void putU32(uint8_t* out, uint32_t v) {
  out[0] = uint8_t(v);
  out[1] = uint8_t(v >> 8);
  out[2] = uint8_t(v >> 16);
  out[3] = uint8_t(v >> 24);
}

uint32_t getU32(const uint8_t* in) {
  return uint32_t(in[0]) | (uint32_t(in[1]) << 8) | (uint32_t(in[2]) << 16) |
         (uint32_t(in[3]) << 24);
}

// A byte that is not the sum of the others: a one-byte check that catches a zeroed record, a
// record of all ones, and a single flipped byte, which is what a half-written file or garbage RAM
// looks like.
uint8_t checkByte(const uint8_t* data, size_t len) {
  uint8_t sum = 0x5A;
  for (size_t i = 0; i < len; i++) sum = uint8_t(sum + data[i] + 1);
  return uint8_t(~sum);
}

// magic, value, flags, check - the layout both records share.
void encodeRecord(uint32_t magic, uint32_t value, uint8_t flags, uint8_t* out) {
  putU32(out, magic);
  putU32(out + 4, value);
  out[8] = flags;
  out[9] = checkByte(out, 9);
}

bool decodeRecord(uint32_t magic, const uint8_t* data, size_t len, uint32_t& value,
                  uint8_t& flags) {
  if (data == nullptr || len < 10) return false;
  if (getU32(data) != magic) return false;
  if (data[9] != checkByte(data, 9)) return false;
  value = getU32(data + 4);
  flags = data[8];
  return true;
}

const uint8_t kFlagClean = 0x01;

}  // namespace

// ----------------------------------------------------------------------------- the two records

void encodeSavedTime(const SavedTime& in, uint8_t out[kSavedTimeBytes]) {
  encodeRecord(kFileMagic, in.epoch, in.clean ? kFlagClean : 0, out);
}

bool decodeSavedTime(const uint8_t* data, size_t len, SavedTime& out) {
  uint32_t value = 0;
  uint8_t flags = 0;
  if (!decodeRecord(kFileMagic, data, len, value, flags)) return false;
  // A value that cannot be a time is a record that passed its checksum by accident, or one
  // written by a firmware that had no clock. Refuse it rather than restore 1970.
  if (!isSaneEpoch(value)) return false;
  out.epoch = value;
  out.clean = (flags & kFlagClean) != 0;
  return true;
}

void encodeRetained(uint32_t epoch, uint8_t out[kRetainedBytes]) {
  encodeRecord(kRetainedMagic, epoch, 0, out);
}

bool decodeRetained(const uint8_t* data, size_t len, uint32_t& epoch) {
  uint32_t value = 0;
  uint8_t flags = 0;
  if (!decodeRecord(kRetainedMagic, data, len, value, flags)) return false;
  if (!isSaneEpoch(value)) return false;
  epoch = value;
  return true;
}

// ----------------------------------------------------------------------------- boot

void ClockKeeper::begin(uint32_t now_ms, bool software_reset) {
  uint32_t chosen = 0;
  bool have = false;
  bool used_clean_file = false;

  // 1. A hardware RTC that answers with a sane time wins. It needs no setting: on this board the
  //    RTC *is* upstream's clock (AutoDiscoverRTCClock), so the answer is already the clock's.
  uint32_t from_rtc = 0;
  if (p_.rtc != nullptr && p_.rtc->read(from_rtc) && isSaneEpoch(from_rtc)) {
    state_ = kRtc;
    chosen = from_rtc;
    have = true;
  } else {
    // 2. RAM that survived the reset: the clock simply continues.
    uint32_t from_ram = 0;
    if (p_.retained != nullptr && p_.retained->load(from_ram) && isSaneEpoch(from_ram)) {
      chosen = from_ram;
      state_ = kRestored;
      have = true;
    } else if (p_.file != nullptr) {
      // 3. The file, plus the save interval - except when the file was written for a restart this
      //    firmware asked for and this boot is that restart: then its value is that second's.
      SavedTime saved;
      if (p_.file->load(saved)) {
        file_known_ = true;
        file_epoch_ = saved.epoch;
        used_clean_file = saved.clean && software_reset;
        chosen = used_clean_file ? saved.epoch : saved.epoch + kSaveIntervalSeconds;
        state_ = kRestored;
        have = true;
      }
    }
    // 4. Otherwise upstream's default, which is never right.
    if (have) {
      // The clock is set even when that moves it *down*: the only way to be here with a clock
      // later than what we restored is a hardware RTC whose answer failed isSaneEpoch() - a part
      // that lost its backup and reads a date in the next century - and leaving the node there
      // would poison every timestamp it ever sends. The set writes such an RTC, which repairs it.
      p_.upstream->set(chosen);
      chosen = p_.upstream->now();
    } else {
      state_ = kUnset;
      chosen = p_.upstream->now();
    }
  }

  poll_ms_ = now_ms;
  poll_epoch_ = chosen;

  // The hold: on unless a hardware RTC answered (LINK-PROTOCOL.md section 5.4; a node with an
  // RTC has nothing to wait for).
  hold_ = (state_ != kRtc);
  hold_until_ms_ = now_ms + kAdvertHoldMs;

  // The memory that survives a reset costs nothing to write, so it is written at every boot: a
  // reset a second from now then finds this value rather than nothing.
  saveRetained(chosen);

  // A `clean` file is good for exactly one boot. Spend the flag now, while the value is still
  // this second's, so that a crash an hour from now cannot restore an hour-old time as if it
  // were fresh.
  if (used_clean_file) saveFile(file_epoch_, /*clean=*/false, /*rate_limited=*/false);
}

// ----------------------------------------------------------------------------- the pass

void ClockKeeper::loop(uint32_t now_ms) {
  const uint32_t t = p_.upstream->now();

  // A clock somebody else set: upstream's own console does it (`time <epoch>` and `clock sync`,
  // src/helpers/CommonCLI.cpp:210-238 at the pin 81dbae59, both of which refuse to go backwards
  // themselves). Polling for the jump is what keeps upstream's CLI unpatched. A *backwards* jump
  // - which only `clkreboot` can make, and it reboots straight afterwards - is deliberately not
  // noticed: this keeper's memories are a high-water mark and never come down except for a
  // forced set.
  const uint32_t elapsed_s = (now_ms - poll_ms_) / 1000u;
  if (t > poll_epoch_ + elapsed_s + kJumpSlackSeconds) {
    if (state_ != kRtc) state_ = kSet;
    saveBoth(t, /*force_file=*/false);
  }
  poll_ms_ = now_ms;
  poll_epoch_ = t;

  // The file, by value rather than by wall clock: it is rewritten when what it holds is more
  // than the save interval behind the clock, which is "at most once an hour" however often the
  // time is set.
  if (p_.file != nullptr && (!file_known_ || t > file_epoch_ + kSaveIntervalSeconds)) {
    saveFile(t, /*clean=*/false, /*rate_limited=*/true);
  }

  // The advert hold.
  if (hold_) {
    const bool have_clock = (state_ == kSet || state_ == kRtc);
    const bool timed_out = int32_t(now_ms - hold_until_ms_) >= 0;
    if (have_clock || timed_out) hold_ = false;
  }
}

// ----------------------------------------------------------------------------- TIME_SET

bool ClockKeeper::setTime(uint32_t epoch, bool force, uint32_t& prev_epoch) {
  prev_epoch = p_.upstream->now();

  // Nothing before this firmware existed is a time, whatever force says: a clock set into the
  // past of the project is the one mistake this node could not recover from by itself.
  if (!isSaneEpoch(epoch)) return false;

  const bool forward = epoch >= prev_epoch;
  // Section 5.4: backwards only with force, or while the state is 0 or 1 and nothing carrying a
  // timestamp has left this node since it booted.
  const bool allowed =
      forward || force || ((state_ == kUnset || state_ == kRestored) && !emitted_);
  if (!allowed) return false;

  p_.upstream->set(epoch);
  state_ = kSet;
  // The jump poll must not see our own set as somebody else's.
  poll_epoch_ = p_.upstream->now();

  // Both memories (section 5.4: "A set makes the state 2 and saves both memories"). The file
  // keeps its once-an-hour rule, except when the clock came *down*: the saved lower bound has to
  // come down with it, or the next boot would restore the time this set just corrected.
  saveBoth(epoch, /*force_file=*/epoch < prev_epoch);
  return true;
}

// ----------------------------------------------------------------------------- the two memories

void ClockKeeper::saveBeforeRestart() {
  const uint32_t t = p_.upstream->now();
  saveRetained(t);
  if (isSaneEpoch(t)) saveFile(t, /*clean=*/true, /*rate_limited=*/false);
}

void ClockKeeper::saveBoth(uint32_t epoch, bool force_file) {
  saveRetained(epoch);
  if (p_.file == nullptr) return;
  if (force_file) {
    saveFile(epoch, /*clean=*/false, /*rate_limited=*/false);
  } else if (!file_known_ || epoch > file_epoch_ + kSaveIntervalSeconds) {
    saveFile(epoch, /*clean=*/false, /*rate_limited=*/true);
  }
}

void ClockKeeper::saveRetained(uint32_t epoch) {
  if (p_.retained == nullptr || !isSaneEpoch(epoch)) return;
  p_.retained->save(epoch);
  retained_writes_++;
}

void ClockKeeper::saveFile(uint32_t epoch, bool clean, bool rate_limited) {
  if (p_.file == nullptr || !isSaneEpoch(epoch)) return;
  if (rate_limited && file_known_ && epoch <= file_epoch_ + kSaveIntervalSeconds) return;
  SavedTime out;
  out.epoch = epoch;
  out.clean = clean;
  if (!p_.file->save(out)) return;  // a write that failed changes nothing we believe
  file_known_ = true;
  file_epoch_ = epoch;
  file_writes_++;
}

}  // namespace clock
}  // namespace twinoak
