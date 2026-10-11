// src/clock/: the clock keeper's rules as a pure function of (what the RTC answered, what
// survived in RAM, what the file holds, what the clock says now), against fakes - and the
// TIME_SET and TIME_GET cases of spec/fixtures/link/link-v1.json driven through the real session
// with the real keeper behind it, byte for byte.
//
// What is checked, in the order tasks/phase-1/04-the-clock.md asks for it:
//   - every boot order: a sane RTC, a surviving RAM value, the file plus the save interval, the
//     file written for a restart we asked for, nothing at all - and a corrupt RAM value and a
//     corrupt file, which are each the same as not having one;
//   - the file is written at most once an hour however often the time is set, and always on an
//     asked-for restart;
//   - a forward set; a backward set refused in state `set`; allowed in state `restored` before
//     anything was sent; refused after; allowed with force;
//   - a restored clock is never earlier than the saved one plus the interval;
//   - the hold: released by a set, released by the timeout, never held when an RTC answered;
//   - the two fixture cases through the session, and a TIME_SET refused as ERR_RANGE.

#include <gtest/gtest.h>

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "clock/ClockKeeper.h"
#include "clock/LinkClock.h"
#include "link/codec/Bytes.h"
#include "link/codec/FrameReceiver.h"
#include "link/codec/FrameWriter.h"
#include "link/codec/Messages.h"
#include "link/codec/Numbers.h"
#include "link/session/Interfaces.h"
#include "link/session/Session.h"

#ifndef FIXTURES_DIR
#error "FIXTURES_DIR must be defined; platformio.ini's [env:native] does it"
#endif

namespace {

using namespace twinoak::clock;  // NOLINT: this is the suite of exactly this namespace

// A time inside the sane window, and one an hour and a day later: the suite never uses a bare
// number where it means "a plausible clock".
const uint32_t kNoon = 1791720000;  // 2026-10-11 12:00:00 UTC, the fixture's own TIME_SET value
const uint32_t kHour = 3600;
const uint32_t kDay = 86400;

// ----------------------------------------------------------------------------- the fakes

class FakeUpstream : public UpstreamClock {
 public:
  // Upstream's volatile clock starts here at every boot (VolatileRTCClock, 15 May 2024), which
  // is before kSaneEpochFloor and is therefore never mistaken for a time anybody set.
  uint32_t value = 1715770351;
  int sets = 0;

  uint32_t now() const override { return value; }
  void set(uint32_t epoch) override {
    value = epoch;
    sets++;
  }
  void advance(uint32_t seconds) { value += seconds; }
};

class FakeRtc : public HardwareRtc {
 public:
  bool answers = false;
  uint32_t value = 0;

  bool read(uint32_t& epoch) override {
    if (!answers) return false;
    epoch = value;
    return true;
  }
};

// Ten bytes of memory that may or may not have survived, held through the keeper's own codec so
// that the test of a corrupt value is a test of real code.
class FakeRetained : public RetainedMemory {
 public:
  uint8_t bytes[kRetainedBytes] = {0};
  bool present = false;
  int saves = 0;

  bool load(uint32_t& epoch) override {
    if (!present) return false;
    return decodeRetained(bytes, sizeof(bytes), epoch);
  }
  void save(uint32_t epoch) override {
    encodeRetained(epoch, bytes);
    present = true;
    saves++;
  }
  void hold(uint32_t epoch) {
    save(epoch);
    saves = 0;
  }
  void corrupt() { bytes[5] ^= 0xFF; }
};

class FakeFile : public ClockFile {
 public:
  uint8_t bytes[kSavedTimeBytes] = {0};
  bool present = false;
  bool writable = true;
  int saves = 0;

  bool load(SavedTime& out) override {
    if (!present) return false;
    return decodeSavedTime(bytes, sizeof(bytes), out);
  }
  bool save(const SavedTime& in) override {
    if (!writable) return false;
    encodeSavedTime(in, bytes);
    present = true;
    saves++;
    return true;
  }
  // Puts a record there as a previous boot would have, without counting it as a write.
  void hold(uint32_t epoch, bool clean) {
    SavedTime in;
    in.epoch = epoch;
    in.clean = clean;
    const bool was = writable;
    writable = true;
    save(in);
    writable = was;
    saves = 0;
  }
  uint32_t epochInFile() const {
    SavedTime out;
    return decodeSavedTime(bytes, sizeof(bytes), out) ? out.epoch : 0;
  }
  bool cleanInFile() const {
    SavedTime out;
    return decodeSavedTime(bytes, sizeof(bytes), out) && out.clean;
  }
  void corrupt() { bytes[2] ^= 0xFF; }
};

// A keeper with all four ports and a millisecond clock of its own.
struct Rig {
  Rig() {
    ports.upstream = &upstream;
    ports.rtc = &rtc;
    ports.retained = &retained;
    ports.file = &file;
  }

  // A hardware RTC that answers with that time. On this board the RTC *is* upstream's clock -
  // AutoDiscoverRTCClock::getCurrentTime() returns the part's time and falls back to the
  // volatile clock only when it found no part (src/clock/BoardClock.cpp) - so the two fakes move
  // together, and a test that fits an RTC says so once here.
  void fitRtc(uint32_t epoch) {
    rtc.answers = true;
    rtc.value = epoch;
    upstream.value = epoch;
  }

  // The keeper is constructed late, so that a test can arrange the ports first.
  ClockKeeper& start(bool software_reset = false) {
    keeper.reset(new ClockKeeper(ports));
    keeper->begin(now_ms, software_reset);
    return *keeper;
  }

  // Time passes: both clocks together, as they do on the board.
  void tick(uint32_t seconds) {
    now_ms += seconds * 1000u;
    upstream.advance(seconds);
    keeper->loop(now_ms);
  }
  // Only the millisecond clock, for the hold's timeout.
  void tickMillis(uint32_t ms) {
    now_ms += ms;
    keeper->loop(now_ms);
  }

  FakeUpstream upstream;
  FakeRtc rtc;
  FakeRetained retained;
  FakeFile file;
  ClockPorts ports;
  uint32_t now_ms = 5000;
  std::unique_ptr<ClockKeeper> keeper;
};

}  // namespace

// ----------------------------------------------------------------------------- the two records

TEST(TheRecords, ARoundTripAndEveryWayOfBeingWrong) {
  uint8_t buf[kSavedTimeBytes] = {0};
  SavedTime in;
  in.epoch = kNoon;
  in.clean = true;
  encodeSavedTime(in, buf);

  SavedTime out;
  ASSERT_TRUE(decodeSavedTime(buf, sizeof(buf), out));
  EXPECT_EQ(kNoon, out.epoch);
  EXPECT_TRUE(out.clean);

  // Short, empty, all zeroes, all ones: none of them is a time.
  EXPECT_FALSE(decodeSavedTime(buf, sizeof(buf) - 1, out));
  EXPECT_FALSE(decodeSavedTime(nullptr, sizeof(buf), out));
  uint8_t zeroes[kSavedTimeBytes] = {0};
  EXPECT_FALSE(decodeSavedTime(zeroes, sizeof(zeroes), out));
  uint8_t ones[kSavedTimeBytes];
  memset(ones, 0xFF, sizeof(ones));
  EXPECT_FALSE(decodeSavedTime(ones, sizeof(ones), out));

  // Every single flipped byte is caught by the magic or the check byte.
  for (size_t i = 0; i < sizeof(buf); i++) {
    uint8_t bad[kSavedTimeBytes];
    memcpy(bad, buf, sizeof(bad));
    bad[i] ^= 0x01;
    EXPECT_FALSE(decodeSavedTime(bad, sizeof(bad), out)) << "byte " << i;
  }

  // A record that is ours and passes its checksum but cannot be a time is still refused.
  SavedTime old;
  old.epoch = kSaneEpochFloor - 1;
  encodeSavedTime(old, buf);
  EXPECT_FALSE(decodeSavedTime(buf, sizeof(buf), out));
}

TEST(TheRecords, TheTwoRecordsAreNotEachOther) {
  uint8_t file_bytes[kSavedTimeBytes] = {0};
  SavedTime in;
  in.epoch = kNoon;
  encodeSavedTime(in, file_bytes);

  uint8_t ram_bytes[kRetainedBytes] = {0};
  encodeRetained(kNoon, ram_bytes);

  uint32_t epoch = 0;
  SavedTime out;
  EXPECT_FALSE(decodeRetained(file_bytes, sizeof(file_bytes), epoch));
  EXPECT_FALSE(decodeSavedTime(ram_bytes, sizeof(ram_bytes), out));
  EXPECT_TRUE(decodeRetained(ram_bytes, sizeof(ram_bytes), epoch));
  EXPECT_EQ(kNoon, epoch);
}

// ----------------------------------------------------------------------------- the boot order

TEST(TheBootOrder, AnRtcWithASaneTimeWins) {
  Rig r;
  r.fitRtc(kNoon);
  r.retained.hold(kNoon - kDay);
  r.file.hold(kNoon - 2 * kDay, false);

  ClockKeeper& k = r.start();
  EXPECT_EQ(kRtc, k.timeState());
  EXPECT_EQ(kNoon, k.epoch());
  // Nothing was written through to the clock: on this board the RTC *is* upstream's clock, and a
  // set would write the part for no reason.
  EXPECT_EQ(0, r.upstream.sets);
  EXPECT_FALSE(k.advertsHeld()) << "a node with an RTC has nothing to wait for (section 5.4)";
}

TEST(TheBootOrder, AnRtcThatAnswersWithNonsenseIsNotBelieved) {
  // A fitted RV-3028 that lost its backup answers with 2000-01-01, and upstream's volatile clock
  // answers with its 2024 default; both are before the sane floor and neither is a time.
  const uint32_t kY2k = 946684800;
  for (uint32_t answer : {uint32_t(0), kY2k, uint32_t(1715770351), kSaneEpochCeiling}) {
    Rig r;
    r.fitRtc(answer);
    r.file.hold(kNoon, false);

    ClockKeeper& k = r.start();
    EXPECT_EQ(kRestored, k.timeState()) << "answer " << answer;
    // The file's value wins, and it is written through to the part as well - including when that
    // moves the clock back out of the next century.
    EXPECT_EQ(kNoon + kSaveIntervalSeconds, k.epoch()) << "answer " << answer;
    EXPECT_EQ(1, r.upstream.sets) << "answer " << answer;
  }
}

TEST(TheBootOrder, RamThatSurvivedIsUsedAsItStandsAndBeatsTheFile) {
  Rig r;
  r.retained.hold(kNoon);
  r.file.hold(kNoon - kDay, false);

  ClockKeeper& k = r.start();
  EXPECT_EQ(kRestored, k.timeState());
  EXPECT_EQ(kNoon, k.epoch()) << "the clock simply continues: no margin is needed";
  EXPECT_TRUE(k.advertsHeld());
}

TEST(TheBootOrder, ACorruptRamValueIsTheSameAsNoRamValue) {
  Rig r;
  r.retained.hold(kNoon);
  r.retained.corrupt();
  r.file.hold(kNoon - kDay, false);

  ClockKeeper& k = r.start();
  EXPECT_EQ(kRestored, k.timeState());
  EXPECT_EQ(kNoon - kDay + kSaveIntervalSeconds, k.epoch()) << "the file, with its margin";
}

TEST(TheBootOrder, TheFileIsUsedWithTheSaveIntervalAdded) {
  Rig r;
  r.file.hold(kNoon, false);

  ClockKeeper& k = r.start();
  EXPECT_EQ(kRestored, k.timeState());
  EXPECT_EQ(kNoon + kSaveIntervalSeconds, k.epoch());
  EXPECT_EQ(1, r.upstream.sets);
  EXPECT_TRUE(k.advertsHeld());
}

TEST(TheBootOrder, AFileWrittenForARestartWeAskedForNeedsNoMargin) {
  Rig r;
  r.file.hold(kNoon, /*clean=*/true);

  ClockKeeper& k = r.start(/*software_reset=*/true);
  EXPECT_EQ(kRestored, k.timeState());
  EXPECT_EQ(kNoon, k.epoch()) << "the file was written in the second the restart happened";

  // The flag is spent at once, so that a crash an hour from now cannot restore an hour-old time
  // as if it were fresh.
  EXPECT_FALSE(r.file.cleanInFile());
  EXPECT_EQ(1, r.file.saves);
}

TEST(TheBootOrder, ACleanFileStillGetsTheMarginAfterAnyOtherKindOfReset) {
  // The reset switch, the watchdog, a power cut: the chip did not restart because this firmware
  // asked it to, so the file may be up to an hour old and the margin is what covers that.
  Rig r;
  r.file.hold(kNoon, /*clean=*/true);

  ClockKeeper& k = r.start(/*software_reset=*/false);
  EXPECT_EQ(kNoon + kSaveIntervalSeconds, k.epoch());
}

TEST(TheBootOrder, ACorruptFileIsTheSameAsNoFile) {
  Rig r;
  r.file.hold(kNoon, false);
  r.file.corrupt();

  ClockKeeper& k = r.start();
  EXPECT_EQ(kUnset, k.timeState());
  EXPECT_EQ(1715770351u, k.epoch()) << "upstream's default, which is never right";
  EXPECT_EQ(0, r.upstream.sets);
  EXPECT_TRUE(k.advertsHeld());
}

TEST(TheBootOrder, WithNoMemoryAtAllTheStateIsUnset) {
  Rig r;
  ClockKeeper& k = r.start();
  EXPECT_EQ(kUnset, k.timeState());
  EXPECT_EQ(1715770351u, k.epoch());
  // And nothing is written: a clock that is not a time is not worth a flash page.
  r.tick(5);
  EXPECT_EQ(0, r.file.saves);
  EXPECT_EQ(0, r.retained.saves);
}

TEST(TheBootOrder, ARestoredClockIsNeverEarlierThanTheSavedOnePlusTheInterval) {
  // The property the margin exists for, over a spread of saved values and both memories.
  for (uint32_t saved : {kSaneEpochFloor, kNoon, kNoon + 99 * kDay}) {
    Rig from_file;
    from_file.file.hold(saved, false);
    EXPECT_GE(from_file.start().epoch(), saved + kSaveIntervalSeconds);

    Rig from_ram;
    from_ram.retained.hold(saved);
    EXPECT_GE(from_ram.start().epoch(), saved);
  }
}

TEST(TheBootOrder, TheSurvivingMemoryIsWrittenAtBootSoThatAResetASecondLaterFindsIt) {
  Rig r;
  r.file.hold(kNoon, false);
  r.start();
  EXPECT_EQ(1, r.retained.saves);
  uint32_t in_ram = 0;
  ASSERT_TRUE(r.retained.load(in_ram));
  EXPECT_EQ(kNoon + kSaveIntervalSeconds, in_ram);
}

// ----------------------------------------------------------------------------- setting the time

TEST(SettingTheTime, ForwardIsAlwaysAllowed) {
  Rig r;
  ClockKeeper& k = r.start();
  uint32_t prev = 0;
  ASSERT_TRUE(k.setTime(kNoon, false, prev));
  EXPECT_EQ(1715770351u, prev);
  EXPECT_EQ(kNoon, k.epoch());
  EXPECT_EQ(kSet, k.timeState());
}

TEST(SettingTheTime, BackwardsIsRefusedOnceTheClockHasBeenSet) {
  Rig r;
  ClockKeeper& k = r.start();
  uint32_t prev = 0;
  ASSERT_TRUE(k.setTime(kNoon, false, prev));

  EXPECT_FALSE(k.setTime(kNoon - 10, false, prev)) << "state is `set`: this is ERR_RANGE";
  EXPECT_EQ(kNoon, prev) << "prev_epoch is what the clock says, refused or not";
  EXPECT_EQ(kNoon, k.epoch());
  // The same second is not backwards.
  EXPECT_TRUE(k.setTime(kNoon, false, prev));
}

TEST(SettingTheTime, BackwardsIsAllowedWhileNothingWithATimestampHasLeft) {
  Rig r;
  r.file.hold(kNoon, false);
  ClockKeeper& k = r.start();
  ASSERT_EQ(kRestored, k.timeState());
  EXPECT_FALSE(k.timestampEmitted());

  // The normal case: the restored clock is an hour ahead of the truth and the Walter corrects it
  // before the held advert has gone.
  uint32_t prev = 0;
  ASSERT_TRUE(k.setTime(kNoon, false, prev));
  EXPECT_EQ(kNoon + kSaveIntervalSeconds, prev);
  EXPECT_EQ(kNoon, k.epoch());
  EXPECT_EQ(kSet, k.timeState());
}

TEST(SettingTheTime, BackwardsIsRefusedOnceSomethingHasGoneOnTheAir) {
  Rig r;
  r.file.hold(kNoon, false);
  ClockKeeper& k = r.start();
  k.noteTimestampEmitted();
  EXPECT_TRUE(k.timestampEmitted());

  uint32_t prev = 0;
  EXPECT_FALSE(k.setTime(kNoon, false, prev));
  EXPECT_EQ(kNoon + kSaveIntervalSeconds, k.epoch());
  // Forward still works, and so does force.
  EXPECT_TRUE(k.setTime(kNoon + 2 * kSaveIntervalSeconds, false, prev));
  EXPECT_TRUE(k.setTime(kNoon, true, prev));
  EXPECT_EQ(kNoon, k.epoch());
}

TEST(SettingTheTime, TheBootAdvertIsWhatMakesTheBackwardsCaseTheNormalOne) {
  // Sending the boot advert is itself a timestamp leaving the node, so the window in which a
  // restored clock may be corrected downwards is exactly the hold.
  Rig r;
  r.file.hold(kNoon, false);
  ClockKeeper& k = r.start();
  EXPECT_FALSE(k.timestampEmitted());
  k.noteBootAdvertSent();
  EXPECT_TRUE(k.timestampEmitted());

  uint32_t prev = 0;
  EXPECT_FALSE(k.setTime(kNoon, false, prev));
}

TEST(SettingTheTime, NothingBeforeThisFirmwareExistedIsATimeEvenWithForce) {
  Rig r;
  ClockKeeper& k = r.start();
  uint32_t prev = 0;
  EXPECT_FALSE(k.setTime(0, true, prev));
  EXPECT_FALSE(k.setTime(kSaneEpochFloor - 1, true, prev));
  EXPECT_FALSE(k.setTime(kSaneEpochCeiling, true, prev));
  EXPECT_EQ(kUnset, k.timeState());
  EXPECT_TRUE(k.setTime(kSaneEpochFloor, true, prev));
}

TEST(SettingTheTime, ASetSavesBothMemories) {
  Rig r;
  ClockKeeper& k = r.start();
  uint32_t prev = 0;
  ASSERT_TRUE(k.setTime(kNoon, false, prev));
  EXPECT_EQ(1, r.file.saves);
  EXPECT_EQ(kNoon, r.file.epochInFile());
  EXPECT_FALSE(r.file.cleanInFile()) << "only a restart we asked for writes a clean record";
  uint32_t in_ram = 0;
  ASSERT_TRUE(r.retained.load(in_ram));
  EXPECT_EQ(kNoon, in_ram);
}

TEST(SettingTheTime, AForcedSetBackwardsBringsTheSavedBoundDownWithIt) {
  Rig r;
  ClockKeeper& k = r.start();
  uint32_t prev = 0;
  ASSERT_TRUE(k.setTime(kNoon + kDay, false, prev));
  ASSERT_EQ(kNoon + kDay, r.file.epochInFile());

  // A clock that was set wrong, corrected on a person's instruction: if the file kept the wrong
  // value the next boot would restore it and the correction would be undone.
  ASSERT_TRUE(k.setTime(kNoon, true, prev));
  EXPECT_EQ(kNoon, r.file.epochInFile());
  EXPECT_EQ(2, r.file.saves);
}

TEST(SettingTheTime, AClockWithNoFileAndNoRamStillMoves) {
  Rig r;
  r.ports.file = nullptr;
  r.ports.retained = nullptr;
  r.ports.rtc = nullptr;
  ClockKeeper& k = r.start();
  uint32_t prev = 0;
  EXPECT_TRUE(k.setTime(kNoon, false, prev));
  EXPECT_EQ(kNoon, k.epoch());
  EXPECT_EQ(0, r.file.saves);
}

// ----------------------------------------------------------------------------- the file's rate

TEST(TheFile, IsWrittenAtMostOnceAnHourHoweverOftenTheTimeIsSet) {
  Rig r;
  ClockKeeper& k = r.start();
  uint32_t prev = 0;

  ASSERT_TRUE(k.setTime(kNoon, false, prev));
  EXPECT_EQ(1, r.file.saves) << "the first set, from a clock that was never a time";
  EXPECT_EQ(kNoon, r.file.epochInFile());

  // Thirty-six hundred sets, one second of clock apart - an hour of it - and not one more write.
  for (uint32_t i = 1; i <= kHour; i++) {
    ASSERT_TRUE(k.setTime(kNoon + i, false, prev));
  }
  EXPECT_EQ(1, r.file.saves) << "however often the time is set, the file costs one hour's flash";

  // The first second past the hour writes, and the next hour runs from there.
  ASSERT_TRUE(k.setTime(kNoon + kHour + 1, false, prev));
  EXPECT_EQ(2, r.file.saves);
  EXPECT_EQ(kNoon + kHour + 1, r.file.epochInFile());
  for (uint32_t i = 2; i <= kHour; i++) {
    ASSERT_TRUE(k.setTime(kNoon + kHour + i, false, prev));
  }
  EXPECT_EQ(2, r.file.saves);
}

TEST(TheFile, IsWrittenOnceAnHourWhileTheNodeJustRuns) {
  Rig r;
  ClockKeeper& k = r.start();
  uint32_t prev = 0;
  ASSERT_TRUE(k.setTime(kNoon, false, prev));
  ASSERT_EQ(1, r.file.saves);

  for (int minute = 0; minute < 59; minute++) r.tick(60);
  EXPECT_EQ(1, r.file.saves) << "fifty-nine minutes is not an hour";
  r.tick(61);
  EXPECT_EQ(2, r.file.saves);
  EXPECT_EQ(kNoon + kHour + 1, r.file.epochInFile());
}

TEST(TheFile, IsAlwaysWrittenOnARestartWeAskFor) {
  Rig r;
  ClockKeeper& k = r.start();
  uint32_t prev = 0;
  ASSERT_TRUE(k.setTime(kNoon, false, prev));
  ASSERT_EQ(1, r.file.saves);

  // A minute later, nowhere near the save interval: the restart writes all the same, and marks
  // the record as this second's.
  r.tick(60);
  ASSERT_EQ(1, r.file.saves);
  k.saveBeforeRestart();
  EXPECT_EQ(2, r.file.saves);
  EXPECT_EQ(kNoon + 60, r.file.epochInFile());
  EXPECT_TRUE(r.file.cleanInFile());

  // Twice in a row (the box link saves, then board.reboot() saves again) is two writes of the
  // same second and changes nothing.
  k.saveBeforeRestart();
  EXPECT_EQ(kNoon + 60, r.file.epochInFile());
}

TEST(TheFile, ARestartBeforeThereIsAClockWritesNothing) {
  Rig r;
  ClockKeeper& k = r.start();
  ASSERT_EQ(kUnset, k.timeState());
  k.saveBeforeRestart();
  EXPECT_EQ(0, r.file.saves) << "upstream's 2024 default is not worth saving";
}

TEST(TheFile, AWriteThatFailedChangesNothingWeBelieve) {
  Rig r;
  r.file.writable = false;
  ClockKeeper& k = r.start();
  uint32_t prev = 0;
  ASSERT_TRUE(k.setTime(kNoon, false, prev)) << "the clock moves whatever the flash says";
  EXPECT_EQ(0u, k.fileWrites());

  // The next pass tries again, because nothing was recorded as saved.
  r.file.writable = true;
  r.tick(1);
  EXPECT_EQ(1u, k.fileWrites());
}

// ----------------------------------------------------------------------------- the console

TEST(TheConsole, AClockSetBehindOurBackIsNoticedAndBecomesStateSet) {
  // Upstream's own `time <epoch>` and `clock sync` call rtc_clock.setCurrentTime() directly. The
  // keeper polls for the jump rather than patching upstream's CLI (tasks/phase-1/04).
  Rig r;
  r.file.hold(kNoon, false);
  ClockKeeper& k = r.start();
  ASSERT_EQ(kRestored, k.timeState());

  r.upstream.set(kNoon + kDay);  // what the console does
  r.tickMillis(100);
  EXPECT_EQ(kSet, k.timeState());
  EXPECT_EQ(kNoon + kDay, r.file.epochInFile()) << "and both memories follow";
  EXPECT_FALSE(k.advertsHeld()) << "which also releases the advert";
}

TEST(TheConsole, SecondsPassingIsNotAJump) {
  Rig r;
  r.file.hold(kNoon, false);
  ClockKeeper& k = r.start();
  for (int i = 0; i < 100; i++) r.tick(13);
  EXPECT_EQ(kRestored, k.timeState());
}

TEST(TheConsole, AClockThatGoesBackwardsBehindOurBackDoesNotPullTheMemoriesDown) {
  // `clkreboot` sets the clock to upstream's 2024 default and reboots. The memories are a
  // high-water mark: the next boot restores the saved time, which is the whole point.
  Rig r;
  ClockKeeper& k = r.start();
  uint32_t prev = 0;
  ASSERT_TRUE(k.setTime(kNoon, false, prev));

  r.upstream.set(1715770351);
  r.tickMillis(100);
  EXPECT_EQ(kNoon, r.file.epochInFile());
  EXPECT_EQ(kSet, k.timeState());
}

// ----------------------------------------------------------------------------- the advert hold

TEST(TheAdvertHold, IsReleasedByASet) {
  Rig r;
  r.file.hold(kNoon, false);
  ClockKeeper& k = r.start();
  EXPECT_TRUE(k.advertsHeld());
  EXPECT_FALSE(k.bootAdvertDue());

  uint32_t prev = 0;
  ASSERT_TRUE(k.setTime(kNoon + kHour, false, prev));
  r.tickMillis(1);
  EXPECT_FALSE(k.advertsHeld());
  EXPECT_TRUE(k.bootAdvertDue());

  k.noteBootAdvertSent();
  EXPECT_FALSE(k.bootAdvertDue()) << "once, and only once";
}

TEST(TheAdvertHold, IsReleasedByTheTimeoutAndNotBefore) {
  Rig r;
  ClockKeeper& k = r.start();
  r.tickMillis(kAdvertHoldMs - 1);
  EXPECT_TRUE(k.advertsHeld());
  EXPECT_FALSE(k.bootAdvertDue());
  r.tickMillis(1);
  EXPECT_FALSE(k.advertsHeld());
  EXPECT_TRUE(k.bootAdvertDue()) << "a repeater with no Walter still announces itself";
  EXPECT_EQ(kUnset, k.timeState()) << "with the best time it has, which is none";
}

TEST(TheAdvertHold, IsNeverOnWhenAnRtcAnswered) {
  Rig r;
  r.fitRtc(kNoon);
  ClockKeeper& k = r.start();
  EXPECT_FALSE(k.advertsHeld());
  EXPECT_TRUE(k.bootAdvertDue());
}

TEST(TheAdvertHold, SurvivesTheMillisecondClockWrapping) {
  Rig r;
  r.now_ms = 0xFFFFF000;  // about four seconds before millis() wraps
  ClockKeeper& k = r.start();
  r.tickMillis(kAdvertHoldMs - 1);
  EXPECT_TRUE(k.advertsHeld());
  r.tickMillis(1);
  EXPECT_FALSE(k.advertsHeld());
}

// ----------------------------------------------------------------------------- through the link

namespace {

std::vector<uint8_t> fromHex(const std::string& s) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    out.push_back(uint8_t(std::stoul(s.substr(i, 2), nullptr, 16)));
  }
  return out;
}

std::string toHex(const uint8_t* data, size_t len) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < len; i++) {
    out += digits[data[i] >> 4];
    out += digits[data[i] & 0x0F];
  }
  return out;
}

JsonDocument& fixture() {
  static JsonDocument doc;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    const std::string path = std::string(FIXTURES_DIR) + "/link-v1.json";
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << "cannot read " << path;
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();
    const DeserializationError err = deserializeJson(doc, text);
    EXPECT_TRUE(err == DeserializationError::Ok) << path << ": " << err.c_str();
  }
  return doc;
}

// The fixture case of that kind, type and seq, as hex of its frame - and its fields.
JsonObject caseFields(const char* kind, uint8_t type, uint8_t seq, std::string& frame_hex) {
  JsonArray all = fixture()["cases"].as<JsonArray>();
  for (JsonObject c : all) {
    if (c["kind"].as<std::string>() == kind && c["type"].as<uint8_t>() == type &&
        c["seq"].as<uint8_t>() == seq) {
      frame_hex = c["frame"].as<std::string>();
      return c["fields"].as<JsonObject>();
    }
  }
  ADD_FAILURE() << "no " << kind << " case of type " << int(type) << " seq " << int(seq);
  return JsonObject();
}

// Everything the session needs that is not the clock. The box link's own suite
// (test/test_link_session) is where these are exercised; here they only have to be there.
class StubSystem : public twinoak::link::SystemAccess {
 public:
  uint32_t uptimeSeconds() const override { return 0; }
  uint8_t resetReason() const override { return twinoak::link::kResetPowerOn; }
  uint32_t bootId() const override { return 0; }
  bool radioUp() const override { return true; }
  bool canRestart(uint8_t) const override { return true; }
  void restartNow(uint8_t) override {}
};

class StubIdentity : public twinoak::link::IdentityAccess {
 public:
  uint8_t key[twinoak::link::kPubKeyLen] = {0};
  const uint8_t* publicKey() const override { return key; }
  const char* nodeName() const override { return ""; }
};

// A session with the real keeper behind it, driven the way src/link/SerialTransport.cpp drives
// it. The clock is the only interface present, so HELLO would advertise TIME_PERSIST and nothing
// else - which is what the board does today.
struct LinkRig {
  explicit LinkRig(ClockKeeper& keeper) : link_clock(keeper) {
    firmware.role = twinoak::link::kRoleRepeater;
    services.firmware = &firmware;
    services.system = &system;
    services.identity = &identity;
    services.clock = &link_clock;
  }

  // Feeds one frame and returns the frame the session answers with, as hex, without the leading
  // zero of an idle line.
  std::string ask(const std::vector<uint8_t>& frame) {
    for (uint8_t b : frame) {
      session.feedByte(b, now_ms);
    }
    session.poll(now_ms);
    if (!session.hasOutgoing()) return "";
    std::string out = toHex(session.outgoing(), session.outgoingSize());
    session.outgoingSent(now_ms);
    if (out.rfind("00", 0) == 0) out = out.substr(2);
    return out;
  }

  twinoak::link::Firmware firmware;
  StubSystem system;
  StubIdentity identity;
  LinkClock link_clock;
  twinoak::link::Services services;
  twinoak::link::Session session{services};
  uint32_t now_ms = 5000;
};

std::vector<uint8_t> timeSetFrame(uint8_t seq, uint32_t epoch, uint8_t source, uint8_t flags) {
  uint8_t body[6];
  body[0] = uint8_t(epoch);
  body[1] = uint8_t(epoch >> 8);
  body[2] = uint8_t(epoch >> 16);
  body[3] = uint8_t(epoch >> 24);
  body[4] = source;
  body[5] = flags;
  twinoak::link::FrameWriter w;
  EXPECT_TRUE(w.build(twinoak::link::kMsgTimeSet, seq, body, sizeof(body), false));
  return std::vector<uint8_t>(w.bytes(), w.bytes() + w.size());
}

}  // namespace

TEST(ThroughTheLink, TheFixturesTimeSetAndTimeGetByteForByte) {
  // The fixture's TIME_SET sets 1791720000 from a clock reading 1715770400 and answers with
  // time_state 2; its TIME_GET, forty-two seconds later, answers with 1791720042.
  std::string set_request_hex, set_response_hex, get_request_hex, get_response_hex;
  JsonObject set_request = caseFields("request", twinoak::link::kMsgTimeSet, 5, set_request_hex);
  JsonObject set_response = caseFields("response", twinoak::link::kMsgTimeSet, 5, set_response_hex);
  JsonObject get_request = caseFields("request", twinoak::link::kMsgTimeGet, 7, get_request_hex);
  JsonObject get_response = caseFields("response", twinoak::link::kMsgTimeGet, 7, get_response_hex);
  ASSERT_FALSE(set_request.isNull());
  ASSERT_FALSE(get_request.isNull());

  Rig r;
  // The clock the fixture's prev_epoch says it had: a node whose clock has never been set, forty
  // nine seconds after upstream's default.
  r.upstream.value = set_response["prev_epoch"].as<uint32_t>();
  ClockKeeper& k = r.start();
  ASSERT_EQ(kUnset, k.timeState());

  LinkRig link(k);
  EXPECT_EQ(set_response_hex, link.ask(fromHex(set_request_hex)));
  EXPECT_EQ(set_request["epoch"].as<uint32_t>(), k.epoch());
  EXPECT_EQ(set_response["time_state"].as<uint8_t>(), k.timeState());

  // Forty-two seconds of running, and then TIME_GET.
  r.tick(get_response["epoch"].as<uint32_t>() - k.epoch());
  link.now_ms = r.now_ms;
  EXPECT_EQ(get_response_hex, link.ask(fromHex(get_request_hex)));
}

TEST(ThroughTheLink, TheFixturesRefusalOfABackwardsSetWithoutForce) {
  std::string refusal_hex;
  JsonObject refusal =
      caseFields("response", twinoak::link::kMsgTimeSet, 6, refusal_hex);
  ASSERT_FALSE(refusal.isNull());
  EXPECT_EQ(twinoak::link::kErrRange, refusal["status"].as<uint8_t>());

  Rig r;
  ClockKeeper& k = r.start();
  LinkRig link(k);

  uint32_t prev = 0;
  ASSERT_TRUE(k.setTime(kNoon, false, prev));  // the state is now `set`

  // The same seq the fixture's refusal carries, and an epoch before the clock.
  EXPECT_EQ(refusal_hex, link.ask(timeSetFrame(6, kNoon - 60, 1, 0)));
  EXPECT_EQ(kNoon, k.epoch()) << "and the clock did not move";

  // With force, the same request is accepted - and a different seq, so the one-entry retry
  // memory does not answer it from the refusal (LINK-PROTOCOL.md section 3).
  const std::string forced = link.ask(timeSetFrame(7, kNoon - 60, 3, twinoak::link::kTimeForce));
  EXPECT_NE(refusal_hex, forced);
  EXPECT_EQ(kNoon - 60, k.epoch());
}

TEST(ThroughTheLink, ARetriedTimeSetIsNotAppliedTwice) {
  // The property section 3 exists for, with the one request of this task that changes the node's
  // state: the remembered response is sent again and the clock moves once.
  Rig r;
  ClockKeeper& k = r.start();
  LinkRig link(k);

  const std::vector<uint8_t> frame = timeSetFrame(9, kNoon, 1, 0);
  const std::string first = link.ask(frame);
  EXPECT_EQ(kNoon, k.epoch());
  r.tick(5);
  link.now_ms = r.now_ms;
  const std::string again = link.ask(frame);
  EXPECT_EQ(first, again) << "byte for byte, out of the memory";
  EXPECT_EQ(kNoon + 5, k.epoch()) << "and the five seconds since are not undone";
  EXPECT_EQ(1u, link.session.executed());
}

TEST(ThroughTheLink, StatusCarriesTheClockAndTheHold) {
  Rig r;
  r.file.hold(kNoon, false);
  ClockKeeper& k = r.start();
  LinkRig link(k);

  twinoak::link::FrameWriter w;
  ASSERT_TRUE(w.build(twinoak::link::kMsgStatus, 1, nullptr, 0, false));
  const std::vector<uint8_t> status(w.bytes(), w.bytes() + w.size());

  // The response body after `status`: uptime_s, reset_reason, time_state, epoch, flags...
  std::string answer = link.ask(status);
  ASSERT_FALSE(answer.empty());
  std::vector<uint8_t> frame = fromHex(answer);
  twinoak::link::FrameReceiver rx;
  for (uint8_t b : frame) rx.feedByte(b);
  ASSERT_TRUE(rx.hasPacket());
  twinoak::link::Reader read(rx.packet().body);
  EXPECT_EQ(twinoak::link::kOk, read.u8());
  read.u32();  // uptime_s
  read.u8();   // reset_reason
  EXPECT_EQ(kRestored, read.u8());
  EXPECT_EQ(kNoon + kSaveIntervalSeconds, read.u32());
  const uint16_t flags = read.u16();
  EXPECT_TRUE(read.ok());
  EXPECT_TRUE((flags & twinoak::link::kFlagAdvertsHeld) != 0) << "bit 3 while the hold is on";
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
