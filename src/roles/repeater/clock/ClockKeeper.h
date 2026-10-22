// The clock keeper: a repeater whose clock never runs backwards, on a board with no RTC
// (spec/LINK-PROTOCOL.md section 5.4, docs/ARCHITECTURE.md section 5).
//
// Why any of this exists: MeshCore judges replays by timestamps. Upstream's volatile clock starts
// every boot at a fixed date in 2024 (VolatileRTCClock's constructor,
// src/helpers/ArduinoHelpers.h at the pin 81dbae59, base_time = 1715770351), so a repeater that
// restarts and carries on would send adverts and replies that everyone who heard its earlier ones
// drops. The Walter sets the time after every restart - and this firmware has to behave before it
// does, and when there is no Walter at all.
//
// Host-portable: no Arduino, no MeshCore, no heap, no recursion, no floating point. Everything it
// knows about the board arrives through the four ports below, which is what lets
// test/test_clock drive the whole of it against fakes. src/roles/repeater/clock/BoardClock.cpp is the board's
// half; src/roles/repeater/clock/LinkClock.h is the box link's.
//
// The rules, in one place:
//
//   - **At boot**, in this order: a hardware RTC that answers with a sane time wins (state `rtc`);
//     otherwise RAM that survived the reset (the clock simply continues, state `restored`);
//     otherwise the file's value plus the save interval (state `restored`) - the node cannot know
//     how long it was down, and being an hour ahead is harmless where being a second behind its
//     own last advert is not; otherwise upstream's default (state `unset`).
//   - **The clock goes forward.** A set from the link is always allowed forward; backwards only
//     with *force*, or while the state is `unset` or `restored` and no timestamp has left this
//     node since it booted. The advert hold is what makes that second case the normal one.
//   - **The boot advert waits** until the state is `set` or `rtc`, or 60 seconds pass.
//   - **The file is rewritten at most once an hour**, and on a restart the firmware itself asks
//     for. The flash-wear arithmetic that chose the hour is above kSaveIntervalSeconds below.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace twinoak {
namespace clock {

// ----------------------------------------------------------------------------- the numbers

// LINK-PROTOCOL.md section 5.4's time_state, with the wire's own values. src/roles/repeater/clock/LinkClock.h
// static_asserts that these are the link's kTime* constants, so the two cannot drift.
enum State : uint8_t {
  kUnset = 0,     // the firmware's built-in default; never right
  kRestored = 1,  // carried over from before the reset
  kSet = 2,       // set over the box link (or at the console) since boot
  kRtc = 3,       // read from a hardware RTC
};

// How often the file may be rewritten, and therefore how far ahead of the truth a clock restored
// from it may be.
//
// **The flash-wear arithmetic.** The file system is littlefs over seven 4096-byte pages of the
// nRF52840's own flash at 0xED000 (the Adafruit core's InternalFileSystem.cpp: LFS_FLASH_ADDR,
// LFS_FLASH_TOTAL_SIZE = 7 * FLASH_NRF52_PAGE_SIZE, LFS_BLOCK_SIZE 128), and the part's
// endurance is 10 000 erase cycles per page (nRF52840 product specification, NVMC). A file this
// small lives inside a littlefs metadata pair, and a write to it erases at most one 4096-byte
// page - the core's flash layer erases the page a 128-byte block sits in when the bytes are not
// already blank (cores/nRF5/flash/flash_nrf5x.c, fal_erase). So the honest upper bound is one
// page erase per save:
//
//     once an hour     24 saves a day    8 760 a year    10 000 cycles reached in ~14 months
//     once a minute     1 440 a day    525 600 a year                      ...in ~7 days
//     once a day            1 a day        365 a year                      ...in ~27 years
//
// That bound is worst-case twice over (littlefs appends several commits into a block before it
// has to erase one, and the saves of a running node are spread over the pages the pair compacts
// into), but an hour is the shortest interval whose *worst* case is years rather than weeks,
// which is why it is an hour and not a minute. A unit that is up for ten years and never reset
// writes ~87 600 times, so the reviewer may yet want this longer: the only cost of a longer
// interval is that a clock restored after a power cut is further ahead of the truth, and the
// Walter corrects it within seconds of EV_BOOT anyway (tasks/phase-1/reports/04.md).
constexpr uint32_t kSaveIntervalSeconds = 3600;

// How long the boot advert waits for a clock (LINK-PROTOCOL.md section 5.4: "a RAK with
// TIME_PERSIST holds its boot advert back until its clock is set or a timeout passes"). The
// Walter normally sets the clock within a second or two of EV_BOOT.
constexpr uint32_t kAdvertHoldMs = 60000;

// No clock earlier than this is believed, from an RTC or from the link: this firmware did not
// exist then, so such an answer is a part that lost its backup, an unset RTC, or a mistake.
// 1767225600 is 2026-01-01 00:00:00 UTC. It is also what makes upstream's 2024 default fail the
// test, which is how this code tells "an RTC answered" from "upstream's volatile clock answered"
// on a board where they are the same object (BoardClock.cpp says why).
constexpr uint32_t kSaneEpochFloor = 1767225600;
// 4102444800 is 2100-01-01 00:00:00 UTC. A u32 epoch runs out in 2106.
constexpr uint32_t kSaneEpochCeiling = 4102444800u;

// A second-by-second advance is not a jump: the clock may be read a moment after the millisecond
// clock, and a hardware RTC rounds. More than this much more than the elapsed time is somebody
// setting the clock behind our back - the console's `time` or `clock sync` (CommonCLI.cpp at the
// pin), which is what the keeper polls for rather than patching upstream's CLI.
constexpr uint32_t kJumpSlackSeconds = 2;

inline bool isSaneEpoch(uint32_t epoch) {
  return epoch >= kSaneEpochFloor && epoch < kSaneEpochCeiling;
}

// ----------------------------------------------------------------------------- the two records

// What the file holds. `clean` means it was written for a restart this firmware itself asked for,
// so its value is that restart's own second and needs no margin - true only for the first boot
// after it, which the keeper enforces by clearing the flag as soon as it has used it.
struct SavedTime {
  SavedTime() : epoch(0), clean(false) {}
  uint32_t epoch;
  bool clean;
};

// The file's bytes: a magic, the value, the flag and a checksum, so that a half-written or
// foreign file is refused rather than believed. littlefs checksums its own blocks; this is
// against a file that is ours in name only.
constexpr size_t kSavedTimeBytes = 10;
void encodeSavedTime(const SavedTime& in, uint8_t out[kSavedTimeBytes]);
bool decodeSavedTime(const uint8_t* data, size_t len, SavedTime& out);

// The same for the value kept in memory that survives a reset: a magic, the value and a checksum,
// so that the garbage RAM holds after a power cut is not taken for a time. No board of this
// family has a place to put it yet (tasks/phase-1/reports/04.md says where it was looked for and
// why nothing is safe); the record and its rules are here because the seam is, and the day a
// place is found - or a bump of the core's linker script makes one - this is the only piece that
// has to be given a home.
constexpr size_t kRetainedBytes = 10;
void encodeRetained(uint32_t epoch, uint8_t out[kRetainedBytes]);
bool decodeRetained(const uint8_t* data, size_t len, uint32_t& epoch);

// ----------------------------------------------------------------------------- the ports

// Upstream's clock, which is the one the mesh uses: mesh::RTCClock through
// AutoDiscoverRTCClock (src/common/board/target.h). The keeper sets it and reads it and touches nothing
// else of upstream's.
struct UpstreamClock {
  virtual ~UpstreamClock() {}
  virtual uint32_t now() const = 0;
  virtual void set(uint32_t epoch) = 0;
};

// A hardware real-time clock, if the board can ask one. `read` returns false when there is
// nothing to ask; the keeper decides whether the answer is sane.
struct HardwareRtc {
  virtual ~HardwareRtc() {}
  virtual bool read(uint32_t& epoch) = 0;
};

// Memory the C runtime does not initialise: valid after a watchdog, pin or software reset, gone
// after a power cut. A null port is a board with no such place.
struct RetainedMemory {
  virtual ~RetainedMemory() {}
  virtual bool load(uint32_t& epoch) = 0;
  virtual void save(uint32_t epoch) = 0;
};

// The file in the node's file system. `save` returns false when the write failed.
struct ClockFile {
  virtual ~ClockFile() {}
  virtual bool load(SavedTime& out) = 0;
  virtual bool save(const SavedTime& in) = 0;
};

// Only `upstream` is required; every other null pointer is a memory this board does not have.
struct ClockPorts {
  ClockPorts() : upstream(nullptr), rtc(nullptr), retained(nullptr), file(nullptr) {}
  UpstreamClock* upstream;
  HardwareRtc* rtc;
  RetainedMemory* retained;
  ClockFile* file;
};

// ----------------------------------------------------------------------------- the keeper

class ClockKeeper {
 public:
  explicit ClockKeeper(const ClockPorts& ports) : p_(ports) {}

  // Decides what the time is and sets upstream's clock to it, and starts the advert hold.
  // `software_reset` is true when the board's reset reason says this boot followed a reset the
  // firmware itself asked for (LINK-PROTOCOL.md section 5.3's reason 4): only then is a `clean`
  // file believed without the margin. Called from setup(), after the clock helper has probed the
  // I2C bus and before anything of upstream's can put a timestamp on the air.
  void begin(uint32_t now_ms, bool software_reset);

  // Once per loop(): notices a clock somebody else set, keeps the file within the save interval,
  // and lifts the advert hold.
  void loop(uint32_t now_ms);

  // --- what the link and the console read ---------------------------------------------------

  uint32_t epoch() const { return p_.upstream->now(); }
  uint8_t timeState() const { return state_; }
  // STATUS flag bit 3 (section 5.3).
  bool advertsHeld() const { return hold_; }

  // --- TIME_SET (section 5.4) ---------------------------------------------------------------

  // Moves the clock, or refuses. prev_epoch is what the clock said either way; false is the
  // protocol's ERR_RANGE.
  bool setTime(uint32_t epoch, bool force, uint32_t& prev_epoch);

  // --- the boot advert ------------------------------------------------------------------------

  // True once the hold has lifted and the boot advert has not been sent. src/roles/repeater/main.cpp sends it
  // and says so, because ours is the main.cpp that decides when (ARCHITECTURE section 5).
  bool bootAdvertDue() const { return !hold_ && !boot_advert_sent_; }
  void noteBootAdvertSent() {
    boot_advert_sent_ = true;
    noteTimestampEmitted();
  }

  // --- what makes a backwards set unsafe ------------------------------------------------------

  // Something that carries this node's clock has gone on the air (or may have: a packet
  // addressed to us that upstream answers). src/roles/repeater/app/TwinOakMesh.cpp says which places.
  void noteTimestampEmitted() { emitted_ = true; }
  bool timestampEmitted() const { return emitted_; }

  // --- a restart this firmware asks for -------------------------------------------------------

  // Writes both memories now, the file with `clean` set, whatever the save interval says: the
  // value is this second's and the next boot can use it without a margin. Called from
  // TwinOakBoard::reboot()/powerOff() and from the box link's restart, so that every restart the
  // firmware itself performs goes through it.
  void saveBeforeRestart();

  // --- what the tests and the report count ----------------------------------------------------

  uint32_t fileWrites() const { return file_writes_; }
  uint32_t retainedWrites() const { return retained_writes_; }
  // What the file holds as far as this keeper knows, for `clock.state` and the tests.
  uint32_t savedEpoch() const { return file_epoch_; }

 private:
  // Writes the file, unless `rate_limited` and the value it holds is still within the save
  // interval of `epoch`.
  void saveFile(uint32_t epoch, bool clean, bool rate_limited);
  void saveRetained(uint32_t epoch);
  // Both memories, after the clock moved.
  void saveBoth(uint32_t epoch, bool force_file);

  const ClockPorts p_;

  uint8_t state_ = kUnset;
  bool emitted_ = false;

  // True until begin() has decided otherwise, so a keeper that has not begun holds the advert
  // rather than letting one out with no clock at all.
  bool hold_ = true;
  bool boot_advert_sent_ = false;
  uint32_t hold_until_ms_ = 0;

  // The baseline of the jump poll: what the clock said, and when, the last time we looked.
  uint32_t poll_ms_ = 0;
  uint32_t poll_epoch_ = 0;

  // What the file holds, and whether anything is known about it at all.
  bool file_known_ = false;
  uint32_t file_epoch_ = 0;

  uint32_t file_writes_ = 0;
  uint32_t retained_writes_ = 0;
};

}  // namespace clock
}  // namespace twinoak
