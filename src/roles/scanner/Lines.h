// What the scanner says, and the only place it is spelled: NDJSON, one object per line, exactly
// the lines docs/noisescope.md documents.
//
// This is NoiseScope's out.cpp and the emit_* functions of its main.cpp and scanner.cpp
// (software/firmware/noisescope-rak4630/ at b798aab0 of the hardware repository), with the
// serial ports replaced by a sink. **Not frames**: the box link's codec is for the repeater, and
// a Walter that meets text on this UART knows it is talking to a scanner
// (spec/LINK-PROTOCOL.md section 9).
//
// Host-portable - no Arduino, no RadioLib, C++11 - so the lines are what test/test_scanner
// parses: every one of them is valid JSON and every number in it is finite. The second half of
// that is why nothing here prints a `double` with a bare format: `number()` writes `null` for a
// NaN or an infinity, because `nan` is not JSON and a line a parser drops is worse than a field
// it reads as missing. Only src/roles/scanner/BoardLines.cpp knows what a serial port is.
//
// Every data line carries `seq` (monotonic per boot - a gap is a lost line, a restart is a
// reboot), `ts` (unix epoch, 0 = the clock was never set) and `ms` (millis since boot).

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "roles/scanner/Sweep.h"

namespace twinoak {
namespace scanner {

// Where a line goes. The board's writes to the Walter's UART always and to USB CDC when a host
// is attached; the tests' appends to a string.
struct LineSink {
  virtual ~LineSink() {}
  // `text` is a NUL-terminated fragment, with no newline of its own.
  virtual void write(const char* text) = 0;
};

// The three fields every data line carries.
struct Meta {
  uint32_t seq = 0;
  uint32_t ts = 0;
  uint32_t ms = 0;
};

// `boot`, once after every reset.
struct BootFields {
  const char* fw = "";       // this firmware's name, "noisescope"
  const char* version = "";  // the image's own version, out of its descriptor
  const char* role = "";     // and its role, and its board - phase 3's task 02
  const char* board = "";
  const char* hw = "";
  const char* radio = "";
  const char* reset = "";  // por | pin | wdt | soft | lockup
  bool radio_ok = false;
  bool rtc = false;
  bool ina = false;
  uint32_t ts = 0;
};

// `info`, on request.
struct InfoFields {
  BootFields boot;
  const char* build = "";   // __DATE__ of the compile
  uint32_t devid_high = 0;  // the nRF52's own device id
  uint32_t devid_low = 0;
  bool boosted_gain = false;
  uint32_t auto_minutes = 0;
  double home_f_mhz = 0.0;
  double home_bw_khz = 0.0;
  uint8_t home_sf = 0;
  uint8_t home_cr = 0;
};

// The three INA3221 channels as `stat.pwr` reports them: panel, battery, load
// (src/common/power/Ina3221.h, which is where the per-channel shunts are applied).
struct PowerFields {
  bool present = false;
  uint16_t vpanel_mv = 0;
  int16_t ipanel_ma = 0;
  uint16_t vbat_mv = 0;
  int16_t ibat_ma = 0;
  uint16_t vload_mv = 0;
  int16_t iload_ma = 0;
};

// `stat`, the heartbeat. `rssi` is present in monitor mode only; `pwr` when the INA3221 answered.
struct StatFields {
  Meta meta;
  uint32_t uptime_s = 0;
  const char* mode = "";
  double home_f_mhz = 0.0;
  bool have_rssi = false;
  WindowStats rssi;
  PowerFields pwr;
};

// `sweep`, one line per completed pass. `avg` and `max` are `plan.points` long and point *i* is
// at `f0 + i*step/1000` MHz.
struct SweepFields {
  Meta meta;
  SweepPlan plan;
  uint16_t dwell_ms = 0;
  const double* avg = nullptr;
  const double* max = nullptr;
};

// `hist`, one line per frequency per pass: the spectral-scan engine's 33 bins of 4 dB, bin 0 at
// -11 dBm.
struct HistFields {
  Meta meta;
  double f_mhz = 0.0;
  double rbw_khz = 0.0;
  uint16_t samples = 0;
  const uint16_t* bins = nullptr;  // kHistBins of them
};

// `dwell`, one line per window: `stat.rssi`'s statistics at the commanded frequency.
struct DwellFields {
  Meta meta;
  double f_mhz = 0.0;
  uint32_t win_ms = 0;
  WindowStats stats;
};

// `cad`: how many of `n` channel-activity detections found a LoRa preamble.
struct CadFields {
  Meta meta;
  double f_mhz = 0.0;
  uint8_t sf = 0;
  double bw_khz = 0.0;
  uint16_t n = 0;
  uint16_t detections = 0;
};

class Lines {
 public:
  explicit Lines(LineSink& sink) : sink_(sink) {}

  // --- the messages of docs/noisescope.md --------------------------------------------------

  void boot(const BootFields& fields);
  void info(const InfoFields& fields);
  void stat(const StatFields& fields);
  void sweep(const SweepFields& fields);
  void hist(const HistFields& fields);
  void dwell(const DwellFields& fields);
  void cad(const CadFields& fields);
  void done(const char* command, const Meta& meta);
  void ack(const char* command, bool ok, const char* message);
  void err(const char* message);

  // The command list, as `# ` chatter: any line that does not begin with '{' is for a person and
  // a machine parser skips it (docs/noisescope.md).
  void help(const char* version);

  // --- the pieces ---------------------------------------------------------------------------

  // A fragment, and the newline that ends a line. Public because src/roles/scanner/Radio.cpp
  // has one line (`err`) to write from inside a scan step.
  void raw(const char* text) { sink_.write(text); }
  void end() { sink_.write("\n"); }

 private:
  // A number, with `decimals` after the point - or `null` when it is not a finite one.
  void number(double value, int decimals);
  void field(const char* name, double value, int decimals);
  void meta(const Meta& meta);
  void bootBody(const BootFields& fields);
  void homeBody(double f_mhz, double bw_khz, uint8_t sf, uint8_t cr);
  void doubles(const double* values, uint16_t count, int decimals);
  void text(const char* fmt, ...)
#if defined(__GNUC__)
      __attribute__((format(printf, 2, 3)))
#endif
      ;

  LineSink& sink_;
};

}  // namespace scanner
}  // namespace twinoak
