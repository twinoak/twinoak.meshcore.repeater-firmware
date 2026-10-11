// The band check, as two static_asserts: a board environment whose band and default frequency
// disagree does not compile, and the error says so in a sentence.
//
// Every board environment compiles this file (platformio.ini, build_src_filter: +<common/>).
// It has no code and leaves no bytes in the image - the whole of it is the two assertions
// below, made against the table of src/common/bands/Bands.h and the two defines the environment
// was built with.
//
// It is not compiled by [env:native]: LORA_FREQ is a radio default and only a board build has
// one. What the native tests hold instead is the mechanism - the table and the lookup, with a
// made-up band (test/test_image).

#include "common/bands/Bands.h"

#ifndef TWINOAK_BAND
#error "TWINOAK_BAND is set by platformio.ini from the environment's [img_*] section"
#endif
#ifndef LORA_FREQ
#error "LORA_FREQ is the radio's default frequency, set by platformio.ini from a [band_*] section"
#endif

namespace {
constexpr const twinoak::bands::Band* kRows = twinoak::bands::kBands;
constexpr size_t kCount = twinoak::bands::kBandCount;
}  // namespace

static_assert(twinoak::bands::known(kRows, kCount, TWINOAK_BAND),
              "this environment's TWINOAK_BAND is not a band this firmware knows: add a row to "
              "src/common/bands/Bands.h and a [band_<name>] section to platformio.ini, and "
              "measure a radio against both before you ship it");

static_assert(twinoak::bands::holds(kRows, kCount, TWINOAK_BAND, LORA_FREQ),
              "this environment's default LoRa frequency is outside the band the image says it "
              "is in: LORA_FREQ comes from platformio.ini's [band_*] section and TWINOAK_BAND "
              "from the environment's [img_*] section, and the two have been allowed to "
              "disagree. An image is refused on a radio of another band, and a band is not a "
              "label (spec/FIRMWARE-MANIFEST.md section 2)");
