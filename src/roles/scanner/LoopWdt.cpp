#include "roles/scanner/LoopWdt.h"

namespace twinoak {
namespace scanner {
namespace {

// 32768 / (4095 + 1) = 8 Hz, so the counter increments - and TICK fires - every 125 ms.
constexpr uint32_t kPrescaler = 4095u;
constexpr uint32_t kTickMs = 1000u / 8u;

constexpr uint32_t kMarkerFired = 0x57445421u;  // "WDT!"

// The marker, in RAM the startup code leaves alone. `.noinit` is placed by
// src/roles/scanner/loopwdt.ld - after `.bss`, before `.heap`, NOLOAD - which says why each of
// those three matters and asserts the first two at link time.
//
// A power-on gives RAM no value at all, so a cold boot has one chance in 2^32 of reading this as
// a watchdog reset. The word is a number no counter and no pointer would be.
__attribute__((section(".noinit"), used)) volatile uint32_t g_marker;

LoopWatch g_watch(LOOP_WDT_TIMEOUT_MS, kTickMs);

// The interrupt's whole job, out of the vector so that the vector is four lines: ask, and go.
void onTick() {
  if (!g_watch.tick()) return;
  g_marker = kMarkerFired;
  // NVIC_SystemReset()'s own __DSB() is what gets that store out of the write buffer before the
  // part goes; RESETREAS will say SREQ, and the marker is what makes `boot` say `wdt` (the
  // header).
  NVIC_SystemReset();
}

}  // namespace

void loopWdtBegin() {
  // The LFCLK is already running: FreeRTOS's own tick is RTC1 on this core, and nothing of this
  // firmware would work if it were not.
  NRF_RTC2->TASKS_STOP = 1;
  NRF_RTC2->PRESCALER = kPrescaler;
  NRF_RTC2->EVTENSET = RTC_EVTENSET_TICK_Msk;
  NRF_RTC2->INTENSET = RTC_INTENSET_TICK_Msk;
  NRF_RTC2->TASKS_CLEAR = 1;
  // Below the ports' and the radio's: this interrupt does one comparison, and on the one occasion
  // it does more the part is on its way to a reset anyway.
  NVIC_SetPriority(RTC2_IRQn, 6);
  NVIC_ClearPendingIRQ(RTC2_IRQn);
  NVIC_EnableIRQ(RTC2_IRQn);
  NRF_RTC2->TASKS_START = 1;
  // One heartbeat to begin with, so the deadline is counted from here: setup() opens the ports,
  // the radio and the I2C parts before loop() ever runs.
  g_watch.beat();
}

void loopWdtBeat() { g_watch.beat(); }

LoopWdtReason loopWdtReason() {
  const bool fired = g_marker == kMarkerFired;
  g_marker = 0u;
  return fired ? kLoopWdtFired : kLoopWdtNo;
}

uint32_t loopWdtDeadlineMs() { return g_watch.deadlineMs(); }
uint32_t loopWdtTickMs() { return g_watch.tickMs(); }

void loopWdtStall(uint32_t ms) {
  const uint32_t began = millis();
  // A spin and not a delay(): a delay() yields to the scheduler, and what this has to imitate is
  // a loop that is not coming back. Nothing in here feeds either watchdog or reads a port.
  while (millis() - began < ms) {
  }
}

}  // namespace scanner
}  // namespace twinoak

// The vector. The core ships no nrfx_rtc.c, so this is the only definition of it in the image and
// the weak one of gcc_startup_nrf52840.S is what it replaces.
extern "C" void RTC2_IRQHandler(void) {
  if (NRF_RTC2->EVENTS_TICK == 0) return;
  NRF_RTC2->EVENTS_TICK = 0;
  // An event register is cleared with a delay of up to four CPU cycles on this part; reading it
  // back is Nordic's own way of not being re-entered on an event already handled.
  (void)NRF_RTC2->EVENTS_TICK;
  twinoak::scanner::onTick();
}
