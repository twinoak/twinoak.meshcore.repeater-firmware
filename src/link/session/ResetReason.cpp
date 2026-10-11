#include "ResetReason.h"

#include "../codec/Numbers.h"

namespace twinoak {
namespace link {

uint8_t resetReasonFromNrf52(uint32_t resetreas) {
  // An empty register is the one case the nRF52 states by saying nothing: a power-on reset (or a
  // brown-out) leaves every bit clear.
  if (resetreas == 0) return kResetPowerOn;
  if (resetreas & kReasDog) return kResetWatchdog;
  if (resetreas & kReasLockup) return kResetLockup;
  if (resetreas & (kReasOff | kReasLpComp | kReasNfc | kReasVbus)) return kResetSystemOff;
  if (resetreas & kReasResetPin) return kResetPin;
  if (resetreas & kReasSoftReset) return kResetSoftware;
  // DIF alone (a debugger attached and reset the core) and any bit Nordic adds later.
  return kResetUnknown;
}

}  // namespace link
}  // namespace twinoak
