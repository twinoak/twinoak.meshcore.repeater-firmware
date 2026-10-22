// NoiseScope's build-time configuration, as it came from the hardware repository
// (software/firmware/noisescope-rak4630/src/config.h at b798aab0) with the values that are
// written elsewhere in this repository taken from there instead.
//
// What is no longer here, and where it went:
//
//   the SX1262's pins, the RF switch, the TCXO voltage
//                        MeshCore's variants/rak4631/variant.h, through Arduino.h
//                        (src/roles/scanner/ScannerHeaders.h). NoiseScope restated them with
//                        #ifndef guards, and they were the variant's values
//   the home channel     platformio.ini's [band_868], through LORA_FREQ/BW/SF/CR - the same five
//                        lines the repeater's radio is built from, so the two images a repeater
//                        swaps listen on the same channel by construction
//   the default sweep    the band's own range (src/common/bands/Bands.h), which is what
//                        NoiseScope's 863.0-870.0 was
//   the INA3221          src/common/power/Ina3221.h: the address, the three shunts and the
//                        arithmetic, which NoiseScope's telemetry.cpp had its own copy of
//   the version          the image descriptor's, read out of the image's own 96 bytes
//                        (src/common/fwdesc/ImageDescriptor.h)
//
// Every value below can still be overridden with -D <NAME>=<value> in platformio.ini.

#pragma once

#include "common/bands/Bands.h"

#ifndef TWINOAK_BAND
#error "TWINOAK_BAND is set by platformio.ini from the environment's [img_*] section"
#endif
#ifndef LORA_FREQ
#error "LORA_FREQ is the radio's default frequency, set by platformio.ini from a [band_*] section"
#endif

// The name the NDJSON `boot` and `info` lines carry, which is this firmware's own and not the
// image's role (docs/noisescope.md; the role is the descriptor's).
#ifndef FW_NAME
#define FW_NAME "noisescope"
#endif

// ---- Home channel (what the repeater of the same band listens on; monitor mode uses this) ----
#ifndef HOME_FREQ_MHZ
#define HOME_FREQ_MHZ LORA_FREQ
#endif
#ifndef HOME_BW_KHZ
#define HOME_BW_KHZ LORA_BW
#endif
#ifndef HOME_SF
#define HOME_SF LORA_SF
#endif
#ifndef HOME_CR
#define HOME_CR LORA_CR
#endif

// ---- The radio's RX path, which must be the repeater's ----
// SX126X_DIO2_AS_RF_SWITCH and SX126X_DIO3_TCXO_VOLTAGE are the variant's. The other two are
// [env:repeater_rak4630_single]'s own flags, and the scanner environment passes the same values:
// a reading is only comparable with what the repeater hears if the front end is the same one.
#ifndef SX126X_CURRENT_LIMIT
#define SX126X_CURRENT_LIMIT 140.0f
#endif
#ifndef SX126X_RX_BOOSTED_GAIN
#define SX126X_RX_BOOSTED_GAIN 1
#endif

// ---- Walter management UART ----
// TwinOak RAK4630 adapter: J4/J5 jumpers 1-2 -> Serial1 (P0.15 RX / P0.16 TX, default),
// jumpers 2-3 -> Serial2 (P0.19 / P0.20). Override WALTER_SERIAL to Serial2 if strapped so.
// Serial1 is the link to the Walter on every board and in every role
// (docs/ARCHITECTURE.md section 8).
#ifndef WALTER_SERIAL
#define WALTER_SERIAL Serial1
#endif
#ifndef WALTER_BAUD
#define WALTER_BAUD 115200  // spec/LINK-PROTOCOL.md section 9; INTERCONNECT.md's bulkhead filter
#endif

// ---- Default sweep (also used by the periodic auto-sweep) ----
// The band's own range, from the one table that says what a band is. NoiseScope's own defaults
// were 863.0 and 870.0, which is band 868 - so this is the same sweep, read rather than
// restated, and a scanner of another band sweeps that band instead.
#ifndef SWEEP_F0_MHZ
#define SWEEP_F0_MHZ \
  (twinoak::bands::lowOf(twinoak::bands::kBands, twinoak::bands::kBandCount, TWINOAK_BAND))
#endif
#ifndef SWEEP_F1_MHZ
#define SWEEP_F1_MHZ \
  (twinoak::bands::highOf(twinoak::bands::kBands, twinoak::bands::kBandCount, TWINOAK_BAND))
#endif
#ifndef SWEEP_STEP_KHZ
#define SWEEP_STEP_KHZ 25.0f
#endif
#ifndef SWEEP_DWELL_MS
#define SWEEP_DWELL_MS 20
#endif
#ifndef HIST_SAMPLES_DEFAULT
#define HIST_SAMPLES_DEFAULT 2048
#endif
#ifndef AUTO_SWEEP_MIN_DEFAULT
#define AUTO_SWEEP_MIN_DEFAULT 10  // minutes between unattended sweeps; 0 = off
#endif

// ---- Reporting cadence / limits ----
#ifndef STAT_INTERVAL_MS
#define STAT_INTERVAL_MS 10000  // heartbeat "stat" line
#endif
#ifndef MON_SAMPLE_US
#define MON_SAMPLE_US 1000  // monitor/dwell RSSI sampling pace (~1 kHz)
#endif

// ---- I2C peripherals on the TwinOak boards ----
#define RTC_ADDR 0x52  // RV-3028-C7 on the RAK adapter
#ifndef I2C_CLOCK_HZ
#define I2C_CLOCK_HZ 100000  // 1k pullups through bulkhead filter: keep at/below 100 kHz
#endif

// ---- The two watchdogs ----
//
// **The hardware one** (src/roles/scanner/Wdt.h): the nRF52840's WDT, which cannot be stopped and
// which survives the soft reset into the bootloader with the same timeout - so the bootloader,
// which feeds nothing, has exactly this long to take an image. 60 s is the longest **measured**
// USB update of the largest image the scanner is replaced by, doubled and rounded up to whole
// tens: four flashes of the 414 052-byte repeater package from the bootloader of the first board
// on this bench each took **26.3 s** from the start of the DFU to the application's port coming
// back, 25.2 s of it adafruit-nrfutil's bytes at 115200 baud
// (tasks/phase-3.5/reports/03.md, *The measurement*; every flash of tools/bench logs its own
// `update_s`). It was 8 s until that measurement, and 8 s is what ended the DFU that would have
// replaced the scanner (tasks/phase-3/FOLLOWUPS.md).
//
// **The software one** (src/roles/scanner/LoopWatch.h and LoopWdt.h): the 8 s check NoiseScope
// was designed around - *a step that takes longer than the timeout is a step that should not have
// been started* - now where it belongs, in a cadence an interrupt keeps rather than in a
// peripheral a bootloader inherits. This is the number that says the loop has stopped; the one
// above is the backstop for a part so far gone that the interrupt has stopped too.
#ifndef WDT_TIMEOUT_S
#define WDT_TIMEOUT_S 60
#endif
#ifndef LOOP_WDT_TIMEOUT_MS
#define LOOP_WDT_TIMEOUT_MS 8000  // 0 turns the loop check off and leaves the hardware one alone
#endif
