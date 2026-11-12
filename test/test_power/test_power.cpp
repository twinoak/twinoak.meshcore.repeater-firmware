// src/common/power/: the INA3221's arithmetic, the two readers, the battery thresholds, the Cayenne-LPP
// and the preferences record - all of it host-portable and all of it against fakes
// (tasks/phase-1/06-power-and-environment.md, "Tests (native)").
//
// What is checked, in the order that task asks for it:
//   - the conversion against hand-calculated register values for each channel: full scale, zero,
//     one LSB, a negative battery current, and the figures of the hardware repository's
//     CURRENT-SENSE.md section 3 (0.4, 2 and 0.8 mA per LSB);
//   - the reader against a fake bus: a channel that fails goes invalid and comes back, a device
//     that goes away is left alone and found again, and a transaction that takes too long does
//     not hold the schedule - and, since p3.10-01, that a sweep happens once per averaging
//     window of the device's own, found through the conversion-ready flag;
//   - the totals (src/common/power/PowerTotals.h): an hour at a steady current, the battery's two
//     directions apart, a burst in its own interval, the three kinds of gap, and a year at full
//     scale by arithmetic on the types;
//   - the thresholds: a dip shorter than the minimum time raises nothing; low, then critical;
//     recovery needs the hysteresis; and nothing re-raised inside the hour, through the real
//     attention machine of src/common/link/attention/;
//   - the LPP of LPP_GET equals the LPP the telemetry path builds, for the same fake readings -
//     and both equal bytes computed by hand from CayenneLPP's own arithmetic;
//   - and POWER_GET's and ENV_GET's own bodies through src/common/power/LinkPower.h. Their *frames*,
//     byte for byte against the fixture, are test/test_link_session's.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "common/link/attention/Attention.h"
#include "common/link/codec/Numbers.h"
#include "common/power/BatteryWatch.h"
#include "common/power/Environment.h"
#include "common/power/Ina3221.h"
#include "common/power/LinkPower.h"
#include "common/power/PowerReader.h"
#include "common/power/PowerTotals.h"
#include "common/power/Prefs.h"
#include "common/power/Telemetry.h"

namespace {

using namespace twinoak::power;  // NOLINT: a test of one namespace

std::string toHex(const uint8_t* data, size_t len) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < len; i++) {
    out += digits[data[i] >> 4];
    out += digits[data[i] & 0x0F];
  }
  return out;
}

// The register value that carries a 13-bit measurement: the value left-aligned, the low three
// bits unused (INA3221 datasheet; src/common/power/Ina3221.h's rawOf is the other direction).
uint16_t regOf(int16_t raw) { return uint16_t(uint16_t(raw) << 3); }

// ----------------------------------------------------------------------------- the fake bus

class FakeBus : public I2cBus {
 public:
  uint16_t reg[256] = {0};
  bool fail_reg[256] = {false};
  bool fail_everything = false;
  uint32_t micros_to_report = 400;  // a healthy read at 100 kHz is about 500 us

  int reads = 0;
  int ready_reads = 0;   // of those, reads of the Mask/Enable register
  int writes = 0;
  uint16_t last_written = 0;
  uint8_t last_written_reg = 0;

  FakeBus() { reg[kRegManufacturerId] = kManufacturerId; }

  // The three channels, as a board with something connected would answer.
  void setChannel(uint8_t channel_1based, int16_t shunt_raw, int16_t bus_raw) {
    reg[shuntRegOf(channel_1based)] = regOf(shunt_raw);
    reg[busRegOf(channel_1based)] = regOf(bus_raw);
  }

  // The device has finished an averaging window: bit 0 of the Mask/Enable register goes up, and
  // the read of that register below is what puts it down again (src/common/power/Ina3221.h,
  // kMaskConversionReady). That clearing is the whole reason the reader cannot read one window
  // twice, so the fake has to do it.
  void finishWindow() { reg[kRegMaskEnable] = uint16_t(reg[kRegMaskEnable] | kMaskConversionReady); }

  bool writeRegister(uint8_t address, uint8_t r, uint16_t value) override {
    (void)address;
    writes++;
    last_written_reg = r;
    last_written = value;
    return !(fail_everything || fail_reg[r]);
  }

  bool readRegister(uint8_t address, uint8_t r, uint16_t& value) override {
    (void)address;
    reads++;
    if (fail_everything || fail_reg[r]) return false;
    value = reg[r];
    if (r == kRegMaskEnable) {
      ready_reads++;
      reg[r] = uint16_t(reg[r] & uint16_t(~kMaskConversionReady));
    }
    return true;
  }

  uint32_t lastTransactionMicros() const override { return micros_to_report; }
};

// A bus whose transactions complete but take far longer than the reader's budget.
class SlowBus : public FakeBus {
 public:
  SlowBus() { micros_to_report = kTransactionBudgetUs + 1; }
};

// One averaging window of the device's, end to end: `after_ms` after the window before it the
// device sets the ready flag, the reader's next poll finds it, and the three channels are read
// one per pass (kChannelsPerPass). `now` is left at the instant the flag was seen, which is the
// instant the totals attribute the window to - so the interval they integrate is exactly
// `after_ms`. Only good for an `after_ms` of at least kReadyPollFromMs, which is every call
// below: a shorter one is the device running fast, and the reader would not be polling yet.
void finishWindow(PowerReader& reader, FakeBus& bus, uint32_t& now, uint32_t after_ms) {
  now += after_ms;
  bus.finishWindow();
  reader.loop(now);  // the poll that finds the flag, and no channel
  for (uint8_t i = 0; i < kChannels; i++) reader.loop(now);
}

// The three channels as PowerTotals sees them, all valid.
PowerReadings readingsOf(int16_t panel_ma, uint16_t panel_mv, int16_t batt_ma, uint16_t batt_mv,
                         int16_t load_ma, uint16_t load_mv) {
  PowerReadings r;
  r.ch[0].current_ma = panel_ma;
  r.ch[0].bus_mv = panel_mv;
  r.ch[1].current_ma = batt_ma;
  r.ch[1].bus_mv = batt_mv;
  r.ch[2].current_ma = load_ma;
  r.ch[2].bus_mv = load_mv;
  for (uint8_t i = 0; i < kChannels; i++) r.ch[i].valid = true;
  return r;
}

// ----------------------------------------------------------------------------- the fake part

class FakePart : public EnvSensor {
 public:
  bool present = true;
  bool read_ok = true;
  float temp_c = 21.5f;
  float rh_pct = 44.0f;
  float pressure_pa = 101325.0f;
  uint32_t micros_to_report = 1500;
  int begins = 0;
  int reads = 0;

  bool begin() override {
    begins++;
    return present;
  }
  bool read(float& t, float& h, float& p) override {
    reads++;
    t = temp_c;
    h = rh_pct;
    p = pressure_pa;
    return read_ok;
  }
  uint32_t lastTransactionMicros() const override { return micros_to_report; }
};

// ----------------------------------------------------------------------------- a fake snapshot

class FakeSource : public TelemetrySource {
 public:
  TelemetrySnapshot s;
  TelemetrySnapshot snapshot() const override { return s; }
};

// The readings every LPP test below shares, and the bytes CayenneLPP's own arithmetic produces
// for them (computed by hand from src/CayenneLPP.cpp's addField at version 1.6.1: the value
// times the type's multiplier, truncated towards zero, two's complement, most significant byte
// first).
TelemetrySnapshot sampleSnapshot() {
  TelemetrySnapshot s;
  s.power.ch[0].bus_mv = 18096;  // panel, 18.096 V
  s.power.ch[0].current_ma = 120;
  s.power.ch[0].valid = true;
  s.power.ch[1].bus_mv = 4008;  // battery, 4.008 V, charging
  s.power.ch[1].current_ma = -52;
  s.power.ch[1].valid = true;
  s.power.ch[2].bus_mv = 3984;  // load, 3.984 V
  s.power.ch[2].current_ma = 85;
  s.power.ch[2].valid = true;
  s.env.valid = kEnvTemperatureBit | kEnvHumidityBit | kEnvPressureBit;
  s.env.temp_centi = -350;  // -3.50 C, the fixture's own below-freezing case
  s.env.rh_centi = 8125;
  s.env.pressure_pa = 101325;
  s.batt_mv = 4008;
  s.mcu_temp_valid = true;
  s.mcu_temp_c = 21.25f;
  return s;
}

const char* const kBaseHex = "01740190";  // channel 1, voltage, 4.00 V (0x0190 = 400)
const char* const kOursHex =
    "02740711"  // channel 2 panel voltage, 18.09 V (1809)
    "02750078"  // channel 2 panel current, 0.120 A (120)
    "03740190"  // channel 3 battery voltage, 4.00 V
    "0375ffcc"  // channel 3 battery current, -0.052 A (-52): negative, two's complement
    "0474018e"  // channel 4 load voltage, 3.98 V (398)
    "04750055"  // channel 4 load current, 0.085 A (85)
    "0567ffdd";  // channel 5 enclosure temperature, -3.5 C (-35)
const char* const kMcuHex = "016700d4";  // channel 1, temperature, 21.25 -> 212, truncated

}  // namespace

// ============================================================================= the conversion

TEST(TheConversion, AMeasurementRegisterIsThirteenBitsLeftAlignedAndSigned) {
  EXPECT_EQ(0, rawOf(0x0000));
  EXPECT_EQ(1, rawOf(0x0008));
  EXPECT_EQ(-1, rawOf(0xFFF8));
  EXPECT_EQ(4095, rawOf(0x7FF8)) << "the largest positive shunt reading";
  EXPECT_EQ(-4096, rawOf(0x8000)) << "and the largest negative one";
  // The low three bits are not part of the value and are ignored, whatever they hold.
  EXPECT_EQ(1, rawOf(0x000F));
  EXPECT_EQ(4095, rawOf(0x7FFF));
  EXPECT_EQ(-1, rawOf(0xFFFF));
}

TEST(TheConversion, TheBusVoltageIsEightMillivoltsPerStepAndNeverNegative) {
  EXPECT_EQ(0, busMilliVolts(0));
  EXPECT_EQ(8, busMilliVolts(1)) << "one LSB";
  EXPECT_EQ(18096, busMilliVolts(2262)) << "a panel at 18.1 V";
  EXPECT_EQ(4008, busMilliVolts(501)) << "a pack at 4.0 V";
  EXPECT_EQ(32760, busMilliVolts(4095)) << "full scale: 32.76 V, which no rail here reaches";
  // A rail below ground is noise around zero, and the protocol's field is a u16.
  EXPECT_EQ(0, busMilliVolts(-1));
  EXPECT_EQ(0, busMilliVolts(-4096));
}

TEST(TheConversion, EachChannelGetsItsOwnShunt) {
  // The board's table, which is the whole reason this firmware exists: stock MeshCore applies
  // 100 mOhm to all three (CURRENT-SENSE.md section 4).
  ASSERT_EQ(100, kShuntMilliOhm[0]) << "RS2, panel";
  ASSERT_EQ(20, kShuntMilliOhm[1]) << "RS3, battery";
  ASSERT_EQ(50, kShuntMilliOhm[2]) << "RS4, load";

  // CURRENT-SENSE.md section 3's figures: 0.4, 2 and 0.8 mA per LSB. One LSB of channel 1 is
  // below a milliamp and rounds to none, which is the resolution the link reports.
  EXPECT_EQ(0, currentMilliAmps(1, 100)) << "0.4 mA";
  EXPECT_EQ(2, currentMilliAmps(5, 100)) << "five steps of 0.4 mA";
  EXPECT_EQ(2, currentMilliAmps(1, 20)) << "2 mA";
  EXPECT_EQ(1, currentMilliAmps(1, 50)) << "0.8 mA, rounded";
  EXPECT_EQ(4, currentMilliAmps(5, 50)) << "five steps of 0.8 mA";

  // Zero is zero on every channel.
  for (uint8_t i = 0; i < kChannels; i++) EXPECT_EQ(0, currentMilliAmps(0, kShuntMilliOhm[i]));

  // Full scale: 1.638 A, 8.19 A and 3.276 A (CURRENT-SENSE.md section 3's 1.64, 8.19, 3.28).
  EXPECT_EQ(1638, currentMilliAmps(4095, 100));
  EXPECT_EQ(8190, currentMilliAmps(4095, 20));
  EXPECT_EQ(3276, currentMilliAmps(4095, 50));
  EXPECT_EQ(-1638, currentMilliAmps(-4096, 100));
  EXPECT_EQ(-8192, currentMilliAmps(-4096, 20));
  EXPECT_EQ(-3277, currentMilliAmps(-4096, 50));

  // The numbers of the worked examples below, and the one that matters most: a battery that is
  // **discharging** reads negative (section 5.8).
  EXPECT_EQ(120, currentMilliAmps(300, 100)) << "a panel giving 120 mA";
  EXPECT_EQ(-52, currentMilliAmps(-26, 20)) << "a pack draining at 52 mA";
  EXPECT_EQ(52, currentMilliAmps(26, 20)) << "and charging at the same current";
  EXPECT_EQ(85, currentMilliAmps(106, 50)) << "a load drawing 85 mA";

  // Rounding is symmetric in the sign: the battery channel is the one that goes both ways.
  EXPECT_EQ(1, currentMilliAmps(3, 100));
  EXPECT_EQ(-1, currentMilliAmps(-3, 100));
  EXPECT_EQ(0, currentMilliAmps(1, 100));
  EXPECT_EQ(0, currentMilliAmps(-1, 100));

  // A table that was never filled in is not a current of zero amps dressed up as one.
  EXPECT_EQ(0, currentMilliAmps(4095, 0));
}

TEST(TheConversion, TheValidBitsAndTheBatteryVoltageComeOutOfTheReadings) {
  PowerReadings r;
  EXPECT_EQ(0, r.validBits());
  EXPECT_EQ(0, r.batteryMilliVolts()) << "nothing read yet is 0 mV, not a stale value";
  EXPECT_FALSE(r.batteryValid());

  r.ch[0].valid = true;
  EXPECT_EQ(twinoak::link::kPowerChannel1, r.validBits());
  r.ch[2].valid = true;
  EXPECT_EQ(twinoak::link::kPowerChannel1 | twinoak::link::kPowerChannel3, r.validBits());

  r.ch[1].valid = true;
  r.ch[1].bus_mv = 3912;
  EXPECT_EQ(7, r.validBits());
  EXPECT_EQ(3912, r.batteryMilliVolts());
  r.ch[1].valid = false;
  EXPECT_EQ(0, r.batteryMilliVolts()) << "a channel whose bit is clear reads 0 (section 5.8)";
}

// ============================================================================= the reader

TEST(TheReader, AHealthyDeviceIsConfiguredOnceAndSweptOncePerAveragingWindow) {
  FakeBus bus;
  bus.setChannel(1, 300, 2262);
  bus.setChannel(2, -26, 501);
  bus.setChannel(3, 106, 498);
  PowerReader reader(bus);

  uint32_t now = 1000;
  reader.begin(now);
  EXPECT_TRUE(reader.devicePresent());
  EXPECT_EQ(1, bus.writes);
  EXPECT_EQ(uint8_t(kRegConfig), bus.last_written_reg);
  EXPECT_EQ(kConfigWord, bus.last_written) << "continuous shunt+bus, AVG 128, 1.1 ms, all three";
  EXPECT_EQ(845u, kConversionCycleMs)
      << "128 x 3 x 2 x 1.1 ms, rounded up (src/common/power/Ina3221.h)";

  // Nothing at all is read until a window could have finished: not a channel, and not even the
  // ready flag (kReadyPollFromMs).
  const int configured = bus.reads;
  for (int i = 0; i < 100; i++) reader.loop(now + kReadyPollFromMs - 1);
  EXPECT_EQ(configured, bus.reads);

  // The flag, asked for and not yet up: one register, and no channel in that pass.
  reader.loop(now + kReadyPollFromMs);
  EXPECT_EQ(configured + 1, bus.reads);
  EXPECT_EQ(1, bus.ready_reads);
  EXPECT_EQ(0u, reader.sweeps());
  EXPECT_EQ(0, reader.readings().validBits());
  for (int i = 0; i < 10; i++) reader.loop(now + kReadyPollFromMs + kReadyPollMs - 1);
  EXPECT_EQ(1, bus.ready_reads) << "and it is not asked again for kReadyPollMs";

  // The device finishes the window. One channel per pass (kChannelsPerPass), so a sweep is the
  // pass that found the flag and three after it.
  bus.finishWindow();
  now += kConversionCycleMs;
  reader.loop(now);
  EXPECT_EQ(2, bus.ready_reads);
  EXPECT_EQ(0u, reader.sweeps()) << "the pass that found the flag reads no channel";
  reader.loop(now);
  EXPECT_EQ(twinoak::link::kPowerChannel1, reader.readings().validBits());
  reader.loop(now);
  reader.loop(now);
  EXPECT_EQ(1u, reader.sweeps());
  EXPECT_EQ(7, reader.readings().validBits());

  EXPECT_EQ(18096, reader.readings().ch[0].bus_mv);
  EXPECT_EQ(120, reader.readings().ch[0].current_ma);
  EXPECT_EQ(4008, reader.readings().ch[1].bus_mv);
  EXPECT_EQ(-52, reader.readings().ch[1].current_ma) << "the battery, draining";
  EXPECT_EQ(3984, reader.readings().ch[2].bus_mv);
  EXPECT_EQ(85, reader.readings().ch[2].current_ma);
  EXPECT_EQ(4008, reader.readings().batteryMilliVolts());

  // And the window is in the totals, at its own length: the interval the flag measured out, not
  // an assumed second (src/common/power/PowerTotals.h).
  EXPECT_EQ(uint64_t(kConversionCycleMs), reader.totals().millisCovered());
  EXPECT_EQ(uint64_t(85) * kConversionCycleMs, reader.totals().load().charge_uc);
  EXPECT_EQ(uint64_t(52) * kConversionCycleMs, reader.totals().batteryOut().charge_uc);
  EXPECT_EQ(uint64_t(0), reader.totals().batteryIn().charge_uc);
  EXPECT_EQ(0u, reader.totals().gaps());
  EXPECT_EQ(0u, reader.sweepsWithoutTheFlag()) << "the flag did the work, not the fallback";

  // Nothing happens again until the next window could have finished.
  const int read = bus.reads;
  for (int i = 0; i < 100; i++) reader.loop(now + kReadyPollFromMs - 1);
  EXPECT_EQ(read, bus.reads) << "no I2C between windows";
  reader.loop(now + kReadyPollFromMs);
  EXPECT_EQ(read + 1, bus.reads) << "and then one register, the flag";
}

TEST(TheReader, AWindowIsSweptOnceAndNoWindowIsSweptTwice) {
  // The one property the whole of PowerTotals.h rests on: reading the Mask/Enable register is
  // what clears the flag, so a window the reader has read cannot be read again - and a reader
  // that asks before the device has finished the next one is told no and reads nothing.
  FakeBus bus;
  bus.setChannel(1, 300, 2262);
  bus.setChannel(2, -26, 501);
  bus.setChannel(3, 106, 498);
  PowerReader reader(bus);

  uint32_t now = 1000;
  reader.begin(now);
  finishWindow(reader, bus, now, kConversionCycleMs);
  ASSERT_EQ(1u, reader.sweeps());

  const int reads = bus.reads;
  const int asked = bus.ready_reads;
  // Up to kReadyWaitMaxMs, which is where the fallback below takes over.
  for (uint32_t t = 1; t < kReadyWaitMaxMs; t += 10) reader.loop(now + t);
  EXPECT_GT(bus.ready_reads, asked) << "it did ask, more than once";
  EXPECT_EQ(bus.reads - reads, bus.ready_reads - asked) << "and read nothing but the flag";
  EXPECT_EQ(1u, reader.sweeps());
  EXPECT_EQ(uint64_t(kConversionCycleMs), reader.totals().millisCovered())
      << "a device with nothing new to say adds no time to the totals";
}

TEST(TheReader, APartThatNeverRaisesTheFlagIsStillRead) {
  // The one thing no machine without an INA3221 on it can prove is that a real INA3221 raises
  // the flag in continuous mode (src/common/power/PowerReader.h, kReadyWaitMaxMs; docs/bench.md
  // section 5 is the line that settles it). A part that never did must not take the readings
  // down with it, so after two windows of silence the reader reads the registers anyway - and
  // says so, and the totals say honestly that they do not cover that time.
  FakeBus bus;  // whose finishWindow() is never called: the flag never goes up
  bus.setChannel(1, 300, 2262);
  bus.setChannel(2, -26, 501);
  bus.setChannel(3, 106, 498);
  PowerReader reader(bus);

  uint32_t now = 1000;
  reader.begin(now);
  for (uint32_t t = 0; t < kReadyWaitMaxMs; t += kReadyPollMs) reader.loop(now + t);
  EXPECT_EQ(0u, reader.sweeps()) << "it waits for the flag first";
  EXPECT_EQ(0, reader.readings().validBits());

  now += kReadyWaitMaxMs + kReadyPollMs;
  for (int i = 0; i < 4; i++) reader.loop(now);  // the poll that gives up, then three channels
  EXPECT_EQ(1u, reader.sweeps());
  EXPECT_EQ(1u, reader.sweepsWithoutTheFlag());
  EXPECT_EQ(7, reader.readings().validBits()) << "and POWER_GET has its answers back";
  EXPECT_EQ(4008, reader.readings().batteryMilliVolts());
  EXPECT_TRUE(reader.devicePresent());
  // Past the totals' bound, so that reading covers no window anybody can name: a gap.
  EXPECT_EQ(uint64_t(0), reader.totals().millisCovered());
  EXPECT_EQ(uint64_t(0), reader.totals().load().charge_uc);
  EXPECT_EQ(1u, reader.totals().gaps());
}

TEST(TheReader, AChannelThatFailsGoesInvalidAndComesBack) {
  FakeBus bus;
  bus.setChannel(1, 300, 2262);
  bus.setChannel(2, -26, 501);
  bus.setChannel(3, 106, 498);
  PowerReader reader(bus);

  uint32_t now = 1000;
  reader.begin(now);
  finishWindow(reader, bus, now, kConversionCycleMs);
  ASSERT_EQ(7, reader.readings().validBits());

  // Channel 2's shunt register stops answering. The other two are still read: on this board they
  // are three ranges of one chip.
  bus.fail_reg[shuntRegOf(2)] = true;
  finishWindow(reader, bus, now, kConversionCycleMs);
  EXPECT_EQ(twinoak::link::kPowerChannel1 | twinoak::link::kPowerChannel3,
            reader.readings().validBits());
  EXPECT_EQ(0, reader.readings().batteryMilliVolts()) << "and the battery reads 0, not stale";
  EXPECT_TRUE(reader.devicePresent()) << "one register is not the device going away";
  EXPECT_EQ(1u, reader.failures());
  // A sweep counts for all three channels or for none, so that window is a gap: the two channels
  // that did answer add nothing either.
  EXPECT_EQ(1u, reader.totals().gaps());
  EXPECT_EQ(uint64_t(kConversionCycleMs), reader.totals().millisCovered());
  EXPECT_EQ(uint64_t(85) * kConversionCycleMs, reader.totals().load().charge_uc);

  // And back.
  bus.fail_reg[shuntRegOf(2)] = false;
  finishWindow(reader, bus, now, kConversionCycleMs);
  EXPECT_EQ(7, reader.readings().validBits());
  EXPECT_EQ(4008, reader.readings().batteryMilliVolts());
  EXPECT_EQ(uint64_t(2) * kConversionCycleMs, reader.totals().millisCovered())
      << "the window that did read is counted and the gap is not";
}

TEST(TheReader, ADeviceThatIsNotThereIsLeftAloneAndFoundWhenItComes) {
  FakeBus bus;
  bus.fail_everything = true;
  PowerReader reader(bus);

  uint32_t now = 1000;
  reader.begin(now);
  EXPECT_FALSE(reader.devicePresent());
  EXPECT_EQ(0, reader.readings().validBits());

  // Nothing is asked again until the back-off is over: a dead sensor costs the loop two
  // transactions a minute and not two a pass.
  const int reads = bus.reads;
  for (int i = 0; i < 1000; i++) reader.loop(now + 100);
  EXPECT_EQ(reads, bus.reads);

  // The B-board plugged back in.
  bus.fail_everything = false;
  bus.setChannel(1, 300, 2262);
  bus.setChannel(2, -26, 501);
  bus.setChannel(3, 106, 498);
  now += kDeviceRetryMs;
  reader.loop(now);
  EXPECT_TRUE(reader.devicePresent());
  for (int i = 0; i < 3; i++) reader.loop(now);
  EXPECT_EQ(0, reader.readings().validBits()) << "the window it has only just begun is not read";
  finishWindow(reader, bus, now, kConversionCycleMs);
  EXPECT_EQ(7, reader.readings().validBits());
  // The back-off is a gap by construction: the totals' clock is re-anchored when the device is
  // configured, so the five seconds it was away are in no total and in no second covered.
  EXPECT_EQ(uint64_t(kConversionCycleMs), reader.totals().millisCovered());
}

TEST(TheReader, ADeviceThatAnswersButIsNotAnIna3221IsNotConfigured) {
  FakeBus bus;
  bus.reg[kRegManufacturerId] = 0x1234;
  PowerReader reader(bus);
  reader.begin(1000);
  EXPECT_FALSE(reader.devicePresent());
  EXPECT_EQ(0, bus.writes) << "nothing is written into a part that is not the one we know";
}

TEST(TheReader, AStuckTransactionDoesNotHoldTheSchedule) {
  SlowBus bus;
  bus.setChannel(1, 300, 2262);
  bus.setChannel(2, -26, 501);
  bus.setChannel(3, 106, 498);
  PowerReader reader(bus);

  uint32_t now = 1000;
  reader.begin(now);
  // The probe itself answered - late - so the device is never configured and never swept.
  EXPECT_FALSE(reader.devicePresent());
  EXPECT_EQ(0, reader.readings().validBits());
  EXPECT_EQ(1u, reader.overruns());
  const int reads = bus.reads;
  for (int i = 0; i < 1000; i++) reader.loop(now + 1);
  EXPECT_EQ(reads, bus.reads) << "and the loop is not spent on it";

  // A bus that was healthy and then slows down mid-sweep: the channel goes invalid, the sweep is
  // abandoned, and the device is left alone until the back-off is over.
  FakeBus ok;
  ok.setChannel(1, 300, 2262);
  ok.setChannel(2, -26, 501);
  ok.setChannel(3, 106, 498);
  PowerReader second(ok);
  now = 5000;
  second.begin(now);
  ok.finishWindow();
  now += kConversionCycleMs;
  second.loop(now);  // the pass that finds the flag
  second.loop(now);  // channel 1
  ASSERT_EQ(twinoak::link::kPowerChannel1, second.readings().validBits());
  ok.micros_to_report = kTransactionBudgetUs + 1;
  second.loop(now);
  EXPECT_EQ(0, second.readings().validBits()) << "a bus in trouble invalidates every channel";
  EXPECT_FALSE(second.devicePresent());
  EXPECT_EQ(1u, second.overruns());
  EXPECT_EQ(uint64_t(0), second.totals().millisCovered()) << "an abandoned sweep adds nothing";
  const int after = ok.reads;
  for (int i = 0; i < 1000; i++) second.loop(now + 10);
  EXPECT_EQ(after, ok.reads);
}

TEST(TheReader, ABusThatWillNotSayWhetherItHasConvertedIsTheDeviceGoingAway) {
  // The Mask/Enable register is the device itself rather than a channel of it: a part that will
  // not answer it is a part there is nothing to read from, so this is the back-off and not a
  // missing reading (src/common/power/PowerReader.cpp says so beside the branch).
  FakeBus bus;
  bus.setChannel(1, 300, 2262);
  bus.setChannel(2, -26, 501);
  bus.setChannel(3, 106, 498);
  PowerReader reader(bus);

  uint32_t now = 1000;
  reader.begin(now);
  finishWindow(reader, bus, now, kConversionCycleMs);
  ASSERT_EQ(7, reader.readings().validBits());

  bus.fail_reg[kRegMaskEnable] = true;
  now += kConversionCycleMs;
  reader.loop(now);
  EXPECT_FALSE(reader.devicePresent());
  EXPECT_EQ(0, reader.readings().validBits());
  EXPECT_EQ(1u, reader.failures());
  const int reads = bus.reads;
  for (int i = 0; i < 1000; i++) reader.loop(now + 10);
  EXPECT_EQ(reads, bus.reads) << "and it is left alone for kDeviceRetryMs";
}

TEST(TheReader, EveryChannelFailingAtOnceIsTheDeviceGoingAway) {
  FakeBus bus;
  bus.setChannel(1, 300, 2262);
  bus.setChannel(2, -26, 501);
  bus.setChannel(3, 106, 498);
  PowerReader reader(bus);

  uint32_t now = 1000;
  reader.begin(now);
  finishWindow(reader, bus, now, kConversionCycleMs);
  ASSERT_TRUE(reader.devicePresent());

  for (uint8_t ch = 1; ch <= 3; ch++) bus.fail_reg[shuntRegOf(ch)] = true;
  finishWindow(reader, bus, now, kConversionCycleMs);
  EXPECT_FALSE(reader.devicePresent());
  EXPECT_EQ(0, reader.readings().validBits());
  EXPECT_EQ(3u, reader.failures());
  EXPECT_EQ(uint64_t(kConversionCycleMs), reader.totals().millisCovered())
      << "and that window is in no total";
}

// ============================================================================= the totals

TEST(TheTotals, ASteadyHundredMilliampsForAnHourIsAHundredMilliampHours) {
  // The acceptance criterion, with the device's own window as the step: 4260 windows of 845 ms
  // and one last one of 300 ms is an hour to the millisecond.
  PowerTotals totals;
  uint32_t now = 1000;
  totals.anchor(now);

  const PowerReadings r = readingsOf(0, 18000, 0, 4000, 100, 4000);
  for (int i = 0; i < 4260; i++) {
    now += kConversionCycleMs;
    ASSERT_TRUE(totals.note(r, now));
  }
  now += 3600000u - 4260u * kConversionCycleMs;
  ASSERT_TRUE(totals.note(r, now));

  EXPECT_EQ(3600u, totals.secondsCovered());
  EXPECT_EQ(uint64_t(3600000), totals.millisCovered());
  // 100 mA for 3 600 000 ms is 360 000 000 microcoulomb, which is 100 mAh exactly. Both are
  // exact here; the rounding of the two convenience units is a floor, so at most 1 mAh and 1 mWh
  // short, and the accumulators themselves are exact to the millisecond.
  EXPECT_EQ(uint64_t(360000000), totals.load().charge_uc);
  EXPECT_EQ(uint64_t(100), milliAmpHours(totals.load()));
  // 4000 mV x 100 mA = 400 mW for an hour: 400 mWh, 1.44e9 microjoule.
  EXPECT_EQ(uint64_t(1440000000), totals.load().energy_uj);
  EXPECT_EQ(uint64_t(400), milliWattHours(totals.load()));
  EXPECT_EQ(0u, totals.gaps());
}

TEST(TheTotals, TheBatteryKeepsItsTwoDirectionsApart) {
  // +500 mA for ten minutes and -50 mA for fifty, and neither direction has the other's. The
  // interval here is a round second so that ten minutes is ten minutes; the device's own 845 ms
  // is the step everywhere else, and the arithmetic is per-millisecond either way.
  PowerTotals totals;
  uint32_t now = 500;
  totals.anchor(now);

  const PowerReadings charging = readingsOf(0, 18000, 500, 4100, 0, 4100);
  for (int i = 0; i < 600; i++) {
    now += 1000;
    ASSERT_TRUE(totals.note(charging, now));
  }
  const PowerReadings draining = readingsOf(0, 0, -50, 3900, 0, 3900);
  for (int i = 0; i < 3000; i++) {
    now += 1000;
    ASSERT_TRUE(totals.note(draining, now));
  }

  EXPECT_EQ(3600u, totals.secondsCovered());
  // 500 mA x 600 000 ms, and 4100 mV x 500 mA x 600 000 ms = 1.23e12 nJ = 1.23e9 uJ.
  EXPECT_EQ(uint64_t(300000000), totals.batteryIn().charge_uc);
  EXPECT_EQ(uint64_t(1230000000), totals.batteryIn().energy_uj);
  EXPECT_EQ(uint64_t(83), milliAmpHours(totals.batteryIn()));
  EXPECT_EQ(uint64_t(341), milliWattHours(totals.batteryIn()));
  // 50 mA x 3 000 000 ms, and 3900 mV x 50 mA x 3 000 000 ms = 5.85e11 nJ = 5.85e8 uJ.
  EXPECT_EQ(uint64_t(150000000), totals.batteryOut().charge_uc);
  EXPECT_EQ(uint64_t(585000000), totals.batteryOut().energy_uj);
  EXPECT_EQ(uint64_t(41), milliAmpHours(totals.batteryOut()));
  EXPECT_EQ(uint64_t(162), milliWattHours(totals.batteryOut()));
  // The two channels that read zero have nothing, and neither direction has the other's.
  EXPECT_EQ(uint64_t(0), totals.panel().charge_uc);
  EXPECT_EQ(uint64_t(0), totals.panel().energy_uj);
  EXPECT_EQ(uint64_t(0), totals.load().charge_uc);
  EXPECT_EQ(uint64_t(0), totals.load().energy_uj);
}

TEST(TheTotals, ThePanelAndTheLoadCountWhatIsPositive) {
  // A negative reading on the panel or the load is noise around zero (the device's own input
  // offset is tens of microvolts, CURRENT-SENSE.md section 3.5) and adds nothing; on the battery
  // it is the pack draining, which is the whole point of having two directions.
  PowerTotals totals;
  uint32_t now = 0;
  totals.anchor(now);
  const PowerReadings negative = readingsOf(-1, 18000, -1, 4000, -1, 4000);
  now += kConversionCycleMs;
  ASSERT_TRUE(totals.note(negative, now));

  EXPECT_EQ(uint64_t(0), totals.panel().charge_uc);
  EXPECT_EQ(uint64_t(0), totals.panel().energy_uj);
  EXPECT_EQ(uint64_t(0), totals.load().charge_uc);
  EXPECT_EQ(uint64_t(0), totals.load().energy_uj);
  EXPECT_EQ(uint64_t(kConversionCycleMs), totals.batteryOut().charge_uc) << "1 mA for one window";
  EXPECT_EQ(uint64_t(0), totals.batteryIn().charge_uc);
  // It is a reading and not a gap: the time it covers is covered.
  EXPECT_EQ(uint64_t(kConversionCycleMs), totals.millisCovered());
  EXPECT_EQ(0u, totals.gaps());
}

TEST(TheTotals, ABurstIsCountedForItsOwnIntervalAndNoOther) {
  PowerTotals totals;
  uint32_t now = 1000;
  totals.anchor(now);

  const PowerReadings idle = readingsOf(0, 18000, -100, 4000, 100, 4000);
  const PowerReadings burst = readingsOf(0, 18000, -1500, 3700, 1500, 3700);

  now += kConversionCycleMs;
  ASSERT_TRUE(totals.note(idle, now));
  EXPECT_EQ(uint64_t(100) * kConversionCycleMs, totals.load().charge_uc);

  now += kConversionCycleMs;
  ASSERT_TRUE(totals.note(burst, now));
  EXPECT_EQ(uint64_t(1600) * kConversionCycleMs, totals.load().charge_uc)
      << "the burst's own window, and only it";

  now += kConversionCycleMs;
  ASSERT_TRUE(totals.note(idle, now));
  EXPECT_EQ(uint64_t(1700) * kConversionCycleMs, totals.load().charge_uc)
      << "and the window after it is its own again";
  EXPECT_EQ(uint64_t(3) * kConversionCycleMs, totals.millisCovered());
}

TEST(TheTotals, ALateSweepIsAGapAndTheOneAtTheBoundIsNot) {
  // The bound is stated in PowerTotals.h: one and a half averaging windows. Past it the average
  // in the device's registers covers part of the interval and not the whole of it, so the
  // interval is in no total at all.
  const PowerReadings r = readingsOf(0, 18000, 0, 4000, 100, 4000);

  PowerTotals at_the_bound;
  uint32_t now = 1000;
  at_the_bound.anchor(now);
  now += kMaxIntervalMs;
  EXPECT_TRUE(at_the_bound.note(r, now));
  EXPECT_EQ(uint64_t(kMaxIntervalMs), at_the_bound.millisCovered());
  EXPECT_EQ(0u, at_the_bound.gaps());

  PowerTotals late;
  now = 1000;
  late.anchor(now);
  now += kMaxIntervalMs + 1;
  EXPECT_FALSE(late.note(r, now));
  EXPECT_EQ(uint64_t(0), late.millisCovered());
  EXPECT_EQ(uint64_t(0), late.load().charge_uc);
  EXPECT_EQ(1u, late.gaps());

  // And the gap is that interval, not the next one as well.
  now += kConversionCycleMs;
  EXPECT_TRUE(late.note(r, now));
  EXPECT_EQ(uint64_t(kConversionCycleMs), late.millisCovered());
}

TEST(TheTotals, AChannelThatDidNotReadIsAGapForAllThree) {
  PowerTotals totals;
  uint32_t now = 1000;
  totals.anchor(now);

  PowerReadings r = readingsOf(300, 18000, -100, 4000, 100, 4000);
  r.ch[1].valid = false;
  now += kConversionCycleMs;
  EXPECT_FALSE(totals.note(r, now));
  EXPECT_EQ(uint64_t(0), totals.panel().charge_uc) << "the channel that did read adds nothing";
  EXPECT_EQ(uint64_t(0), totals.load().charge_uc);
  EXPECT_EQ(uint64_t(0), totals.millisCovered());
  EXPECT_EQ(1u, totals.gaps());
}

TEST(TheTotals, AnAnchorMovesTheClockAndClearsNoTotal) {
  // "Zero at start, and only then": a reconfigured device re-anchors the clock, which is what
  // keeps the back-off out of the totals, and that must not be a way to lose what has flowed.
  PowerTotals totals;
  uint32_t now = 1000;
  totals.anchor(now);
  const PowerReadings r = readingsOf(0, 18000, 0, 4000, 100, 4000);
  now += kConversionCycleMs;
  ASSERT_TRUE(totals.note(r, now));
  const uint64_t charge = totals.load().charge_uc;
  ASSERT_GT(charge, uint64_t(0));

  now += 60000;  // a minute of back-off, and then the device is configured again
  totals.anchor(now);
  EXPECT_EQ(charge, totals.load().charge_uc);
  EXPECT_EQ(uint64_t(kConversionCycleMs), totals.millisCovered());

  now += kConversionCycleMs;
  EXPECT_TRUE(totals.note(r, now)) << "and the window after it is one window, not a minute";
  EXPECT_EQ(uint64_t(2) * kConversionCycleMs, totals.millisCovered());
  EXPECT_EQ(uint64_t(200) * kConversionCycleMs, totals.load().charge_uc);
}

TEST(TheTotals, AYearAtFullScaleOverflowsNothing) {
  // By arithmetic on the types, as the task asks. Full scale on this board is 1638, 8190 and
  // 3276 mA (src/common/power/Ina3221.h) and the bus register's own full scale is 4095 x 8 mV;
  // the worst case for every accumulator is the battery channel at both.
  constexpr uint64_t kYearMs = 365ull * 24ull * 3600ull * 1000ull;  // 3.1536e10
  constexpr uint64_t kFullScaleMa = 8190;
  constexpr uint64_t kFullScaleMv = 4095 * 8;  // 32760

  // Charge, in microcoulomb: 2.58e14 a year, and a uint64 holds 1.8e19 - 71 000 years of it.
  static_assert(kFullScaleMa * kYearMs < UINT64_MAX / 70000ull,
                "a year of charge at full scale has tens of thousands of years of room");
  // Energy, in microjoule: 8.46e15 a year, which is 2 100 years of room. Nanojoules would have
  // been 8.5e18 - two years - which is why add() carries a remainder into microjoules instead.
  static_assert(kFullScaleMv * kFullScaleMa * kYearMs / 1000ull < UINT64_MAX / 2000ull,
                "a year of energy at full scale has thousands of years of room");
  // And one step's own product, in nanojoules, which is what add() computes before it divides.
  static_assert(
      kFullScaleMv * kFullScaleMa * uint64_t(kMaxIntervalMs) + 999ull < UINT64_MAX / 1000ull,
      "one interval at full scale is far inside a uint64");
  // The time covered: a year of milliseconds is nothing to a uint64 either.
  static_assert(kYearMs < UINT64_MAX / 1000000ull, "millisCovered has room for half a million years");

  // And the accumulator really does it, over many steps at full scale: no drift, and the
  // hand-computed product to the microcoulomb and the microjoule.
  PowerTotals totals;
  uint32_t now = 0;
  totals.anchor(now);
  const PowerReadings full = readingsOf(1638, 32760, 8190, 32760, 3276, 32760);
  const int steps = 100000;
  for (int i = 0; i < steps; i++) {
    now += kMaxIntervalMs;
    ASSERT_TRUE(totals.note(full, now));
  }
  const uint64_t ms = uint64_t(steps) * kMaxIntervalMs;
  EXPECT_EQ(ms, totals.millisCovered());
  EXPECT_EQ(uint64_t(8190) * ms, totals.batteryIn().charge_uc);
  EXPECT_EQ(uint64_t(32760) * 8190 * ms / 1000, totals.batteryIn().energy_uj);
  EXPECT_EQ(uint64_t(1638) * ms, totals.panel().charge_uc);
  EXPECT_EQ(uint64_t(3276) * ms, totals.load().charge_uc);
}

TEST(TheTotals, NothingBelowAMicrojouleIsLost) {
  // A step whose energy is a few nanojoules is kept as a remainder and carried, so that a year
  // of small steps is not a year of rounding down to nothing.
  PowerTotals totals;
  uint32_t now = 0;
  totals.anchor(now);
  const PowerReadings tiny = readingsOf(0, 0, 0, 0, 1, 1);  // 1 mV x 1 mA x 1 ms = 1 nJ a step
  for (int i = 0; i < 999; i++) {
    now += 1;
    ASSERT_TRUE(totals.note(tiny, now));
  }
  EXPECT_EQ(uint64_t(0), totals.load().energy_uj) << "999 nanojoules is no whole microjoule yet";
  EXPECT_EQ(uint64_t(999), totals.load().charge_uc) << "but the charge is exact at every step";
  now += 1;
  ASSERT_TRUE(totals.note(tiny, now));
  EXPECT_EQ(uint64_t(1), totals.load().energy_uj) << "and the thousandth is the microjoule";
  EXPECT_EQ(uint64_t(1000), totals.millisCovered());
}

TEST(TheTotals, TheThresholdsSeeWhatTheySaw) {
  // The battery thresholds read channel 2 out of the same cache the totals are built from, and
  // the ready-flag schedule did not change what that cache says: the watch sees every window's
  // bus voltage, in order, and a pack below 3500 mV for a minute is the low level as it was
  // before the totals existed (TheThresholds, below, is the same behaviour driven by hand).
  FakeBus bus;
  bus.setChannel(1, 0, 0);
  bus.setChannel(3, 106, 498);
  PowerReader reader(bus);
  BatteryWatch watch;

  uint32_t now = 1000;
  reader.begin(now);

  // 4.008 V, for a window: normal, and the watch has the cache's own number.
  bus.setChannel(2, -26, 501);
  finishWindow(reader, bus, now, kConversionCycleMs);
  const ChannelReading* batt = &reader.readings().ch[1];
  ASSERT_TRUE(batt->valid);
  EXPECT_EQ(kBatteryNoChange, watch.poll(batt->valid, batt->bus_mv, now));
  EXPECT_EQ(4008, watch.lastMilliVolts()) << "what the cache said, not a number of its own";

  // 3.44 V: below the default low threshold, and nothing for the first minute.
  bus.setChannel(2, -26, 430);
  const uint32_t fell_at = now;
  uint64_t covered = reader.totals().millisCovered();
  // The clock on a pending change starts at the first poll that is below the threshold, which is
  // one window after the pack fell, so the level is entered one window past kMinTimeBelowMs: 73
  // windows of 845 ms.
  for (int i = 0; i < 73; i++) {
    finishWindow(reader, bus, now, kConversionCycleMs);
    batt = &reader.readings().ch[1];
    ASSERT_TRUE(batt->valid);
    EXPECT_EQ(3440, batt->bus_mv);
    const uint8_t entered = watch.poll(batt->valid, batt->bus_mv, now);
    if (now - fell_at < kMinTimeBelowMs) {
      EXPECT_EQ(kBatteryNoChange, entered) << "at " << now << " ms, inside the minute";
    }
    covered += kConversionCycleMs;
    EXPECT_EQ(covered, reader.totals().millisCovered());
  }
  EXPECT_EQ(kBatteryLow, watch.level()) << "and then the level the pack really is at";
  EXPECT_EQ(3440, watch.lastMilliVolts());

  // And the totals of those windows are the current the cache held, to the millisecond.
  EXPECT_EQ(uint64_t(52) * covered, reader.totals().batteryOut().charge_uc);
  EXPECT_EQ(uint64_t(85) * covered, reader.totals().load().charge_uc);
  EXPECT_EQ(0u, reader.totals().gaps());
}

// ============================================================================= the environment

TEST(TheEnvironment, OneSampleBecomesTheThreeFieldsOfEnvGet) {
  const EnvReadings r = envReadingsFrom(-3.5f, 81.25f, 101325.0f);
  EXPECT_EQ(7, r.valid);
  EXPECT_EQ(-350, r.temp_centi) << "the fixture's own below-freezing case";
  EXPECT_EQ(8125, r.rh_centi);
  EXPECT_EQ(101325u, r.pressure_pa);

  // Rounded to the nearest, symmetric in the sign.
  EXPECT_EQ(2137, envReadingsFrom(21.374f, 0.0f, 0.0f).temp_centi);
  EXPECT_EQ(-2137, envReadingsFrom(-21.374f, 0.0f, 0.0f).temp_centi);
  EXPECT_EQ(0, envReadingsFrom(0.0f, 0.0f, 0.0f).temp_centi);
}

TEST(TheEnvironment, NonsenseClearsThatQuantitysBitRatherThanWrapping) {
  const float nan = std::nanf("");

  const EnvReadings all_nan = envReadingsFrom(nan, nan, nan);
  EXPECT_EQ(0, all_nan.valid);
  EXPECT_EQ(0, all_nan.temp_centi);
  EXPECT_EQ(0, all_nan.rh_centi);
  EXPECT_EQ(0u, all_nan.pressure_pa);

  // One quantity bad does not take the others with it: a part half way through a reset, or a
  // BMP280 fitted where a BME280 was expected (no humidity).
  const EnvReadings no_rh = envReadingsFrom(19.0f, nan, 99000.0f);
  EXPECT_EQ(kEnvTemperatureBit | kEnvPressureBit, no_rh.valid);
  EXPECT_EQ(1900, no_rh.temp_centi);

  // Outside the part's own range: what 0xFFFF out of every register compensates to.
  EXPECT_EQ(0, envReadingsFrom(500.0f, 50.0f, 101000.0f).valid & kEnvTemperatureBit);
  EXPECT_EQ(0, envReadingsFrom(-200.0f, 50.0f, 101000.0f).valid & kEnvTemperatureBit);
  EXPECT_EQ(0, envReadingsFrom(20.0f, 101.0f, 101000.0f).valid & kEnvHumidityBit);
  EXPECT_EQ(0, envReadingsFrom(20.0f, -1.0f, 101000.0f).valid & kEnvHumidityBit);
  EXPECT_EQ(0, envReadingsFrom(20.0f, 50.0f, 0.0f).valid & kEnvPressureBit);
  EXPECT_EQ(0, envReadingsFrom(20.0f, 50.0f, 200000.0f).valid & kEnvPressureBit);
}

TEST(TheEnvironment, ThePartIsSampledOnItsOwnScheduleAndDroppedWhenItStopsAnswering) {
  FakePart part;
  EnvReader reader(part);

  uint32_t now = 1000;
  reader.begin(now);
  EXPECT_TRUE(reader.present());
  reader.loop(now);
  EXPECT_EQ(1u, reader.samples());
  EXPECT_EQ(2150, reader.readings().temp_centi);

  for (int i = 0; i < 100; i++) reader.loop(now + kEnvSampleIntervalMs - 1);
  EXPECT_EQ(1u, reader.samples()) << "once every interval and no more";
  reader.loop(now + kEnvSampleIntervalMs);
  EXPECT_EQ(2u, reader.samples());

  // A part that stops answering is a missing reading: every bit of `valid` goes clear.
  now += 2 * kEnvSampleIntervalMs;
  part.read_ok = false;
  reader.loop(now);
  EXPECT_EQ(0, reader.readings().valid);
  EXPECT_FALSE(reader.present());
  EXPECT_EQ(1u, reader.failures());
  const int reads = part.reads;
  for (int i = 0; i < 100; i++) reader.loop(now + 1);
  EXPECT_EQ(reads, part.reads) << "and it is left alone until the back-off is over";

  part.read_ok = true;
  reader.loop(now + kEnvRetryMs);
  EXPECT_TRUE(reader.present());
  reader.loop(now + kEnvRetryMs + kEnvSampleIntervalMs);
  EXPECT_EQ(3u, reader.samples());
  EXPECT_EQ(2150, reader.readings().temp_centi);
}

TEST(TheEnvironment, ASampleThatTookTooLongIsNotAReading) {
  FakePart part;
  part.micros_to_report = kEnvTransactionBudgetUs + 1;
  EnvReader reader(part);
  reader.begin(1000);
  reader.loop(1000);
  EXPECT_EQ(0, reader.readings().valid);
  EXPECT_EQ(0u, reader.samples());
  EXPECT_EQ(1u, reader.overruns());
  EXPECT_FALSE(reader.present());
}

TEST(TheEnvironment, AUnitWithNoPartFittedSaysSo) {
  FakePart part;
  part.present = false;
  EnvReader reader(part);
  reader.begin(1000);
  EXPECT_FALSE(reader.present());
  EXPECT_EQ(0, reader.readings().valid);
  EXPECT_EQ(0, part.reads);
}

// ============================================================================= the thresholds

TEST(TheThresholds, OnlyAPairThisFirmwareAcceptsIsAccepted) {
  EXPECT_TRUE(thresholdsAcceptable(kDefaultLowMv, kDefaultCritMv));
  EXPECT_TRUE(thresholdsAcceptable(4300, 2500));
  EXPECT_TRUE(thresholdsAcceptable(3401, 3300)) << "just over the hysteresis apart";

  EXPECT_FALSE(thresholdsAcceptable(3400, 3300)) << "exactly the hysteresis apart";
  EXPECT_FALSE(thresholdsAcceptable(3300, 3300));
  EXPECT_FALSE(thresholdsAcceptable(3300, 3500)) << "the wrong way round";
  EXPECT_FALSE(thresholdsAcceptable(4301, 3300)) << "above the ceiling";
  EXPECT_FALSE(thresholdsAcceptable(3500, 2499)) << "below the floor";
  EXPECT_FALSE(thresholdsAcceptable(0, 0));

  BatteryWatch w;
  EXPECT_EQ(kDefaultLowMv, w.lowMilliVolts());
  EXPECT_EQ(kDefaultCritMv, w.critMilliVolts());
  EXPECT_FALSE(w.setThresholds(3300, 3300));
  EXPECT_EQ(kDefaultLowMv, w.lowMilliVolts()) << "and nothing changed";
  EXPECT_TRUE(w.setThresholds(3600, 3400));
  EXPECT_EQ(3600, w.lowMilliVolts());
  EXPECT_EQ(3400, w.critMilliVolts());
}

TEST(TheThresholds, ADipShorterThanTheMinimumTimeRaisesNothing) {
  BatteryWatch w;
  uint32_t now = 1000;

  // A transmit burst: the pack sags for a few seconds and comes back.
  EXPECT_EQ(kBatteryNoChange, w.poll(true, 3900, now));
  for (uint32_t t = 0; t < kMinTimeBelowMs - 1000; t += 1000) {
    EXPECT_EQ(kBatteryNoChange, w.poll(true, 3400, now + t)) << "at " << t << " ms";
  }
  EXPECT_EQ(kBatteryLow, w.pendingLevel()) << "it is on its way, but not believed yet";
  EXPECT_EQ(kBatteryNoChange, w.poll(true, 3900, now + kMinTimeBelowMs));
  EXPECT_EQ(kBatteryNormal, w.level());
  EXPECT_EQ(kBatteryNoChange, w.pendingLevel()) << "and the clock on it started again";

  // The same dip, held long enough, is the news.
  now += 100000;
  EXPECT_EQ(kBatteryNoChange, w.poll(true, 3400, now));
  EXPECT_EQ(kBatteryNoChange, w.poll(true, 3400, now + kMinTimeBelowMs - 1));
  EXPECT_EQ(kBatteryLow, w.poll(true, 3400, now + kMinTimeBelowMs));
  EXPECT_EQ(kBatteryLow, w.level());
  EXPECT_EQ(3400, w.lastMilliVolts()) << "EV_ATTENTION's detail";
}

TEST(TheThresholds, LowThenCriticalAndRecoveryNeedsTheHysteresis) {
  BatteryWatch w;
  uint32_t now = 1000;

  // Down to low.
  w.poll(true, 3400, now);
  EXPECT_EQ(kBatteryLow, w.poll(true, 3400, now + kMinTimeBelowMs));

  // Below the low threshold again raises nothing: it is the same level.
  now += kMinTimeBelowMs;
  EXPECT_EQ(kBatteryNoChange, w.poll(true, 3350, now + 2 * kMinTimeBelowMs));

  // And on to critical.
  now += 10 * kMinTimeBelowMs;
  EXPECT_EQ(kBatteryNoChange, w.poll(true, 3250, now));
  EXPECT_EQ(kBatteryCritical, w.poll(true, 3250, now + kMinTimeBelowMs));
  EXPECT_EQ(3250, w.lastMilliVolts());

  // Coming back needs the hysteresis: 3300 is not out of `critical` and 3400 is not out of
  // `low`, however long they hold.
  now += 20 * kMinTimeBelowMs;
  for (uint32_t t = 0; t < 10 * kMinTimeBelowMs; t += kMinTimeBelowMs) {
    EXPECT_EQ(kBatteryNoChange, w.poll(true, kDefaultCritMv, now + t)) << "at " << t;
  }
  EXPECT_EQ(kBatteryCritical, w.level());

  now += 20 * kMinTimeBelowMs;
  w.poll(true, uint16_t(kDefaultCritMv + kHysteresisMv), now);
  EXPECT_EQ(kBatteryLow, w.poll(true, uint16_t(kDefaultCritMv + kHysteresisMv),
                                now + kMinTimeBelowMs))
      << "critical to low, with the hysteresis";

  now += 20 * kMinTimeBelowMs;
  for (uint32_t t = 0; t < 3 * kMinTimeBelowMs; t += kMinTimeBelowMs) {
    EXPECT_EQ(kBatteryNoChange, w.poll(true, kDefaultLowMv, now + t)) << "at " << t;
  }
  now += 10 * kMinTimeBelowMs;
  w.poll(true, uint16_t(kDefaultLowMv + kHysteresisMv), now);
  EXPECT_EQ(kBatteryNormal,
            w.poll(true, uint16_t(kDefaultLowMv + kHysteresisMv), now + kMinTimeBelowMs))
      << "low to normal, with the hysteresis";
}

TEST(TheThresholds, APackThatFallsStraightThroughIsCriticalAndNotLowFirst) {
  BatteryWatch w;
  const uint32_t now = 1000;
  w.poll(true, 3100, now);
  EXPECT_EQ(kBatteryCritical, w.poll(true, 3100, now + kMinTimeBelowMs))
      << "critical is the news; low would be yesterday's";
}

TEST(TheThresholds, AnInvalidChannelIsNotALowBattery) {
  BatteryWatch w;
  uint32_t now = 1000;

  // The INA3221 unplugged: no reading is not a reading of zero volts.
  for (uint32_t t = 0; t < 10 * kMinTimeBelowMs; t += 1000) {
    EXPECT_EQ(kBatteryNoChange, w.poll(false, 0, now + t));
  }
  EXPECT_EQ(kBatteryNormal, w.level());

  // And a channel that goes away mid-dip starts the clock again when it comes back.
  now += 10 * kMinTimeBelowMs;
  w.poll(true, 3400, now);
  w.poll(false, 0, now + kMinTimeBelowMs / 2);
  EXPECT_EQ(kBatteryNoChange, w.poll(true, 3400, now + kMinTimeBelowMs));
  EXPECT_EQ(kBatteryLow, w.poll(true, 3400, now + 2 * kMinTimeBelowMs));
}

TEST(TheThresholds, NothingIsReRaisedInsideTheHour) {
  // The hourly limit is section 8's and lives in the attention machine, not here: this is the
  // pair working together, which is what the board does (src/common/power/BoardPower.cpp's loop).
  BatteryWatch w;
  twinoak::link::AttentionMachine machine;
  uint32_t now = 1000;

  w.poll(true, 3400, now);
  ASSERT_EQ(kBatteryLow, w.poll(true, 3400, now + kMinTimeBelowMs));
  now += kMinTimeBelowMs;
  EXPECT_TRUE(machine.raise(twinoak::link::kAttentionBatteryLow, w.lastMilliVolts(), now));
  EXPECT_EQ(twinoak::link::kAttentionBatteryLow, machine.pendingReason());

  // The Walter makes a request, which ends the attention.
  machine.noteRequest(twinoak::link::kMsgStatus, now + 1);
  machine.requestAnswered(now + 2);
  EXPECT_EQ(twinoak::link::kAttentionNone, machine.pendingReason());

  // The pack recovers and falls again inside the hour. The watch says so - it is a real
  // transition - and the machine refuses to spend a second LTE attach on it.
  now += 5 * kMinTimeBelowMs;
  w.poll(true, 3700, now);
  ASSERT_EQ(kBatteryNormal, w.poll(true, 3700, now + kMinTimeBelowMs));
  now += 10 * kMinTimeBelowMs;
  w.poll(true, 3400, now);
  ASSERT_EQ(kBatteryLow, w.poll(true, 3400, now + kMinTimeBelowMs));
  now += kMinTimeBelowMs;
  EXPECT_FALSE(machine.raise(twinoak::link::kAttentionBatteryLow, w.lastMilliVolts(), now));
  EXPECT_EQ(1u, machine.rateLimited());

  // Critical is a different reason and is not held back by low's hour.
  w.poll(true, 3250, now);
  ASSERT_EQ(kBatteryCritical, w.poll(true, 3250, now + kMinTimeBelowMs));
  now += kMinTimeBelowMs;
  EXPECT_TRUE(machine.raise(twinoak::link::kAttentionBatteryCritical, w.lastMilliVolts(), now));
}

// ============================================================================= the telemetry

TEST(TheTelemetry, TheFieldsAreOursOnTheChannelsThatNeverMove) {
  const TelemetrySnapshot s = sampleSnapshot();
  LppField f[kMaxLppFields];

  const size_t n = ourFields(s, kPermBase | kPermEnvironment, f, kMaxLppFields);
  ASSERT_EQ(7u, n);
  const uint8_t want_channel[] = {2, 2, 3, 3, 4, 4, 5};
  const uint8_t want_type[] = {kLppVoltage, kLppCurrent, kLppVoltage, kLppCurrent,
                               kLppVoltage, kLppCurrent, kLppTemperature};
  for (size_t i = 0; i < n; i++) {
    EXPECT_EQ(want_channel[i], f[i].channel) << "field " << i;
    EXPECT_EQ(want_type[i], f[i].type) << "field " << i;
  }
  // Chosen once and never renumbered (src/common/power/Telemetry.h; the table in docs/link.md). A
  // compile-time check, because a renumbering is a change to what MeshMaster already named and
  // not something a run should have to find.
  static_assert(kLppChannelSelf == 1, "channel 1 is upstream's own");
  static_assert(kLppChannelPanel == 2, "the panel is LPP channel 2");
  static_assert(kLppChannelBattery == 3, "the battery is LPP channel 3");
  static_assert(kLppChannelLoad == 4, "the load is LPP channel 4");
  static_assert(kLppChannelEnclosure == 5, "the enclosure is LPP channel 5");
}

TEST(TheTelemetry, OursNeedTheEnvironmentPermissionAndAnInvalidChannelIsLeftOut) {
  TelemetrySnapshot s = sampleSnapshot();
  LppField f[kMaxLppFields];

  EXPECT_EQ(0u, ourFields(s, kPermBase, f, kMaxLppFields)) << "base alone gets none of ours";
  EXPECT_EQ(0u, ourFields(s, 0, f, kMaxLppFields));
  EXPECT_EQ(0u, ourFields(s, kPermLocation, f, kMaxLppFields));
  EXPECT_EQ(7u, ourFields(s, 0xFF, f, kMaxLppFields)) << "an unknown bit is ignored";

  // A channel that could not be read is not reported as zero volts, and the channels that are
  // there keep their numbers.
  s.power.ch[1].valid = false;
  ASSERT_EQ(5u, ourFields(s, kPermEnvironment, f, kMaxLppFields));
  EXPECT_EQ(kLppChannelPanel, f[0].channel);
  EXPECT_EQ(kLppChannelLoad, f[2].channel) << "the load is still channel 4";
  EXPECT_EQ(kLppChannelEnclosure, f[4].channel);

  s.env.valid = 0;
  EXPECT_EQ(4u, ourFields(s, kPermEnvironment, f, kMaxLppFields));

  s.power.ch[0].valid = false;
  s.power.ch[2].valid = false;
  EXPECT_EQ(0u, ourFields(s, kPermEnvironment, f, kMaxLppFields));
}

TEST(TheTelemetry, TheBytesAreWhatCayenneLppWrites) {
  const TelemetrySnapshot s = sampleSnapshot();
  LppField f[kMaxLppFields];
  uint8_t out[kMeshLppCap] = {0};

  const size_t n = ourFields(s, kPermEnvironment, f, kMaxLppFields);
  const size_t len = encodeLpp(f, n, out, sizeof(out));
  EXPECT_EQ(std::string(kOursHex), toHex(out, len));

  // The whole reply: the base voltage, ours, then the MCU's own temperature - and 21.25 C comes
  // out as 212 and not 213, because CayenneLPP truncates where it could round.
  const size_t all = telemetryFields(s, kPermEnvironment, f, kMaxLppFields);
  ASSERT_EQ(n + 2, all);
  const size_t all_len = encodeLpp(f, all, out, sizeof(out));
  EXPECT_EQ(std::string(kBaseHex) + kOursHex + kMcuHex, toHex(out, all_len));
}

TEST(TheTelemetry, TheLppOfLppGetIsTheLppTheTelemetryPathBuilds) {
  // The two compositions of src/common/power/Telemetry.h, for the same readings: LPP_GET's whole list,
  // and what upstream's MyMesh::handleRequest writes around TwinOakSensors::querySensors.
  const TelemetrySnapshot s = sampleSnapshot();
  FakeSource source;
  source.s = s;
  LinkPower link(source);

  for (uint8_t mask = 0; mask < 8; mask++) {
    // The mesh path: addVoltage(1, batt), then our fields, then addTemperature(1, mcu).
    LppField parts[kMaxLppFields];
    size_t n = 0;
    parts[n].channel = kLppChannelSelf;
    parts[n].type = kLppVoltage;
    parts[n].value = float(s.batt_mv) / 1000.0f;
    n++;
    n += ourFields(s, mask, parts + n, kMaxLppFields - n);
    parts[n].channel = kLppChannelSelf;
    parts[n].type = kLppTemperature;
    parts[n].value = s.mcu_temp_c;
    n++;
    uint8_t mesh[kMeshLppCap] = {0};
    const size_t mesh_len = encodeLpp(parts, n, mesh, sizeof(mesh));

    // The link path: LPP_GET, through the real handler.
    uint8_t got[240] = {0};
    const size_t got_len = link.readLpp(mask, got, sizeof(got));

    EXPECT_EQ(toHex(mesh, mesh_len), toHex(got, got_len)) << "perm_mask 0x0" << int(mask);
  }
}

TEST(TheTelemetry, TheMcuTemperatureIsLeftOutWhenItIsNotANumber) {
  TelemetrySnapshot s = sampleSnapshot();
  s.mcu_temp_valid = false;
  LppField f[kMaxLppFields];
  const size_t n = telemetryFields(s, kPermEnvironment, f, kMaxLppFields);
  uint8_t out[kMeshLppCap] = {0};
  const size_t len = encodeLpp(f, n, out, sizeof(out));
  EXPECT_EQ(std::string(kBaseHex) + kOursHex, toHex(out, len));
}

TEST(TheTelemetry, AFieldThatWouldNotFitIsDroppedAsTheLibraryDropsIt) {
  const TelemetrySnapshot s = sampleSnapshot();
  LppField f[kMaxLppFields];
  const size_t n = telemetryFields(s, kPermEnvironment, f, kMaxLppFields);
  uint8_t out[64] = {0};

  // Every field this firmware writes is four bytes, so a cap of five holds exactly one.
  EXPECT_EQ(4u, encodeLpp(f, n, out, 5));
  EXPECT_EQ(std::string(kBaseHex), toHex(out, 4));
  EXPECT_EQ(0u, encodeLpp(f, n, out, 3));

  // A type nothing here knows writes nothing at all - which is CayenneLPP's isType() gate, and
  // the reason the enclosure's humidity and pressure are on the box link and not in the LPP.
  LppField unknown;
  unknown.channel = 6;
  unknown.type = 104;  // LPP_RELATIVE_HUMIDITY, which CayenneLPP 1.6.1 refuses
  unknown.value = 44.0f;
  EXPECT_EQ(0u, encodeLpp(&unknown, 1, out, sizeof(out)));
}

// ============================================================================= POWER_GET, ENV_GET

TEST(TheLinkHandlers, PowerGetIsTheCacheAndEveryInvalidChannelReadsZero) {
  FakeSource source;
  source.s = sampleSnapshot();
  LinkPower link(source);

  twinoak::link::PowerResponse out;
  ASSERT_TRUE(link.readPower(out));
  EXPECT_EQ(7, out.valid);
  EXPECT_EQ(18096, out.channels[0].bus_mv);
  EXPECT_EQ(120, out.channels[0].current_ma);
  EXPECT_EQ(4008, out.channels[1].bus_mv);
  EXPECT_EQ(-52, out.channels[1].current_ma);
  EXPECT_EQ(3984, out.channels[2].bus_mv);
  EXPECT_EQ(85, out.channels[2].current_ma);

  // The bench's own case: the INA3221's bus unplugged. `valid` is 0 and every number is 0 - a
  // stale reading is never sent with its bit down.
  source.s.power.ch[0].valid = false;
  source.s.power.ch[1].valid = false;
  source.s.power.ch[2].valid = false;
  ASSERT_TRUE(link.readPower(out));
  EXPECT_EQ(0, out.valid);
  for (uint8_t i = 0; i < kChannels; i++) {
    EXPECT_EQ(0, out.channels[i].bus_mv) << "channel " << int(i + 1);
    EXPECT_EQ(0, out.channels[i].current_ma) << "channel " << int(i + 1);
  }
}

TEST(TheLinkHandlers, EnvGetIsTheCacheAndAUnitWithNoPartSaysValidZero) {
  FakeSource source;
  source.s = sampleSnapshot();
  LinkPower link(source);

  twinoak::link::EnvResponse out;
  ASSERT_TRUE(link.readEnv(out));
  EXPECT_EQ(7, out.valid);
  EXPECT_EQ(-350, out.temp_centi);
  EXPECT_EQ(8125, out.rh_centi);
  EXPECT_EQ(101325u, out.pressure_pa);

  source.s.env = EnvReadings();
  ASSERT_TRUE(link.readEnv(out));
  EXPECT_EQ(0, out.valid);
  EXPECT_EQ(0, out.temp_centi);
  EXPECT_EQ(0, out.rh_centi);
  EXPECT_EQ(0u, out.pressure_pa);
}

// ============================================================================= the preferences

TEST(ThePreferences, TheRecordSurvivesARestartAndNothingElseIsBelieved) {
  SavedPrefs in;
  in.low_mv = 3450;
  in.crit_mv = 3250;
  uint8_t bytes[kSavedPrefsBytes] = {0};
  encodeSavedPrefs(in, bytes);

  SavedPrefs out;
  ASSERT_TRUE(decodeSavedPrefs(bytes, sizeof(bytes), out));
  EXPECT_EQ(3450, out.low_mv);
  EXPECT_EQ(3250, out.crit_mv);

  // A short read, a zeroed file, a file of ones, another firmware's record, and one flipped byte.
  EXPECT_FALSE(decodeSavedPrefs(bytes, sizeof(bytes) - 1, out));
  EXPECT_FALSE(decodeSavedPrefs(nullptr, sizeof(bytes), out));
  const uint8_t zeroes[kSavedPrefsBytes] = {0};
  EXPECT_FALSE(decodeSavedPrefs(zeroes, sizeof(zeroes), out));
  uint8_t ones[kSavedPrefsBytes];
  memset(ones, 0xFF, sizeof(ones));
  EXPECT_FALSE(decodeSavedPrefs(ones, sizeof(ones), out));
  for (size_t i = 0; i < kSavedPrefsBytes; i++) {
    uint8_t flipped[kSavedPrefsBytes];
    memcpy(flipped, bytes, sizeof(flipped));
    flipped[i] = uint8_t(flipped[i] ^ 0x40);
    EXPECT_FALSE(decodeSavedPrefs(flipped, sizeof(flipped), out)) << "byte " << i;
  }

  // And a pair a firmware with a wider window once wrote: refused, so that a file cannot put
  // this node outside what it accepts today.
  SavedPrefs wide;
  wide.low_mv = 4400;
  wide.crit_mv = 2000;
  encodeSavedPrefs(wide, bytes);
  EXPECT_FALSE(decodeSavedPrefs(bytes, sizeof(bytes), out));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
