// Every upstream header the scanner uses, included from one place, as system headers.
//
// This is src/common/board/UpstreamHeaders.h's part for this role, and it stops two libraries
// short of it: the scanner compiles **no MeshCore** (platformio.ini,
// [env:scanner_rak4630_single]: lib_ignore) - it has no mesh, no identity and no file system -
// so what it needs above the Arduino core is RadioLib, SPI and the nRF52's watchdog registers.
//
// src/ is built with -Wall -Wextra -Werror (build_src_flags) and these headers are not written
// to that standard; `#pragma GCC system_header` makes the rest of this file a system header, and
// GCC treats what a system header includes as system headers too, so -Werror stays about our own
// code. The one thing it does not silence is a `#warning` directive, and RadioLib's "God mode
// active" is one - but this environment does not define RADIOLIB_GODMODE, so it is not raised
// here at all (the repeater's build has -Wno-error=cpp for it).
//
// The Arduino core also brings in **MeshCore's variants/rak4631/variant.h** through Arduino.h,
// which is where the SX1262's pins, the DIO2 RF switch, the DIO3 TCXO voltage and
// SX126X_POWER_EN come from: the Adafruit core at the pin has no WisCore_RAK4631_Board variant
// of its own, so the pin map is upstream's for both roles of this board (platformio.ini's -I,
// and tools/pio/meshcore_variant.py, which compiles variant.cpp out of the pinned checkout).

#pragma once

#pragma GCC system_header

#include "common/board/CoreHeaders.h"  // Arduino and Wire, and through them variant.h

#include <SPI.h>
#include <nrf_wdt.h>

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>

// Semtech's spectral-scan RAM patch (origin: Lora-net/sx1302_hal, shipped with RadioLib). It is
// a ~2 kB blob uploaded into the SX1262's RAM at the start of a `hist` session and purged by a
// hardware reset at the end (docs/noisescope.md, "Design notes").
#include <modules/SX126x/patches/SX126x_patch_scan.h>
