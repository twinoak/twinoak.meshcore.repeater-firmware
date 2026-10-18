// The bands this firmware is built for, and the one rule that holds a build to the band it says
// it is in.
//
// An image is one role on one board in one **band** (spec/FIRMWARE-MANIFEST.md section 2), and
// the band is not a label: a 433 MHz module is a different part, and an 868 image on it would
// transmit outside the allocation it was certified in. platformio.ini keeps one `[band_*]`
// section per band - the default frequency, bandwidth, spreading factor, coding rate and
// transmit power, and the duty cycle its regulation allows - and an environment takes its radio
// defaults from the section its own band names. This header is the other half of that: a table
// of what each band *is*, and the checks, made at compile time, that the frequency an
// environment ended up with is inside it and that every band says what duty cycle it allows.
//
// Portable and constexpr (C++11, the board's standard): no Arduino, no MeshCore. The checks are
// static_asserts in src/common/bands/BandCheck.cpp, which every board environment compiles; the
// table and the lookups are tested natively, with a made-up band, in test/test_image.
//
// **Adding a band** (docs/ARCHITECTURE.md section 8): a `[band_<name>]` section in
// platformio.ini and a row here. 433 has neither yet - the mechanism is proved by the test's
// made-up table and not by a block nobody has measured a radio against.

#pragma once

#include <stddef.h>

namespace twinoak {
namespace bands {

// One band: the name as spec/FIRMWARE-MANIFEST.md spells it, the frequencies an image of that
// band may be built for, in MHz, ends included, and the **duty cycle its regulation allows**, as
// a whole percentage of one hour.
//
// The duty cycle is in this row for the reason the frequencies are: it is what the band *is*, not
// a judgement a site makes. Upstream's own accounting is the only thing that can hold it - it
// sees the hundreds of packets a repeater forwards as well as ours (docs/ARCHITECTURE.md
// section 7) - and it starts a repeater at 50 %, so a fresh unit is asked for the band's value on
// its first boot (src/roles/repeater/firstboot/Profile.cpp builds `set dutycycle <this>` from
// this number). A *lower* value is a site's to send afterwards; a higher one is not anybody's.
struct Band {
  const char* name;
  double low;
  double high;
  int duty_cycle;  // whole percent, 1-100; BandCheck.cpp refuses a row without one
};

// The 868 band is the EU's 863-870 MHz allocation; the Danish MeshCore channel at 869.618 MHz
// sits inside it (platformio.ini's [band_868]), in the 869.400-869.650 MHz sub-band of ERC
// Recommendation 70-03, Annex 1, row h1.7 - 500 mW e.r.p. and a duty cycle of 10 %, or LBT+AFA
// instead, which this firmware does not claim (Edition of June 2024, the newest CEPT lists).
//
// The row's range is the whole allocation and its duty cycle is that sub-band's, which is the
// narrower of the two: 869.618 MHz is the only frequency any image of this band is built for
// (platformio.ini's [band_868].freq, held against this row by BandCheck.cpp), and a band whose
// images were spread across sub-bands with different limits would need a row per sub-band.
constexpr Band kBands[] = {
    {"868", 863.0, 870.0, 10},
};
constexpr size_t kBandCount = sizeof(kBands) / sizeof(kBands[0]);

// Two strings, character for character. A constexpr strcmp: C++11 allows one return statement
// and recursion, which is all this needs.
constexpr bool sameName(const char* a, const char* b) {
  return (*a == *b) && (*a == '\0' || sameName(a + 1, b + 1));
}

// The row for `band`, or -1 when the table has none.
constexpr int indexOf(const Band* rows, size_t count, const char* band, size_t i = 0) {
  return i >= count ? -1
                    : (sameName(rows[i].name, band) ? static_cast<int>(i)
                                                    : indexOf(rows, count, band, i + 1));
}

constexpr bool known(const Band* rows, size_t count, const char* band) {
  return indexOf(rows, count, band) >= 0;
}

// The ends of a band, in MHz, or 0 for a band the table does not know - which cannot be a
// frequency, so a caller that forgot to ask `known()` first gets a range no radio tunes rather
// than a plausible one. The scanner's default sweep is a band's own range and is written nowhere
// else (src/roles/scanner/config.h).
constexpr double lowOf(const Band* rows, size_t count, const char* band) {
  return indexOf(rows, count, band) < 0 ? 0.0 : rows[indexOf(rows, count, band)].low;
}

constexpr double highOf(const Band* rows, size_t count, const char* band) {
  return indexOf(rows, count, band) < 0 ? 0.0 : rows[indexOf(rows, count, band)].high;
}

// Whether `mhz` is inside `band`. False for a band the table does not know, so a new band
// cannot be shipped by naming it alone.
constexpr bool holds(const Band* rows, size_t count, const char* band, double mhz) {
  return indexOf(rows, count, band) >= 0 &&
         mhz >= rows[indexOf(rows, count, band)].low &&
         mhz <= rows[indexOf(rows, count, band)].high;
}

// The duty cycle `band`'s regulation allows, as a whole percentage, or 0 for a band the table
// does not know - which is not a duty cycle any radio may transmit at, so a caller that forgot
// to ask `known()` first gets a number that refuses rather than a plausible one. The first-boot
// profile's `set dutycycle` line is this number (src/roles/repeater/firstboot/Profile.cpp).
constexpr int dutyCycleOf(const Band* rows, size_t count, const char* band) {
  return indexOf(rows, count, band) < 0 ? 0 : rows[indexOf(rows, count, band)].duty_cycle;
}

// Whether every row of the table carries a duty cycle a radio may be set to. Upstream's `set
// dutycycle` takes 1 to 100 and refuses anything else (CommonCLI::handleSetCmd,
// src/helpers/CommonCLI.cpp:450-461 at the pin d9296435), so a row outside that range is a
// profile line that would be refused on every first boot - and a row left at 0 is a band whose
// regulation nobody looked up. src/common/bands/BandCheck.cpp makes this a compile error.
constexpr bool everyDutyCycleIsSettable(const Band* rows, size_t count, size_t i = 0) {
  return i >= count ? true
                    : (rows[i].duty_cycle >= 1 && rows[i].duty_cycle <= 100 &&
                       everyDutyCycleIsSettable(rows, count, i + 1));
}

}  // namespace bands
}  // namespace twinoak
