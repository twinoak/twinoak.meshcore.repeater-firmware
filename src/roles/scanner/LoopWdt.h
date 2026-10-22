// The board half of the 8 s loop check: an RTC interrupt that asks LoopWatch the question, and
// the marker that tells `boot` why the part came back.
//
// Four decisions are here and nowhere else (phase 3.5's task 03):
//
//   * **RTC2.** The check has to be asked by something that does not depend on the loop, and it
//     has to be a peripheral the scanner does not already use. RTC0 is the SoftDevice's (not
//     enabled here, and still not ours), RTC1 is FreeRTOS's tick on this core
//     (cores/nRF5/freertos/portable/CMSIS/nrf52/portmacro_cmsis.h: `portNRF_RTC_REG NRF_RTC1`),
//     TIMER2's CC[0] carries the bootloader's version to the application (cores/nRF5/wiring.c),
//     and RTC2 is claimed by nothing: the core ships no nrfx_rtc.c, so `RTC2_IRQHandler` below is
//     the only definition in the image and the weak one of the startup file is what it replaces
//     (cores/nRF5/linker/gcc_startup_nrf52840.S). Nothing of src/ names an nRF timer or RTC
//     either - the RV-3028 on the adapter is an I2C part and not this one.
//   * **The TICK event, not a compare.** With PRESCALER 4095 the counter increments at 8 Hz, so
//     TICK is an interrupt every 125 ms with no compare register to re-arm and no "a compare set
//     one tick ahead may be missed" to reason about.
//   * **The marker is in RAM, not in GPREGRET.** `GPREGRET` is the Adafruit bootloader's own
//     mailbox - this firmware writes its two magic words into it for `dfu` - so a second meaning
//     for the same register is a collision waiting for a reset in the wrong order. RAM survives a
//     soft reset, and `.noinit` is RAM the startup code neither copies into nor zeroes.
//   * **The reset is `NVIC_SystemReset`**, which gives `RESETREAS.SREQ` and not `.DOG`: the
//     marker is what makes `boot` say `wdt` all the same, because both watchdogs mean the loop
//     stopped (docs/noisescope.md section 6).
//
// The deadline itself is not here: it is LoopWatch, host-portable and native-tested. This file is
// the registers, the vector and nothing else.

#pragma once

#include "roles/scanner/LoopWatch.h"
#include "roles/scanner/ScannerHeaders.h"  // Arduino, and through it nrf52840.h
#include "roles/scanner/config.h"

namespace twinoak {
namespace scanner {

// Starts the cadence. Called once from setup(), after loopWdtReason() has read the marker.
void loopWdtBegin();

// The heartbeat, from the top of loop() and from nowhere else.
void loopWdtBeat();

// Why the part came back, as far as this file knows: `kLoopWdtNo` for a reset the loop check had
// nothing to do with, `kLoopWdtFired` when the marker is the one the interrupt wrote. Reads the
// marker **and clears it**, so it answers once per boot; call it before loopWdtBegin().
enum LoopWdtReason : uint8_t { kLoopWdtNo = 0, kLoopWdtFired };
LoopWdtReason loopWdtReason();

// What the check is set to, for `info` and for the test on the bench.
uint32_t loopWdtDeadlineMs();
uint32_t loopWdtTickMs();

// Stops feeding nothing and comes back: a deliberate stall of the main loop, which is how the
// `hang` command makes the check observable on a board (docs/noisescope.md section 5). It feeds
// neither watchdog and polls no input; a stall longer than the deadline therefore ends in the
// reset and this function does not return.
void loopWdtStall(uint32_t ms);

}  // namespace scanner
}  // namespace twinoak
