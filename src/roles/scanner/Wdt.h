// The nRF52840's hardware watchdog, as NoiseScope had it
// (software/firmware/noisescope-rak4630/src/wdt.h at b798aab0 of the hardware repository).
//
// Once started it cannot be stopped or reconfigured until a reset - which is exactly what a
// diagnostic firmware on a mast wants: any hang ends in a reset, and a reset boots back into
// passive monitor mode. It is fed from the main loop, and a scan step feeds nothing: a step that
// takes longer than the timeout is a step that should not have been started
// (docs/noisescope.md, "Design notes").
//
// The repeater does not have this: upstream's own loop has no watchdog, and adding one to the
// node that must repeat for months is a decision of its own and not this role's.

#pragma once

#include "roles/scanner/ScannerHeaders.h"  // Arduino and nrf_wdt
#include "roles/scanner/config.h"

namespace twinoak {
namespace scanner {

inline void wdtBegin() {
  NRF_WDT->CONFIG = (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos) |
                    (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos);
  NRF_WDT->CRV = WDT_TIMEOUT_S * 32768UL;
  NRF_WDT->RREN = WDT_RREN_RR0_Msk;
  NRF_WDT->TASKS_START = 1;
}

inline void wdtFeed() { NRF_WDT->RR[0] = WDT_RR_RR_Reload; }

}  // namespace scanner
}  // namespace twinoak
