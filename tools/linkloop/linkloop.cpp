// The box link's session behind a pipe, and a benchmark of one pass of it.
//
// Why this exists: tools/linkcli.py has to be exercised before a board exists
// (tasks/phase-1/03-the-link-session.md, the last acceptance criterion). This program is the same
// portable Session the firmware runs - src/common/link/session/, byte for byte - with fixed fakes where
// the board would be, pumping stdin to stdout. `linkcli.py --pipe <this program>` then speaks the
// real protocol to the real session with no serial port and no radio in the way.
//
// It is not part of the firmware and not part of tools/verify.ps1: it needs a host compiler, and
// on Windows that is the container tasks/RUNBOOK.md section 2 names. Build and use:
//
//   g++ -std=c++17 -O2 -Wall -Wextra -I src -o out/linkloop/linkloop tools/linkloop/linkloop.cpp
//       src/common/link/codec/*.cpp src/common/link/session/*.cpp src/roles/repeater/clock/ClockKeeper.cpp   (one line)
//   python3 tools/linkcli.py --pipe out/linkloop/linkloop hello
//   out/linkloop/linkloop --bench 2000     # the worst-case pass, timed
//
// The fakes answer with the values docs/link.md quotes, so a session through the pipe looks like
// a session with a board that has what phase 1's tasks 03 to 06 and phase 2's tasks 01 to 03
// implement: caps 0x7f (CLI, POWER, ENV, MESH_CLIENT, OBSERVATIONS, ATTENTION, TIME_PERSIST).
// Six things in it are the firmware's own code and not a stub:
//
//   - the **ClockKeeper** (src/roles/repeater/clock/), with out/linkloop/clock where the board has its flash
//     file, so `time` and `time set` exercise the rules the firmware will run; one run of this
//     program is one boot, so a second run is a reset. `--emitted` starts it as a node that has
//     already put a timestamp on the air;
//   - the **AttentionMachine** (src/common/link/attention/), so the 300 ms gap, the ten sends a second
//     apart and the hourly limit are watched from a laptop with no box at all. `--wake` raises
//     one at startup (reason 4), and `sensor set walter.wake 1` through `cli` raises it the way
//     a mesh admin does;
//   - the **settings table** (src/roles/repeater/app/Settings.*), behind a `sensor list/get/set` that is a
//     miniature of upstream's own CommonCLI branch - including its 134-byte paging rule, so that
//     what `cli "sensor list"` prints here is what the console prints on the board;
//   - the **power and environment readers** (src/common/power/), driven over a fake I2C bus that answers
//     real INA3221 register values - so `power`, `env` and `lpp` go through the firmware's own
//     conversion, its own cache and its own Cayenne-LPP, and `--drop-ina` is the bench's
//     "unplug the B-board" case with no board at all;
//   - the **mesh client** (src/roles/repeater/meshclient/), the job and the five sends and the reply record,
//     over a port where nothing is ever transmitted, so a send is answered as the firmware
//     answers it and its wait ends in the silence event. `--job` begins a job at startup;
//   - the **observation ring** (src/roles/repeater/meshclient/ObservationRing.h and LinkObservations.h), so
//     `obs`, `status` and `counters` through the pipe read the firmware's own ring, its own
//     payload fitting and its own counters. `--adverts <n>` hears that many synthetic adverts at
//     startup - a ring is per-boot RAM, like a job - and `--obs-wake` turns the `obs.wake`
//     setting on so that attention reason 5 can be watched with nothing but a laptop.
//
// What cannot be here is upstream's interpreter itself, so every other command line is answered
// by the handful of canned lines below. POSIX only: the loop waits on stdin with select(), which
// is what lets it emit an attention on time while nothing is being asked of it.

#include <sys/select.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

#include "app/Settings.h"
#include "clock/ClockKeeper.h"
#include "clock/LinkClock.h"
#include "link/attention/Attention.h"
#include "link/codec/FrameWriter.h"
#include "link/codec/Numbers.h"
#include "link/session/Interfaces.h"
#include "link/session/Session.h"
#include "meshclient/Job.h"
#include "meshclient/LinkMeshClient.h"
#include "meshclient/LinkObservations.h"
#include "meshclient/ObservationRing.h"
#include "power/BatteryWatch.h"
#include "power/Environment.h"
#include "power/Ina3221.h"
#include "power/LinkPower.h"
#include "power/PowerReader.h"

using namespace twinoak::link;  // NOLINT: one namespace, and the names are the wire's

namespace {

const char* const kVersion = "0.0.0-dev";
const char* const kBoard = "rak4630-single";
const char* const kUpstream = "repeater-v1.17.1+d9296435";
const char* const kNodeName = "TwinOak linkloop";

uint32_t nowMs() {
  using namespace std::chrono;
  static const steady_clock::time_point start = steady_clock::now();
  return uint32_t(duration_cast<milliseconds>(steady_clock::now() - start).count());
}

class LoopSystem : public SystemAccess {
 public:
  int restarts = 0;
  uint8_t restarted_in = 0xFF;

  uint32_t uptimeSeconds() const override { return nowMs() / 1000u; }
  uint8_t resetReason() const override { return kResetPowerOn; }
  uint32_t bootId() const override { return 0x5EED1234; }
  bool radioUp() const override { return true; }
  bool canRestart(uint8_t mode) const override { return mode <= kRebootOtaDfu; }
  void restartNow(uint8_t mode) override {
    restarts++;
    restarted_in = mode;
    fprintf(stderr, "linkloop: restart in mode %u (a board would not come back here)\n",
            unsigned(mode));
  }
};

class LoopIdentity : public IdentityAccess {
 public:
  LoopIdentity() {
    for (uint8_t i = 0; i < kPubKeyLen; i++) key_[i] = uint8_t(0xA0 + i);
  }
  const uint8_t* publicKey() const override { return key_; }
  const char* nodeName() const override { return kNodeName; }

 private:
  uint8_t key_[kPubKeyLen];
};

// The clock's file where a board has flash: ten bytes in a real file, so that a `time set`
// through the pipe goes through the keeper's own save rules **and** survives this process. One
// run of this program is one boot of the board, so a second run is a reset - which is how the
// whole point of the clock keeper can be watched from a laptop with no board at all. Delete the
// file and the next run is a node that has never been told the time.
const char* const kClockPath = "out/linkloop/clock";

class LoopFile : public twinoak::clock::ClockFile {
 public:
  bool load(twinoak::clock::SavedTime& out) override {
    uint8_t bytes[twinoak::clock::kSavedTimeBytes] = {0};
    FILE* f = fopen(kClockPath, "rb");
    if (f == nullptr) return false;
    const size_t n = fread(bytes, 1, sizeof(bytes), f);
    fclose(f);
    if (n != sizeof(bytes)) return false;
    return twinoak::clock::decodeSavedTime(bytes, sizeof(bytes), out);
  }
  bool save(const twinoak::clock::SavedTime& in) override {
    uint8_t bytes[twinoak::clock::kSavedTimeBytes] = {0};
    twinoak::clock::encodeSavedTime(in, bytes);
    FILE* f = fopen(kClockPath, "wb");
    if (f == nullptr) return false;
    const size_t n = fwrite(bytes, 1, sizeof(bytes), f);
    fclose(f);
    return n == sizeof(bytes);
  }
};

class LoopUpstreamClock : public twinoak::clock::UpstreamClock {
 public:
  // Upstream's volatile clock: a fixed date in 2024 that advances with the millisecond clock.
  uint32_t now() const override { return base_ + nowMs() / 1000u; }
  void set(uint32_t epoch) override { base_ = epoch - nowMs() / 1000u; }

 private:
  uint32_t base_ = 1715770351;
};

bool g_emitted = false;  // --emitted, in main() below
bool g_wake = false;     // --wake
bool g_begin_job = false;  // --job: begin a job at startup, so `path` reads a route back
long g_adverts = 0;        // --adverts <n>: adverts heard at startup, so `obs` has records
bool g_obs_wake = false;   // --obs-wake: the obs.wake setting on

LoopSystem g_system;
LoopIdentity g_identity;
LoopUpstreamClock g_upstream;
LoopFile g_file;
AttentionMachine g_attention;

twinoak::clock::ClockPorts makePorts() {
  twinoak::clock::ClockPorts ports;
  ports.upstream = &g_upstream;
  ports.file = &g_file;
  // No RTC and no surviving RAM, as the first-batch boards have neither.
  return ports;
}

twinoak::clock::ClockPorts g_ports = makePorts();
twinoak::clock::ClockKeeper g_keeper(g_ports);
twinoak::clock::LinkClock g_clock(g_keeper);

// The settings, with the real table over a host of fakes - except `walter.wake`, which really
// does raise an attention on the real machine.
class LoopSettingsHost : public twinoak::SettingsHost {
 public:
  const char* fwVersion() const override {
    return "twinoak-repeater 0.0.0-dev on MeshCore repeater-v1.17.1 (d9296435)";
  }
  uint16_t linkErrors() const override { return 0; }
  bool walterEverSeen() const override { return true; }
  uint32_t walterSeenSecondsAgo() const override { return 0; }
  // The words src/roles/repeater/clock/BoardClock.cpp's stateName() uses; that function reads the board's own
  // keeper, so the mapping is repeated here over this program's.
  const char* clockStateName() const override {
    switch (g_keeper.timeState()) {
      case twinoak::clock::kRestored:
        return "restored";
      case twinoak::clock::kSet:
        return "set";
      case twinoak::clock::kRtc:
        return "rtc";
      case twinoak::clock::kUnset:
      default:
        return "unset";
    }
  }
  bool attentionPending() const override {
    return g_attention.pendingReason() != kAttentionNone;
  }
  bool raiseWalterWake() override {
    return g_attention.raise(kAttentionPersonAsked, 0, nowMs());
  }
  // Out of line, because the watch they read is declared below this class. Nothing here writes a
  // preferences file - a host build has no file system of the board's - so a `set` lasts as long
  // as this process, which is one boot.
  uint16_t batteryLowMv() const override;
  uint16_t batteryCritMv() const override;
  bool setBatteryLowMv(uint16_t mv) override;
  bool setBatteryCritMv(uint16_t mv) override;
  // Likewise out of line: the observation ring is declared below this class too, and so is the
  // mesh client whose ceiling `client.refused` counts.
  bool obsWake() const override;
  void setObsWake(bool on) override;
  uint32_t clientSendsRefused() const override;
};

LoopSettingsHost g_settings_host;
twinoak::Settings g_settings(g_settings_host);

// A stand-in for upstream's command interpreter, which no host build can have: upstream's own
// `sensor list/get/set` branch (src/helpers/CommonCLI.cpp at the pin d9296435, including its
// 134-byte cut-off) over the firmware's real settings table, a few canned lines, and the four
// commands that restart the node and never answer - which here really do end the process, so
// that `cli "reboot"` through the pipe behaves as it does on a board.
class LoopCli : public CliAccess {
 public:
  size_t runCommand(char* line, char* reply, size_t reply_cap) override {
    std::string out;
    const std::string in(line);
    if (in == "reboot" || in == "poweroff" || in == "shutdown" || in == "clkreboot") {
      fprintf(stderr, "linkloop: `%s` - a board would not answer this one\n", line);
      fflush(stdout);
      _exit(0);
    } else if (in.rfind("sensor list", 0) == 0) {
      out = sensorList(in.size() > 12 ? atoi(in.c_str() + 12) : 0);
    } else if (in.rfind("sensor get ", 0) == 0) {
      const char* value = byKey(in.substr(11));
      out = value == nullptr ? "null" : std::string("> ") + value;
    } else if (in.rfind("sensor set ", 0) == 0) {
      const std::string rest = in.substr(11);
      const size_t sp = rest.find(' ');
      const std::string key = rest.substr(0, sp);
      const std::string value = sp == std::string::npos ? "null" : rest.substr(sp + 1);
      out = g_settings.set(key.c_str(), value.c_str()) ? "ok" : "can't find custom var";
    } else if (in == "get radio") {
      out = "> 869.618,62.5,8,8";
    } else if (in == "ver") {
      out = "v1.17.1 (Build: 14 Aug 2026)";
    } else if (in == "neighbors") {
      out = "-none-";
    } else if (in == "get acl") {
      out = "";  // upstream prints the ACL on the console and answers nothing
    } else {
      out = "Unknown command";  // and so does upstream, for anything it does not know
    }
    const size_t n = out.size() < reply_cap - 1 ? out.size() : reply_cap - 1;
    memcpy(reply, out.data(), n);
    reply[n] = 0;
    return n;
  }

 private:
  static const char* byKey(const std::string& key) {
    for (int i = 0; i < twinoak::Settings::count(); i++) {
      if (key == twinoak::Settings::name(i)) return g_settings.value(i);
    }
    return nullptr;
  }

  static std::string sensorList(int start) {
    const int end = twinoak::Settings::count();
    if (start >= end) return "no custom var";
    std::string out = std::to_string(end) + " vars\n";
    int i = start;
    for (; i < end && out.size() < 134; i++) {
      out += std::string(twinoak::Settings::name(i)) + "=" + g_settings.value(i) + "\n";
    }
    if (i < end) return out + "... next:" + std::to_string(i);
    out.resize(out.size() - 1);
    return out;
  }
};

LoopCli g_cli;

// --- power and environment (src/common/power/) ---------------------------------------------------------

// A fake I2C bus that answers the registers a B-board with a panel in the sun and a pack on
// charge would: 18.1 V at 120 mA into the charger, the pack at 4.01 V taking 52 mA, and the two
// radios drawing 85 mA (the numbers tasks/phase-1/reports/06.md works through). The values are
// raw registers, so the firmware's own conversion and its own per-channel shunts are what turn
// them into the millivolts and milliamps `power` prints.
class LoopBus : public twinoak::power::I2cBus {
 public:
  bool dropped = false;  // --drop-ina: the B-board unplugged

  LoopBus() {
    reg_[twinoak::power::kRegManufacturerId] = twinoak::power::kManufacturerId;
    set(1, 300, 2262);   // panel:   +120 mA over 100 mOhm, 18.096 V
    set(2, -26, 501);    // battery:  -52 mA over  20 mOhm,  4.008 V - discharging
    set(3, 106, 498);    // load:     +85 mA over  50 mOhm,  3.984 V
  }

  void set(uint8_t channel, int16_t shunt_raw, int16_t bus_raw) {
    reg_[twinoak::power::shuntRegOf(channel)] = uint16_t(uint16_t(shunt_raw) << 3);
    reg_[twinoak::power::busRegOf(channel)] = uint16_t(uint16_t(bus_raw) << 3);
  }

  bool writeRegister(uint8_t, uint8_t, uint16_t) override { return !dropped; }
  bool readRegister(uint8_t, uint8_t reg, uint16_t& value) override {
    if (dropped) return false;
    value = reg_[reg];
    return true;
  }
  uint32_t lastTransactionMicros() const override { return 400; }

 private:
  uint16_t reg_[256] = {0};
};

// A BME280 that answers what a sealed box on a Danish mast in winter would.
class LoopPart : public twinoak::power::EnvSensor {
 public:
  bool begin() override { return true; }
  bool read(float& temp_c, float& rh_pct, float& pressure_pa) override {
    temp_c = -3.5f;
    rh_pct = 81.25f;
    pressure_pa = 101325.0f;
    return true;
  }
  uint32_t lastTransactionMicros() const override { return 1500; }
};

LoopBus g_bus;
LoopPart g_part;
twinoak::power::PowerReader g_power_reader(g_bus);
twinoak::power::EnvReader g_env_reader(g_part);
twinoak::power::BatteryWatch g_battery;

// The snapshot the three handlers answer out of - the firmware's own, with the MCU temperature
// a board would read from its own die.
class LoopTelemetry : public twinoak::power::TelemetrySource {
 public:
  twinoak::power::TelemetrySnapshot snapshot() const override {
    twinoak::power::TelemetrySnapshot s;
    s.power = g_power_reader.readings();
    s.env = g_env_reader.readings();
    s.batt_mv = s.power.batteryMilliVolts();
    s.mcu_temp_valid = true;
    s.mcu_temp_c = 21.25f;
    return s;
  }
};

LoopTelemetry g_telemetry;
twinoak::power::LinkPower g_link_power(g_telemetry);

// The mesh client's job, its sends and its reply, and the box link's view of them
// (src/roles/repeater/meshclient/), so that `job begin`, `job end`, `path`, the five sends, `reply` and
// `counters` through the pipe go through the firmware's own client - the same code a board runs.
// Where the board builds a MeshCore packet there is this port: every send "succeeds" and nothing
// goes anywhere, so a send with a timeout ends in EV_MESH_REPLY state 2 (silence) when the time
// is up - which is the event path, the deadline and REPLY_GET's fallback watched from a laptop.
class LoopMeshPort : public twinoak::meshclient::MeshPort {
 public:
  uint32_t sends = 0;

  uint32_t uniqueTime() override {
    // MeshCore's unique-time rule (RTCClock::getCurrentTimeUnique): never the same second twice.
    const uint32_t t = g_upstream.now();
    last_unique_ = t <= last_unique_ ? last_unique_ + 1 : t;
    return last_unique_;
  }
  void randomBytes(uint8_t* dest, size_t len) override {
    for (size_t i = 0; i < len; i++) dest[i] = uint8_t(rand() & 0xFF);
  }
  bool sendAnonRequest(const uint8_t*, size_t, const twinoak::meshclient::Route&) override {
    sends++;
    return true;
  }
  bool sendDatagram(uint8_t, const uint8_t*, size_t, const twinoak::meshclient::Route&) override {
    sends++;
    return true;
  }
  bool sendTrace(uint32_t, uint32_t, uint8_t, const uint8_t*, uint8_t) override {
    sends++;
    return true;
  }
  void radioCounters(twinoak::meshclient::RadioCounters& out) override {
    // The numbers docs/link.md quotes for a node that has been up a while.
    out.rx = 12345;
    out.tx = 6789 + sends;
    out.noise_floor = -112;
    out.last_rssi = -97;
    out.last_snr_q4 = 25;
  }

 private:
  uint32_t last_unique_ = 0;
};

LoopMeshPort g_mesh_port;
twinoak::meshclient::Job g_job;
twinoak::meshclient::LinkMeshClient g_mesh_client(g_job, g_mesh_port);

// The observation ring, the firmware's own (src/roles/repeater/meshclient/). Nothing here decodes an advert: a
// record is the bytes of section 7 and the name inside app_data is a stranger's, so the ring is
// filled with synthetic records by --adverts and read back by `obs`, which decodes the name on
// the laptop and only with --names (tools/linkcli.py).
twinoak::meshclient::ObservationRing g_ring;
twinoak::meshclient::LinkObservations g_observations(g_ring);

// One advert heard, as src/roles/repeater/app/TwinOakMesh.cpp builds the record from a packet: the nth node of a
// made-up neighbourhood, half of them direct neighbours with an RSSI and half two hops away with
// none, and an app_data MeshCore's AdvertDataBuilder would have written (a flags byte with the
// repeater type and a name).
void hearAdvert(long n, uint32_t now_ms) {
  twinoak::meshclient::HeardAdvert a;
  for (uint8_t i = 0; i < twinoak::meshclient::kObsPubKeyLen; i++) {
    a.pubkey[i] = uint8_t(0x11 * (n % 13) + i);
  }
  a.adv_ts = g_upstream.now() - uint32_t(60 * n);
  a.heard_ms = now_ms > uint32_t(1000 * n) ? now_ms - uint32_t(1000 * n) : 0;
  a.snr_q4 = int8_t(24 - (n % 40));
  if (n % 2 == 0) {
    a.rssi = int16_t(-80 - (n % 30));
    a.path_len = 0;  // a direct neighbour: hops 0 and an RSSI, which is the bench's marker
  } else {
    a.rssi = twinoak::meshclient::kObsRssiUnknown;
    a.path_len = twinoak::meshclient::encodePathLen(2, 2);
    a.path[0] = 0xa1;
    a.path[1] = 0xb2;
    a.path[2] = 0x7f;
    a.path[3] = 0x3a;
  }
  const char* name = "linkloop node";
  a.app_data[0] = 0x82;  // ADV_TYPE_REPEATER with "a name follows"
  size_t i = 0;
  for (; name[i] != 0 && i + 1 < twinoak::meshclient::kObsAppDataMax; i++) {
    a.app_data[i + 1] = uint8_t(name[i]);
  }
  a.app_len = uint8_t(i + 1);
  g_observations.noteAdvert(a, g_attention.walterAsleep());
}

uint16_t LoopSettingsHost::batteryLowMv() const { return g_battery.lowMilliVolts(); }
uint16_t LoopSettingsHost::batteryCritMv() const { return g_battery.critMilliVolts(); }
bool LoopSettingsHost::setBatteryLowMv(uint16_t mv) {
  return g_battery.setThresholds(mv, g_battery.critMilliVolts());
}
bool LoopSettingsHost::setBatteryCritMv(uint16_t mv) {
  return g_battery.setThresholds(g_battery.lowMilliVolts(), mv);
}
bool LoopSettingsHost::obsWake() const { return g_observations.wakeEnabled(); }
void LoopSettingsHost::setObsWake(bool on) { g_observations.setWakeEnabled(on); }
uint32_t LoopSettingsHost::clientSendsRefused() const { return g_mesh_client.refused(); }

Firmware g_firmware;
Services g_services;

void wire() {
  g_firmware.role = kRoleRepeater;
  g_firmware.slot = 0;
  g_firmware.version = kVersion;
  g_firmware.board = kBoard;
  g_firmware.upstream = kUpstream;
  g_services.firmware = &g_firmware;
  g_services.system = &g_system;
  g_services.identity = &g_identity;
  g_services.clock = &g_clock;
  g_services.cli = &g_cli;
  g_services.attention = &g_attention;
  g_services.power = &g_link_power;
  g_services.env = &g_link_power;
  g_services.lpp = &g_link_power;
  g_services.mesh = &g_mesh_client;  // => kCapMeshClient
  g_services.observations = &g_observations;  // => kCapObservations; caps is now 0x7f
}

// stdin to stdout, one byte at a time: the simplest transport that keeps the session's own rule
// (read until a request is waiting, poll once, write the whole frame). It waits on stdin for at
// most ten milliseconds at a time, so that an attention's 300 ms gap and its 1000 ms repeats are
// kept while nothing at all is being asked.
int pump() {
  // As src/roles/repeater/main.cpp does it: the clock first, then the link.
  g_keeper.begin(nowMs(), /*software_reset=*/false);
  // "Something carrying this node's clock has already gone on the air" - what src/roles/repeater/main.cpp says
  // when it has sent the boot advert. It is a flag here because a board decides it by doing
  // something this program cannot do, and because it is the only way to watch a backwards
  // TIME_SET be refused (spec/LINK-PROTOCOL.md section 5.4).
  if (g_emitted) g_keeper.noteTimestampEmitted();

  Session session(g_services);
  session.begin(nowMs());
  if (session.hasOutgoing()) {
    fwrite(session.outgoing(), 1, session.outgoingSize(), stdout);
    fflush(stdout);
    session.outgoingSent(nowMs());
  }
  // The restart attention, as src/roles/repeater/link/BoardLink.cpp raises it: only for a Walter that was
  // believed asleep, which no board can know - so here it is the --wake flag's business instead.
  if (g_wake) g_attention.raise(kAttentionPersonAsked, 0, nowMs());

  // A job is per-boot RAM, and each linkcli invocation is a fresh boot of this program, so `path`
  // would read ERR_STATE on a node that no earlier command's job survived to. --job begins one
  // here - a peer and a two-hop path of two bytes each - so that one invocation can read the
  // route back and watch STATUS's job bit, the way a board holds a job across requests in one
  // boot.
  if (g_begin_job) {
    const uint8_t key[twinoak::meshclient::kPeerKeyLen] = {
        0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07, 0x18, 0x29, 0x3a, 0x4b,
        0x5c, 0x6d, 0x7e, 0x8f, 0x90, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6,
        0x07, 0x18, 0x29, 0x3a, 0x4b, 0x5c, 0x6d, 0x7e, 0x8f, 0x90};
    const uint8_t hashes[] = {0xa1, 0xb2, 0x7f, 0x3a};
    g_job.begin(key, twinoak::meshclient::kPathHops, 2, 2, hashes, sizeof hashes, nowMs());
  }

  // A ring is per-boot RAM too, so --adverts fills one at startup the way ten minutes on a roof
  // antenna would. More than the ring holds is how a loss count and `dropped` are watched.
  if (g_obs_wake) g_observations.setWakeEnabled(true);
  for (long n = 0; n < g_adverts; n++) hearAdvert(n, nowMs());

  // As TwinOakSensors::begin() does it (src/common/board/target.cpp).
  g_power_reader.begin(nowMs());
  g_env_reader.begin(nowMs());

  for (;;) {
    // read() and not fgetc(), because select() knows nothing about a stdio buffer.
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(STDIN_FILENO, &readable);
    timeval wait;
    wait.tv_sec = 0;
    wait.tv_usec = 10000;  // 10 ms, which is finer than anything the protocol times
    if (select(STDIN_FILENO + 1, &readable, nullptr, nullptr, &wait) > 0) {
      uint8_t byte = 0;
      const ssize_t n = read(STDIN_FILENO, &byte, 1);
      if (n <= 0) return 0;  // the other end closed the pipe
      session.feedByte(byte, nowMs());
    }
    g_keeper.loop(nowMs());
    // The job's 120-second silence rule and the reply's deadline (src/roles/repeater/meshclient/LinkMeshClient.cpp),
    // once a pass as src/roles/repeater/link/BoardLink.cpp's linkLoop() does it.
    g_mesh_client.loop(nowMs());
    // As TwinOakSensors::loop() does it, once a pass: one INA3221 channel, one BME280 sample,
    // and the battery thresholds looked at with whatever channel 2 last said.
    {
      const uint32_t now = nowMs();
      g_power_reader.loop(now);
      g_env_reader.loop(now);
      const twinoak::power::ChannelReading& batt = g_power_reader.readings().ch[1];
      const uint8_t entered = g_battery.poll(batt.valid, batt.bus_mv, now);
      if (entered == twinoak::power::kBatteryLow) {
        g_attention.raise(kAttentionBatteryLow, g_battery.lastMilliVolts(), now);
      } else if (entered == twinoak::power::kBatteryCritical) {
        g_attention.raise(kAttentionBatteryCritical, g_battery.lastMilliVolts(), now);
      }
    }
    session.poll(nowMs());
    if (session.hasOutgoing()) {
      fwrite(session.outgoing(), 1, session.outgoingSize(), stdout);
      fflush(stdout);
      session.outgoingSent(nowMs());
    }
  }
}

// The worst pass this task can produce: a full 240-byte request payload read in (so the CRC runs
// over 242 bytes and COBS over 245), one handler, and the largest response this firmware builds
// framed out. HELLO is the one that does both - its request carries a str8 that can fill the
// payload, and its response is the longest one here.
std::vector<uint8_t> worstRequestFrame(uint8_t seq) {
  std::vector<uint8_t> body;
  body.push_back(kProtoMajor);
  body.push_back(kProtoMinor);
  body.push_back(kPeerBenchTool);
  body.push_back(uint8_t(kMaxPayload));
  const size_t text = kMaxPayload - 5;  // the four above, the str8 length byte, and the text
  body.push_back(uint8_t(text));
  for (size_t i = 0; i < text; i++) body.push_back(uint8_t('a' + (i % 26)));

  FrameWriter w;
  if (!w.build(kMsgHello, seq, body.data(), body.size(), false)) return std::vector<uint8_t>();
  return std::vector<uint8_t>(w.bytes(), w.bytes() + w.size());
}

int bench(long passes) {
  Session session(g_services);
  const std::vector<uint8_t> frame = worstRequestFrame(1);
  if (frame.empty()) {
    fprintf(stderr, "linkloop: could not build the worst-case frame\n");
    return 2;
  }

  // A frame per seq, built up front: the seq is inside the COBS encoding, so a new seq is a new
  // frame, and building it is not part of what is timed.
  std::vector<std::vector<uint8_t>> frames;
  frames.reserve(256);
  for (int seq = 0; seq < 256; seq++) frames.push_back(worstRequestFrame(uint8_t(seq)));

  size_t answered = 0;
  uint32_t t = 0;
  double worst_ns = 0.0;
  double total_ns = 0.0;
  for (long n = 0; n < passes; n++) {
    // A different seq every pass, so the one-entry memory never short-circuits the handler.
    const std::vector<uint8_t>& f = frames[size_t(n & 0xFF)];

    const auto started = std::chrono::steady_clock::now();
    size_t i = 0;
    while (i < f.size() && !session.requestWaiting()) session.feedByte(f[i++], t);
    session.poll(t);
    size_t out_len = 0;
    if (session.hasOutgoing()) {
      out_len = session.outgoingSize();
      session.outgoingSent(t);
    }
    const double ns =
        double(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now() - started)
                   .count());
    if (n > 0) {  // the first pass pays for the page faults
      total_ns += ns;
      if (ns > worst_ns) worst_ns = ns;
    }
    if (out_len > 0) answered++;
    t += 200;  // past the idle threshold, so every pass also builds the leading zero
  }

  printf("passes            %ld\n", passes);
  printf("answered          %zu\n", answered);
  printf("request frame     %zu bytes\n", frame.size());
  printf("mean  per pass    %.2f us\n", total_ns / 1000.0 / double(passes - 1));
  printf("worst per pass    %.2f us\n", worst_ns / 1000.0);
  return answered == size_t(passes) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  wire();
  if (argc >= 2 && strcmp(argv[1], "--bench") == 0) {
    const long passes = argc >= 3 ? strtol(argv[2], nullptr, 10) : 2000;
    return bench(passes < 2 ? 2 : passes);
  }
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--emitted") == 0) {
      g_emitted = true;
    } else if (strcmp(argv[i], "--wake") == 0) {
      g_wake = true;
    } else if (strcmp(argv[i], "--drop-ina") == 0) {
      g_bus.dropped = true;  // the bench's "unplug the B-board": `power` then answers valid 0
    } else if (strcmp(argv[i], "--job") == 0) {
      g_begin_job = true;
    } else if (strcmp(argv[i], "--obs-wake") == 0) {
      g_obs_wake = true;
    } else if (strcmp(argv[i], "--adverts") == 0 && i + 1 < argc) {
      g_adverts = strtol(argv[++i], nullptr, 10);
      if (g_adverts < 0) g_adverts = 0;
    } else {
      fprintf(stderr,
              "usage: linkloop [--bench <passes>] [--emitted] [--wake] [--drop-ina] [--job]"
              " [--adverts <n>] [--obs-wake]\n");
      return 2;
    }
  }
  return pump();
}
