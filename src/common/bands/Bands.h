// The bands this firmware is built for, and the one rule that holds a build to the band it says
// it is in.
//
// An image is one role on one board in one **band** (spec/FIRMWARE-MANIFEST.md section 2), and
// the band is not a label: a 433 MHz module is a different part, and an 868 image on it would
// transmit outside the allocation it was certified in. platformio.ini keeps one `[band_*]`
// section per band - the default frequency, bandwidth, spreading factor, coding rate and
// transmit power - and an environment takes its radio defaults from the section its own band
// names. This header is the other half of that: a table of what each band *is*, and a check,
// made at compile time, that the frequency an environment ended up with is inside it.
//
// Portable and constexpr (C++11, the board's standard): no Arduino, no MeshCore. The check is a
// static_assert in src/common/bands/BandCheck.cpp, which every board environment compiles; the
// table and the lookup are tested natively, with a made-up band, in test/test_image.
//
// **Adding a band** (docs/ARCHITECTURE.md section 8): a `[band_<name>]` section in
// platformio.ini and a row here. 433 has neither yet - the mechanism is proved by the test's
// made-up table and not by a block nobody has measured a radio against.

#pragma once

#include <stddef.h>

namespace twinoak {
namespace bands {

// One band: the name as spec/FIRMWARE-MANIFEST.md spells it, and the frequencies an image of
// that band may be built for, in MHz, ends included.
struct Band {
  const char* name;
  double low;
  double high;
};

// The 868 band is the EU's 863-870 MHz allocation; the Danish MeshCore channel at 869.618 MHz
// sits inside it (platformio.ini's [band_868]).
constexpr Band kBands[] = {
    {"868", 863.0, 870.0},
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

}  // namespace bands
}  // namespace twinoak
