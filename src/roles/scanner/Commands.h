// The scanner's command line: the words the Walter (or a laptop on the USB-C) sends, and the
// numbers each one carries.
//
// NoiseScope's handle_command with the radio and the output taken out of it
// (software/firmware/noisescope-rak4630/src/main.cpp at b798aab0 of the hardware repository).
// Plain-text commands, one per line, space-separated, the command word case-insensitive, at most
// eight tokens - spec/LINK-PROTOCOL.md section 9 and docs/noisescope.md are the page. **Not
// frames**: the scanner is the one role of this family that speaks text on the box link's UART,
// and the Walter tells the two apart by what comes back.
//
// Host-portable - no Arduino, C++11 - and compiled by [env:native] beside test/test_scanner, so
// what a command means is settled by a test and not by a board.

#pragma once

#include <stddef.h>
#include <stdint.h>

// For ScopeAgg, which is a `scope` argument and a `scope` field both: what the words mean is
// settled once, where the rest of that mode's arithmetic is.
#include "roles/scanner/Sweep.h"

namespace twinoak {
namespace scanner {

// The line buffer the board reads into, NoiseScope's own size: longer than any command of
// docs/noisescope.md and short enough that two of them are nothing beside the sweep buffers.
constexpr size_t kMaxLineBytes = 160;

// At most this many words are looked at; `scope f0 f1 points rbw interval agg` is the longest
// command and has seven.
constexpr int kMaxTokens = 8;

// A `time` before this is not a time: it is a host that sent a counter, a zero or a parse
// accident (November 2023).
constexpr uint32_t kEpochFloor = 1700000000u;

enum Verb : uint8_t {
  kVerbUnknown = 0,
  kVerbHelp,
  kVerbInfo,
  kVerbStat,
  kVerbStop,
  kVerbSweep,
  kVerbScope,
  kVerbHist,
  kVerbDwell,
  kVerbCad,
  kVerbHome,
  kVerbBoost,
  kVerbAuto,
  kVerbTime,
  kVerbReboot,
  kVerbDfu,
};

// The words of one line, split in place: `line` is modified and the pointers are into it, so it
// must outlive the result. The command word is lowercased; an argument keeps its case.
struct Tokens {
  int count = 0;
  const char* at[kMaxTokens] = {0};
};

Tokens tokenize(char* line);

// The verb of a command word, kVerbUnknown for one this firmware does not have.
Verb verbOf(const char* word);

// The word as docs/noisescope.md spells it - what an `ack` line carries back. An empty string
// for kVerbUnknown, whose ack carries what the operator actually typed.
const char* nameOf(Verb verb);

// The usage line a refused command answers with, or an empty string for a verb that takes no
// argument it could be refused over.
const char* usageOf(Verb verb);

// ------------------------------------------------------------------ the arguments

// What `sweep` and `hist` mean with no arguments (platformio.ini and src/roles/scanner/config.h).
struct ScanDefaults {
  double f0 = 0.0;
  double f1 = 0.0;
  double step_khz = 0.0;
  uint32_t sweep_dwell_ms = 0;
  uint32_t hist_samples = 0;
};

// `sweep [f0 f1 [step_khz [dwell_ms [passes]]]]` and `hist [f0 f1 [step_khz [samples [passes]]]]`.
// `fourth` is the dwell in milliseconds for a sweep and the sample count for a histogram.
// `passes` of 0 means "until stop".
struct ScanArgs {
  double f0 = 0.0;
  double f1 = 0.0;
  double step_khz = 0.0;
  uint32_t fourth = 0;
  int32_t passes = 1;
};

// Always succeeds: every argument has a default, and whether the numbers are a scan this
// firmware runs is src/roles/scanner/Sweep.h's answer and not the parser's. Both ends are read
// together or not at all, as NoiseScope read them - `sweep 865` is the default sweep.
ScanArgs parseScan(const Tokens& tokens, bool is_hist, const ScanDefaults& defaults);

// `scope <f0> <f1> [points [rbw_khz [interval_ms [agg]]]]`, and `scope off`.
//
// Unlike `sweep`, both ends are **required**: there is no default span for a mode whose point is
// that the span is the operator's choice, from a channel to the whole of what the radio tunes.
// Whether the numbers are a scope this firmware runs is src/roles/scanner/Sweep.h's answer
// (planScope) and not the parser's; what the parser refuses is a missing end and an `agg` that
// is not one of the three words.
struct ScopeArgs {
  bool off = false;  // `scope off`, which is `stop` for this mode alone
  double f0 = 0.0;
  double f1 = 0.0;
  uint32_t points = 0;       // 0 = the plan's default
  double rbw_khz = 0.0;      // 0 = the plan's default
  uint32_t interval_ms = 0;  // 0 = a frame per sweep
  ScopeAgg agg = kScopeAggMax;
};
bool parseScope(const Tokens& tokens, ScopeArgs& out);

// `dwell <f_mhz> [secs [win_ms]]`. False when no frequency was given; the two optional numbers
// are clamped by src/roles/scanner/Sweep.h.
struct DwellArgs {
  double f_mhz = 0.0;
  uint32_t secs = 60;
  uint32_t win_ms = 1000;
};
bool parseDwell(const Tokens& tokens, DwellArgs& out);

// `cad [f_mhz [count]]`, which defaults to the home channel.
struct CadArgs {
  double f_mhz = 0.0;
  uint32_t count = 100;
};
CadArgs parseCad(const Tokens& tokens, double home_f_mhz);

// `home <f_mhz> [bw_khz sf cr]`. False when no frequency was given. The three modem settings are
// taken together - `home 869.4` retunes and keeps the rest - and whether they are settings the
// radio has is src/roles/scanner/Sweep.h's homeSettingsOk().
struct HomeArgs {
  double f_mhz = 0.0;
  double bw_khz = 0.0;
  uint8_t sf = 0;
  uint8_t cr = 0;
};
bool parseHome(const Tokens& tokens, const HomeArgs& current, HomeArgs& out);

// `boost on|off`. False when the word is missing. "on" and "1" are on and anything else is off,
// as NoiseScope read it.
bool parseBoost(const Tokens& tokens, bool& on);

// `auto <minutes>`, 0 = off. False when the number is missing.
bool parseAuto(const Tokens& tokens, uint32_t& minutes);

// `time <unix_epoch>`. False when it is missing or below kEpochFloor.
bool parseEpoch(const Tokens& tokens, uint32_t& epoch);

// `dfu [serial]`: true for the bootloader's serial-only DFU, false for the UF2 drive.
bool parseDfuSerialOnly(const Tokens& tokens);

}  // namespace scanner
}  // namespace twinoak
