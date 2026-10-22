// Our own preferences: the two battery thresholds, kept across a restart in a small versioned
// file of this firmware's own and **not** in upstream's /prefs.json
// (tasks/phase-1/06-power-and-environment.md).
//
// Why not upstream's: /prefs.json is a fixed C struct that upstream's CommonCLI writes whole
// (src/helpers/CommonCLI.cpp:143-158, savePrefs, at the pin 81dbae59). Adding a field of ours to it would
// either mean editing a file of upstream's - which this repository never does - or writing a
// record upstream would overwrite the next time anybody typed `set name`. A file of our own is
// one page of littlefs and nobody else's business, which is also how the clock keeps its lower
// bound (src/roles/repeater/clock/ClockKeeper.h).
//
// The record is the clock file's shape: a magic that carries the version, the values, a flags
// byte and one check byte, so that a half-written or foreign file is refused rather than
// believed. A later version writes a new magic and this one refuses it, which is what "versioned"
// buys.
//
// Host-portable: no Arduino, no MeshCore, no heap, no floating point. src/common/power/BoardPower.cpp is
// the dozen lines that put it in the file system.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace twinoak {
namespace power {

// What the file holds.
struct SavedPrefs {
  uint16_t low_mv = 0;
  uint16_t crit_mv = 0;
};

// magic (4) + low (2) + crit (2) + flags (1) + check (1).
constexpr size_t kSavedPrefsBytes = 10;

void encodeSavedPrefs(const SavedPrefs& in, uint8_t out[kSavedPrefsBytes]);
// False when the bytes are not a record of this version, when the check byte disagrees, or when
// the pair is not one this firmware accepts (BatteryWatch.h, thresholdsAcceptable) - a file that
// survived a firmware whose window was wider must not put this node outside it.
bool decodeSavedPrefs(const uint8_t* data, size_t len, SavedPrefs& out);

}  // namespace power
}  // namespace twinoak
