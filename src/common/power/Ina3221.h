// The INA3221 on connector board B: its registers, the three shunts of *this* board, and the
// arithmetic that turns a raw register into millivolts and milliamps
// (docs/ARCHITECTURE.md section 6; the hardware repository's hardware/pcb/CURRENT-SENSE.md at
// b798aab0, and NoiseScope's src/telemetry.cpp and src/config.h at the same commit).
//
// Why this file exists at all: stock MeshCore applies **one** shunt value to all three channels
// (TELEM_INA3221_SHUNT_VALUE 0.100 through setShuntResistance(i, ...),
// src/helpers/sensors/EnvironmentSensorManager.cpp:345-350 at the pin 81dbae59) and ours are
// 100, 20 and 50 milliohm. With upstream's value channel 2 reads five times low and channel 3
// twice low - which is the finding of CURRENT-SENSE.md section 4 and the reason this unit runs
// custom firmware.
//
// Host-portable: no Arduino, no MeshCore, no heap, no floating point. Every function here is a
// pure function of its arguments, which is what lets test/test_power hold them against
// hand-calculated register values.

#pragma once

#include <stdint.h>

namespace twinoak {
namespace power {

// ----------------------------------------------------------------------------- the device

// A0 tied to SDA (CURRENT-SENSE.md section 1) - which is also stock MeshCore's default address.
constexpr uint8_t kIna3221Address = 0x42;

constexpr uint8_t kChannels = 3;

// The registers this firmware touches. 16-bit, big-endian on the wire.
enum Ina3221Reg : uint8_t {
  kRegConfig = 0x00,
  kRegShuntCh1 = 0x01,  // shunt is 0x01 + 2*(ch-1), bus is that plus one
  kRegMaskEnable = 0x0F,
  kRegManufacturerId = 0xFE,
  kRegDieId = 0xFF,
};

// Bit 0 of the Mask/Enable register: **the conversion-ready flag**, set when all conversions,
// the averaging and the multiplications of one pass over the enabled channels are complete, and
// cleared by reading that register or by writing the Configuration register (INA3221 datasheet,
// "Mask/Enable Register"; the pinned Adafruit INA3221 Library 1.0.1's INA3221_CONV_READY,
// `(1UL << 0)` of `INA3221_REG_MASK_ENABLE 0x0F`, is the same two numbers read off disk).
//
// It is how PowerReader.h reads every averaging window exactly once: the flag is the device
// saying *this window is finished and the next one has begun*.
constexpr uint16_t kMaskConversionReady = 0x0001;

inline uint8_t shuntRegOf(uint8_t channel_1based) {
  return uint8_t(kRegShuntCh1 + 2 * (channel_1based - 1));
}
inline uint8_t busRegOf(uint8_t channel_1based) { return uint8_t(shuntRegOf(channel_1based) + 1); }

// 0x5449 ("TI"), the INA3221's manufacturer ID: what tells a device that answers at 0x42 from
// the INA3221 (INA3221 datasheet, register 0xFE).
constexpr uint16_t kManufacturerId = 0x5449;

// The configuration CURRENT-SENSE.md section 6.7 asks the firmware to use: "continuous
// shunt+bus, 1.1 ms conversions, AVG = 64-256, all three channels enabled".
//
//   bit 15      RST          0
//   bits 14-12  CH1/2/3 EN   1 1 1
//   bits 11-9   AVG          0b100 = 128 samples
//   bits 8-6    VBUS CT      0b100 = 1.1 ms
//   bits 5-3    VSH CT       0b100 = 1.1 ms
//   bits 2-0    MODE         0b111 = continuous, shunt and bus
//
// **0b100 is 128 samples, not 64**, and the comment here said 64 until p3.10-01: the field's
// table is 1, 4, 16, 64, 128, 256, 512, 1024 for 0b000 to 0b111, so 0b011 is 64 and 0b100 is the
// next one up (INA3221 datasheet, "Configuration Register", AVG2-AVG0; read off disk in the
// pinned Adafruit INA3221 Library 1.0.1, `INA3221_AVG_128_SAMPLES = 0b100` of `ina3221_avgmode`,
// beside `INA3221_CONVTIME_1MS = 0b100` - "1.1ms exact" - for the two conversion-time fields).
// The word itself is unchanged: this board has been averaging 128 samples since p1-06, which is
// inside the 64-256 section 6.7 allows, and only the arithmetic below was wrong.
//
// The two fields as numbers, so that nothing downstream has to restate them:
constexpr uint16_t kAveragesField = 0x4;   // AVG2-AVG0
constexpr uint32_t kAverages = 128;        // what that field means
constexpr uint16_t kConvTimeField = 0x4;   // VBUS CT and VSH CT, the same for both
constexpr uint32_t kConvTimeUs = 1100;     // what that field means

constexpr uint16_t kConfigWord =
    0x7000 | uint16_t(kAveragesField << 9) | uint16_t(kConvTimeField << 6) |
    uint16_t(kConvTimeField << 3) | 0x7u;

// How long one averaging window takes: the averages, over both measurements of all three enabled
// channels, at the conversion time each - 128 x 3 x 2 x 1.1 ms = 844.8 ms, rounded up to the
// millisecond the firmware counts in. In continuous mode the device starts the next window as it
// finishes one, so these windows tile time end to end: a reader that takes exactly one reading
// per window has every microsecond of it, which is what PowerTotals.h adds up.
constexpr uint32_t kConversionCycleMs =
    (kAverages * kChannels * 2u * kConvTimeUs + 999u) / 1000u;  // 845

// ----------------------------------------------------------------------------- the shunts

// The board's table, in milliohm, channel 1 first (CURRENT-SENSE.md section 3):
//
//   ch1  RS2  100 mOhm  panel   IN+ PANEL+, IN- VIN     positive = panel into the charger
//   ch2  RS3   20 mOhm  battery IN+ VCC,    IN- VBAT    positive = charging, negative = draining
//   ch3  RS4   50 mOhm  load    IN+ VCC,    IN- VLOAD+  positive = drawn
//
// Build-time constants, and the table is the board's: a later board of this family overrides the
// three defines rather than editing this file (docs/ARCHITECTURE.md section 8).
#ifndef TWINOAK_SHUNT_CH1_MILLIOHM
#define TWINOAK_SHUNT_CH1_MILLIOHM 100
#endif
#ifndef TWINOAK_SHUNT_CH2_MILLIOHM
#define TWINOAK_SHUNT_CH2_MILLIOHM 20
#endif
#ifndef TWINOAK_SHUNT_CH3_MILLIOHM
#define TWINOAK_SHUNT_CH3_MILLIOHM 50
#endif

extern const uint16_t kShuntMilliOhm[kChannels];

// ----------------------------------------------------------------------------- the arithmetic

// Both measurement registers are 13 bits, left-aligned in a 16-bit word with the low three bits
// unused, and two's complement: the value is the signed word shifted right by three
// (INA3221 datasheet, "Shunt-Voltage Registers" and "Bus-Voltage Registers"; NoiseScope's
// ina_read does the same shift).
inline int16_t rawOf(uint16_t reg) { return int16_t(int16_t(reg) >> 3); }

// 40 microvolt per LSB of shunt voltage, 8 millivolt per LSB of bus voltage.
constexpr int32_t kShuntMicroVoltsPerLsb = 40;
constexpr int32_t kBusMilliVoltsPerLsb = 8;

// The bus voltage of a raw reading. The rails this board measures are never negative; a negative
// reading is noise around zero or an input below ground, and 0 is the honest answer for it (the
// protocol's field is a u16, spec/LINK-PROTOCOL.md section 5.8).
uint16_t busMilliVolts(int16_t raw);

// The current of a raw shunt reading, with that channel's own shunt applied, rounded to the
// nearest milliamp and keeping the sign (positive = into IN+, which is charging on channel 2).
//
// One LSB is 40 uV / shunt: **0.4 mA on channel 1, 2 mA on channel 2 and 0.8 mA on channel 3**
// (CURRENT-SENSE.md section 3), so channel 1's single LSB rounds to zero milliamps and that is
// the resolution the link reports. Full scale is 1638, 8190 and 3276 mA.
int16_t currentMilliAmps(int16_t raw, uint16_t shunt_milliohm);

// ----------------------------------------------------------------------------- the readings

// What one channel last said, and whether it said it. A channel whose `valid` is false reads 0
// on the link, which is what section 5.8 asks for.
struct ChannelReading {
  uint16_t bus_mv = 0;
  int16_t current_ma = 0;
  bool valid = false;
};

// The last good value of all three channels. Four readers share one of these: the link's
// POWER_GET, the mesh's telemetry reply, the board's battery voltage, and the battery thresholds
// (docs/ARCHITECTURE.md section 6).
struct PowerReadings {
  ChannelReading ch[kChannels];

  // POWER_GET's `valid` byte: bit 0 channel 1, bit 1 channel 2, bit 2 channel 3.
  uint8_t validBits() const;

  // Channel 2's bus voltage - the battery - or 0 when that channel is not valid. This is what
  // TwinOakBoard::getBattMilliVolts() answers (src/common/board/target.h).
  uint16_t batteryMilliVolts() const;
  bool batteryValid() const { return ch[1].valid; }
};

}  // namespace power
}  // namespace twinoak
