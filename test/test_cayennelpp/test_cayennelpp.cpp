// The Cayenne-LPP library itself, linked and run on the host, against the mirror of it in
// src/common/power/Telemetry.cpp (p3.12-01).
//
// Why this suite exists. `tasks/phase-1/reports/06.md` said CayenneLPP 1.6.1's `isType()` gate
// does not list relative humidity, barometric pressure or power, so that `addRelativeHumidity`
// and `addBarometricPressure` write nothing and return 0 - and on that reading the enclosure's
// humidity and pressure were kept off the wire for a year. The reading was wrong. The gate lists
// every one of those types, each behind a `CAYENNE_DISABLE_*` that nothing in this build defines.
// A claim about a library is worth what the library does, so from here on it is this suite and
// not a transcription: `[env:native]` compiles the **same exact version** the board links
// (`[libs].core`, electroniccats/CayenneLPP 1.6.1), and every byte below comes out of it.
//
// What is checked:
//   - the library encodes humidity and pressure, bytes read back off its own buffer;
//   - its three numbers per type - size, multiplier, signedness - are the mirror's three numbers
//     (a runtime check where src/common/power/BoardPower.cpp's static_asserts are a board build);
//   - the mirror's bytes are the library's bytes, field by field, for every type this firmware
//     writes and for the values that are awkward: a negative, a zero, a truncation, a total above
//     2^24 steps and one at the ceiling;
//   - and the whole telemetry reply, composed as upstream's handleRequest composes it, is the
//     same bytes through the library as through the mirror - which is the one claim
//     `tasks/phase-3.10/reports/02.md` had to leave to a board build.
//
// The library is **not** what answers a request: LPP_GET must perform no I2C and must not depend
// on upstream's request path, and the library puts its buffer on the heap (`Telemetry.h` says
// it). It is here as the thing the mirror is measured against.

#include <CayenneLPP.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

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

// CayenneLPP keeps isType, getTypeSize, getTypeMultiplier and getTypeSigned `protected`. They are
// the four answers the mirror copies, so the test reaches them the way C++ allows it to: a
// subclass. Nothing of the library is edited.
class Lpp : public CayenneLPP {
 public:
  explicit Lpp(uint8_t size) : CayenneLPP(size) {}
  using CayenneLPP::getTypeMultiplier;
  using CayenneLPP::getTypeSigned;
  using CayenneLPP::getTypeSize;
  using CayenneLPP::isType;

  // getBuffer() and getSize() are not const in the library, so neither is this.
  std::string hex() { return toHex(getBuffer(), getSize()); }
};

// The one place the two writers are driven from the same list: this is
// src/common/power/BoardPower.cpp's `appendOurFields` switch, transcribed, and the only reason it
// is transcribed is that the file it lives in needs a board.
void addThroughLibrary(CayenneLPP& lpp, const LppField& f) {
  switch (f.type) {
    case kLppVoltage:
      lpp.addVoltage(f.channel, f.value);
      break;
    case kLppCurrent:
      lpp.addCurrent(f.channel, f.value);
      break;
    case kLppTemperature:
      lpp.addTemperature(f.channel, f.value);
      break;
    case kLppRelativeHumidity:
      lpp.addRelativeHumidity(f.channel, f.value);
      break;
    case kLppBarometricPressure:
      lpp.addBarometricPressure(f.channel, f.value);
      break;
    case kLppGenericSensor:
      lpp.addGenericSensor(f.channel, f.value);
      break;
    default:
      break;
  }
}

LppField fieldOf(uint8_t channel, uint8_t type, float value) {
  LppField f;
  f.channel = channel;
  f.type = type;
  f.value = value;
  return f;
}

// The mirror's bytes for one field.
std::string mirrorHex(const LppField& f) {
  uint8_t out[kMeshLppCap] = {0};
  return toHex(out, encodeLpp(&f, 1, out, sizeof(out)));
}

// The library's bytes for the same field.
std::string libraryHex(const LppField& f) {
  Lpp lpp(kMeshLppCap);
  lpp.reset();
  addThroughLibrary(lpp, f);
  return lpp.hex();
}

// The unit's readings, as the two readers would have left them - the fixture of test_power, so
// that the bytes in the two suites are the bytes of one snapshot.
TelemetrySnapshot sampleSnapshot() {
  TelemetrySnapshot s;
  s.power.ch[0].bus_mv = 18096;  // panel, 18.096 V
  s.power.ch[0].current_ma = 120;
  s.power.ch[0].valid = true;
  s.power.ch[1].bus_mv = 4008;  // battery, 4.008 V, discharging
  s.power.ch[1].current_ma = -52;
  s.power.ch[1].valid = true;
  s.power.ch[2].bus_mv = 3984;  // load, 3.984 V
  s.power.ch[2].current_ma = 85;
  s.power.ch[2].valid = true;
  s.env.valid = uint8_t(kEnvTemperatureBit | kEnvHumidityBit | kEnvPressureBit);
  s.env.temp_centi = -350;
  s.env.rh_centi = 8125;
  s.env.pressure_pa = 101325;
  s.batt_mv = 4008;
  s.mcu_temp_valid = true;
  s.mcu_temp_c = 21.25f;
  s.totals_counted = true;
  s.totals[PowerTotals::kPanel].charge_uc = 12345ull * kChargeStepMicroCoulomb;
  s.totals[PowerTotals::kPanel].energy_uj = 67890ull * kEnergyStepMicroJoule;
  s.totals[PowerTotals::kBatteryIn].charge_uc = 100ull * kChargeStepMicroCoulomb;
  s.totals[PowerTotals::kBatteryIn].energy_uj = 4200ull * kEnergyStepMicroJoule;
  s.totals[PowerTotals::kBatteryOut].charge_uc = 23456ull * kChargeStepMicroCoulomb;
  // One at the ceiling and one above 2^24 steps: the two values where the float the library takes
  // stops being exact, and so the two the mirror could differ over if it did its own arithmetic.
  s.totals[PowerTotals::kBatteryOut].energy_uj =
      uint64_t(kTotalCeiling) * kEnergyStepMicroJoule + 1ull;
  s.totals[PowerTotals::kLoad].charge_uc =
      ((uint64_t(1) << 24) + 3ull) * kChargeStepMicroCoulomb;
  s.totals[PowerTotals::kLoad].energy_uj = 123456ull * kEnergyStepMicroJoule;
  s.totals_covered_ms = 3600ull * kCoveredStepMillis + kCoveredStepMillis - 1;
  return s;
}

}  // namespace

// ============================================================================= the gate

TEST(TheLibrary, EncodesRelativeHumidityAndBarometricPressure) {
  // The claim phase 1's report 06 got wrong, settled by the library: both types are in the gate,
  // both add* methods write, and these are the bytes they write for the fixture's own readings
  // (81.25 %RH and 101 325 Pa, which is 1013.25 hPa).
  Lpp lpp(kMeshLppCap);
  EXPECT_TRUE(lpp.isType(LPP_RELATIVE_HUMIDITY));
  EXPECT_TRUE(lpp.isType(LPP_BAROMETRIC_PRESSURE));
  EXPECT_TRUE(lpp.isType(LPP_POWER)) << "the third type report 06 named, also in the gate";

  lpp.reset();
  EXPECT_EQ(3u, lpp.addRelativeHumidity(kLppChannelEnclosure, 81.25f))
      << "three bytes written, not nothing";
  EXPECT_EQ(7u, lpp.addBarometricPressure(kLppChannelEnclosure, 1013.25f))
      << "and four more after them";
  EXPECT_EQ(std::string("0568a205732794"), lpp.hex());
  EXPECT_EQ(7u, lpp.getSize());
  EXPECT_EQ(LPP_ERROR_OK, lpp.getError());
}

TEST(TheLibrary, ItsThreeNumbersPerTypeAreTheMirrorsThreeNumbers) {
  // src/common/power/BoardPower.cpp holds these against the library's macros in the board build;
  // here they are held against the library's own three functions, which is what addField reads.
  Lpp lpp(kMeshLppCap);
  const struct {
    uint8_t type;
    uint8_t size;
    uint32_t multiplier;
    bool is_signed;
  } kWant[] = {
      {kLppGenericSensor, 4, 1, false},      {kLppTemperature, 2, 10, true},
      {kLppRelativeHumidity, 1, 2, false},   {kLppBarometricPressure, 2, 10, false},
      {kLppVoltage, 2, 100, true},           {kLppCurrent, 2, 1000, true},
  };
  for (const auto& w : kWant) {
    EXPECT_TRUE(lpp.isType(w.type)) << "type " << int(w.type);
    EXPECT_EQ(w.size, lpp.getTypeSize(w.type)) << "type " << int(w.type);
    EXPECT_EQ(w.multiplier, lpp.getTypeMultiplier(w.type)) << "type " << int(w.type);
    EXPECT_EQ(w.is_signed, lpp.getTypeSigned(w.type)) << "type " << int(w.type);
  }
}

// ============================================================================= the mirror

TEST(TheMirror, WritesWhatTheLibraryWritesFieldByField) {
  const LppField kCases[] = {
      // the enclosure, all three of it
      fieldOf(kLppChannelEnclosure, kLppTemperature, -3.5f),
      fieldOf(kLppChannelEnclosure, kLppTemperature, 21.25f),   // truncated, not rounded
      fieldOf(kLppChannelEnclosure, kLppRelativeHumidity, 81.25f),
      fieldOf(kLppChannelEnclosure, kLppRelativeHumidity, 0.0f),
      fieldOf(kLppChannelEnclosure, kLppRelativeHumidity, 100.0f),
      fieldOf(kLppChannelEnclosure, kLppRelativeHumidity, 49.9f),
      fieldOf(kLppChannelEnclosure, kLppBarometricPressure, 1013.25f),
      fieldOf(kLppChannelEnclosure, kLppBarometricPressure, 300.0f),
      fieldOf(kLppChannelEnclosure, kLppBarometricPressure, 1100.0f),
      // the three power channels
      fieldOf(kLppChannelPanel, kLppVoltage, 18.096f),
      fieldOf(kLppChannelBattery, kLppCurrent, -0.052f),  // the signed case
      fieldOf(kLppChannelLoad, kLppCurrent, 0.0f),
      // the totals, including the two values a float cannot hold exactly
      fieldOf(kLppChannelPanelCharge, kLppGenericSensor, 0.0f),
      fieldOf(kLppChannelPanelCharge, kLppGenericSensor, 12345.0f),
      fieldOf(kLppChannelLoadCharge, kLppGenericSensor, float((uint64_t(1) << 24) + 3ull)),
      fieldOf(kLppChannelBatteryEnergyOut, kLppGenericSensor, float(kTotalCeiling)),
  };
  for (const LppField& f : kCases) {
    const std::string want = libraryHex(f);
    EXPECT_FALSE(want.empty()) << "the library refused channel " << int(f.channel) << " type "
                               << int(f.type);
    EXPECT_EQ(want, mirrorHex(f)) << "channel " << int(f.channel) << " type " << int(f.type)
                                  << " value " << f.value;
  }
}

TEST(TheMirror, WritesTheWholeTelemetryReplyTheLibraryWould) {
  // The reply upstream's MyMesh::handleRequest builds, both ways, for every perm_mask: the
  // library as TwinOakSensors::querySensors drives it, and the mirror as LPP_GET drives it. This
  // is what tasks/phase-3.10/reports/02.md could only assert in a board build.
  const TelemetrySnapshot s = sampleSnapshot();
  for (uint8_t mask = 0; mask < 8; mask++) {
    LppField fields[kMaxLppFields];
    const size_t n = telemetryFields(s, mask, fields, kMaxLppFields);

    Lpp lpp(kMeshLppCap);
    lpp.reset();
    for (size_t i = 0; i < n; i++) addThroughLibrary(lpp, fields[i]);

    uint8_t mirror[kMeshLppCap] = {0};
    const size_t len = encodeLpp(fields, n, mirror, kMeshLppCap);

    EXPECT_EQ(lpp.hex(), toHex(mirror, len)) << "perm_mask 0x0" << int(mask);
    EXPECT_EQ(LPP_ERROR_OK, lpp.getError()) << "perm_mask 0x0" << int(mask);
  }
}

TEST(TheMirror, DropsTheFieldsThatWouldNotFitWhereTheLibraryDropsThem) {
  // `cap` is the library's `_maxsize`, and its overflow check is per field and does not stop the
  // walk - so with fields of four different sizes the two writers could disagree about *which*
  // fields a small buffer holds. The caps below are the awkward ones: three bytes, which only the
  // one-byte humidity fits in; thirty-six, which was the whole of v0.1.2's reply; and the cap of
  // the mesh itself, where nothing is dropped.
  const TelemetrySnapshot s = sampleSnapshot();
  LppField fields[kMaxLppFields];
  const size_t n = telemetryFields(s, kPermBase | kPermEnvironment, fields, kMaxLppFields);

  for (uint8_t cap : {uint8_t(2), uint8_t(3), uint8_t(4), uint8_t(5), uint8_t(35), uint8_t(36),
                      uint8_t(39), uint8_t(96), uint8_t(97), uint8_t(kMeshLppCap)}) {
    Lpp lpp(cap);
    lpp.reset();
    for (size_t i = 0; i < n; i++) addThroughLibrary(lpp, fields[i]);

    uint8_t mirror[kMeshLppCap] = {0};
    const size_t len = encodeLpp(fields, n, mirror, cap);

    EXPECT_EQ(lpp.hex(), toHex(mirror, len)) << "cap " << int(cap);
  }
}

TEST(TheMirror, TheWholeReplyIsNinetySevenBytesThroughTheLibraryToo) {
  // The number p3.12-01's report turns into a frame and an airtime, counted by the library.
  const TelemetrySnapshot s = sampleSnapshot();
  LppField fields[kMaxLppFields];
  const size_t n = telemetryFields(s, kPermBase | kPermEnvironment, fields, kMaxLppFields);
  ASSERT_EQ(kMaxLppFields, n) << "every field this firmware can send";

  Lpp lpp(kMeshLppCap);
  lpp.reset();
  for (size_t i = 0; i < n; i++) addThroughLibrary(lpp, fields[i]);
  EXPECT_EQ(97u, lpp.getSize());
  EXPECT_EQ(LPP_ERROR_OK, lpp.getError()) << "nothing was dropped at the mesh's own cap of 180";
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
