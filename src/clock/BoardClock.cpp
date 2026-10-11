#include "BoardClock.h"

#include "../board/target.h"
#include "../link/codec/Numbers.h"       // the reset-reason codes of LINK-PROTOCOL.md 5.3
#include "../link/session/ResetReason.h"  // and the register they are read out of

namespace twinoak {
namespace clock {
namespace {

// --- upstream's clock ---------------------------------------------------------------------------

// `rtc_clock` is upstream's AutoDiscoverRTCClock over a VolatileRTCClock (src/board/target.cpp),
// and it is the clock the mesh uses: MyMesh takes it as mesh::RTCClock& and stamps every advert
// and reply from it. The keeper sets it through setCurrentTime and reads it through
// getCurrentTime, and touches nothing else of upstream's.
class BoardUpstreamClock : public UpstreamClock {
 public:
  uint32_t now() const override { return rtc_clock.getCurrentTime(); }
  void set(uint32_t epoch) override { rtc_clock.setCurrentTime(epoch); }
};

// --- "did a hardware RTC answer?" ---------------------------------------------------------------

// There is no way to ask AutoDiscoverRTCClock whether it found an RTC: its four `*_success`
// flags are file-static in src/helpers/AutoDiscoverRTCClock.cpp at the pin d9296435, and
// getCurrentTime() simply falls back to the volatile clock when none of them is set. So the
// question is answered by the *answer*: VolatileRTCClock starts every boot at 1715770351 (15 May
// 2024, src/helpers/ArduinoHelpers.h), which is years before kSaneEpochFloor, while an RV-3028
// that has kept its backup answers with a time after it. The keeper applies isSaneEpoch() to
// what this returns, so a fitted-but-never-set RTC (which answers with 2000-01-01 or 1970) is
// not believed either.
//
// The first-batch boards have no RTC fitted at all: the RV-3028-C7 and its backup capacitors are
// DNP for a worldwide allocation (the hardware repository's PROJECT-KNOWLEDGE.md section 1 at
// b798aab0 - "Firmware consequence: the Walter must set the RAK's time after every boot and
// every kill cycle"). The footprint is kept, so this path is what a hand-fitted part will take.
class BoardRtc : public HardwareRtc {
 public:
  bool read(uint32_t& epoch) override {
    epoch = rtc_clock.getCurrentTime();
    return true;
  }
};

// --- the file ----------------------------------------------------------------------------------

// The node's file system, beside upstream's own files (/prefs.json, the identity, the ACL) in the
// region docs/ARCHITECTURE.md section 9 maps. An update never erases it.
const char* const kClockFile = "/clock";

// The core's File lives in a namespace of its own (Adafruit_LittleFS_File.h) and nothing of
// upstream's brings it in unqualified on this platform, so it is named in full once here.
typedef Adafruit_LittleFS_Namespace::File ClockFileHandle;

class BoardClockFile : public ClockFile {
 public:
  void attach(FILESYSTEM* fs) { fs_ = fs; }

  bool load(SavedTime& out) override {
    if (fs_ == nullptr || !fs_->exists(kClockFile)) return false;
    uint8_t buf[kSavedTimeBytes] = {0};
    ClockFileHandle file = fs_->open(kClockFile);
    if (!file) return false;
    const int n = file.read(buf, uint16_t(sizeof(buf)));
    file.close();
    if (n != int(sizeof(buf))) return false;
    return decodeSavedTime(buf, sizeof(buf), out);
  }

  bool save(const SavedTime& in) override {
    if (fs_ == nullptr) return false;
    uint8_t buf[kSavedTimeBytes] = {0};
    encodeSavedTime(in, buf);
    // FILE_O_WRITE is LFS_O_RDWR | LFS_O_CREAT and seeks to the end of the file
    // (Adafruit_LittleFS_File.cpp at the core's pinned commit d541301), so the seek is what makes
    // this a rewrite in place of a fixed-size record rather than an append. Upstream removes and
    // recreates /prefs.json instead (CommonCLI::savePrefs); in place is fewer commits into
    // littlefs for the same bytes, which is the whole point of the save interval.
    ClockFileHandle file = fs_->open(kClockFile, Adafruit_LittleFS_Namespace::FILE_O_WRITE);
    if (!file) return false;
    bool ok = file.seek(0);
    if (ok) ok = (file.write(buf, sizeof(buf)) == sizeof(buf));
    file.close();
    return ok;
  }

 private:
  FILESYSTEM* fs_ = nullptr;
};

// --- the one of each ---------------------------------------------------------------------------

BoardUpstreamClock g_upstream;
BoardRtc g_rtc;
BoardClockFile g_file;

ClockPorts makePorts() {
  ClockPorts ports;
  ports.upstream = &g_upstream;
  ports.rtc = &g_rtc;
  ports.file = &g_file;
  // ports.retained stays null: there is no place in this board's RAM that this firmware can show
  // survives a reset. The core's startup copies __data_start__ .. __bss_start__ out of flash and
  // the C library zeroes __bss_start__ .. __bss_end__, so the only gap a linker orphan can be
  // given is above __bss_end__ - and that address (about 0x2000E600 in this image) is inside the
  // RAM the Adafruit bootloader uses for itself, which runs before us on every reset.
  // tasks/phase-1/reports/04.md has the whole reading, with the linker script, the startup's
  // three loops and the bootloader image's own numbers.
  return ports;
}

ClockKeeper g_keeper(makePorts());

}  // namespace

void begin(FILESYSTEM& fs) {
  g_file.attach(&fs);
  // Reason 4 of LINK-PROTOCOL.md section 5.3 - a software reset - is the one that says this boot
  // followed a restart the firmware itself performed, and therefore that the file's `clean` value
  // is that restart's own second. RESETREAS is captured early by upstream and read through the
  // board object, exactly as the box link's STATUS reads it (src/link/BoardLink.cpp).
  const bool software_reset =
      link::resetReasonFromNrf52(board.getResetReason()) == link::kResetSoftware;
  g_keeper.begin(millis(), software_reset);
}

void loop() { g_keeper.loop(millis()); }

ClockKeeper& keeper() { return g_keeper; }

bool bootAdvertDue() { return g_keeper.bootAdvertDue(); }
void noteBootAdvertSent() { g_keeper.noteBootAdvertSent(); }
void noteTimestampEmitted() { g_keeper.noteTimestampEmitted(); }
void saveBeforeRestart() { g_keeper.saveBeforeRestart(); }

const char* stateName() {
  switch (g_keeper.timeState()) {
    case kRestored:
      return "restored";
    case kSet:
      return "set";
    case kRtc:
      return "rtc";
    case kUnset:
    default:
      return "unset";
  }
}

}  // namespace clock
}  // namespace twinoak
