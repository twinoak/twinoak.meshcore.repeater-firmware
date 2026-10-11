// src/link/session/: the whole box-link session, driven the way src/link/SerialTransport.cpp
// drives it, against fakes - and held against spec/fixtures/link/link-v1.json for every request
// this firmware answers today (HELLO, PING, STATUS, TIME_SET, TIME_GET, IDENTITY, REBOOT, CLI,
// SLEEP_NOTICE, POWER_GET, ENV_GET, LPP_GET, OBS_PULL) and for EV_BOOT and EV_ATTENTION.
//
// The fixture is read at run time from FIXTURES_DIR (platformio.ini's [env:native]). For each
// request case the suite finds the response case of the same type and seq, sets the fakes to
// return exactly the values that response carries, feeds the request's own `frame` byte by byte,
// and expects the bytes the session hands the transport to be that response's `frame` - so a
// disagreement with the Walter can only be a disagreement with the specification.
//
// What else is checked, in the order tasks/phase-1/03-the-link-session.md asks for it:
//   - the same request frame twice: one handler call, two identical responses; a different seq:
//     two calls (LINK-PROTOCOL.md section 3, the one-entry memory);
//   - an unknown type answered ERR_UNSUPPORTED with that type's response code;
//   - HELLO with a major we do not speak, and HELLO with a higher minor;
//   - two frames in one read, a frame split over three reads, noise between frames;
//   - a request that completes while the writer still holds an unsent response: neither is lost
//     and nothing blocks;
//   - TIME_SET and TIME_GET: the fixture's frames, a refusal answered kErrRange, and both
//     answered kErrUnsupported when there is no clock interface at all;
//   - the reset-reason mapping;
//   - a capability whose interface is absent is not advertised, and the field of STATUS it owns
//     reads zero;
//   - and, with the **real** observation ring behind a real session (phase 2's task 03): OBS_PULL
//     oldest first, a repeated pull that gets the same records while the ring is drained once,
//     STATUS's two observation fields and MESH_COUNTERS' three.

#include <gtest/gtest.h>

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "link/codec/FrameReceiver.h"
#include "link/codec/FrameWriter.h"
#include "link/codec/Messages.h"
#include "link/codec/Numbers.h"
#include "link/session/Interfaces.h"
#include "link/session/ResetReason.h"
#include "link/session/Session.h"
#include "meshclient/Job.h"
#include "meshclient/LinkMeshClient.h"
#include "meshclient/LinkObservations.h"
#include "meshclient/MeshPort.h"
#include "meshclient/ObservationRing.h"
#include "meshclient/Reply.h"

#ifndef FIXTURES_DIR
#error "FIXTURES_DIR must be defined; platformio.ini's [env:native] does it"
#endif

namespace {

using namespace twinoak::link;  // NOLINT: a test of one namespace, and the names are the wire's

// ----------------------------------------------------------------------------- bytes and hex

std::vector<uint8_t> fromHex(const std::string& s) {
  std::vector<uint8_t> out;
  out.reserve(s.size() / 2);
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    out.push_back(uint8_t(std::stoul(s.substr(i, 2), nullptr, 16)));
  }
  return out;
}

std::string toHex(const uint8_t* data, size_t len) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out += digits[data[i] >> 4];
    out += digits[data[i] & 0x0F];
  }
  return out;
}

std::string toHex(const std::vector<uint8_t>& v) { return toHex(v.data(), v.size()); }

// One frame built the way a Walter builds it, so that a test can send anything the fixture does
// not carry. The leading zero of an idle line is the sender's business and is left off here.
std::vector<uint8_t> frameOf(uint8_t type, uint8_t seq, const std::vector<uint8_t>& body) {
  FrameWriter w;
  const bool ok = w.build(type, seq, body.empty() ? nullptr : body.data(), body.size(), false);
  EXPECT_TRUE(ok);
  return std::vector<uint8_t>(w.bytes(), w.bytes() + w.size());
}

// ----------------------------------------------------------------------------- the fixture file

JsonDocument& fixture() {
  static JsonDocument doc;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    const std::string path = std::string(FIXTURES_DIR) + "/link-v1.json";
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << "cannot read " << path;
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();
    const DeserializationError err = deserializeJson(doc, text);
    EXPECT_TRUE(err == DeserializationError::Ok) << path << ": " << err.c_str();
  }
  return doc;
}

struct Case {
  std::string name;
  std::string kind;
  uint8_t type = 0;
  uint8_t seq = 0;
  JsonObject fields;
  std::vector<uint8_t> payload;
  std::vector<uint8_t> frame;
};

const std::vector<Case>& cases() {
  static std::vector<Case> all;
  if (all.empty()) {
    JsonArray from = fixture()["cases"].as<JsonArray>();
    for (JsonObject c : from) {
      Case k;
      k.name = c["name"].as<std::string>();
      k.kind = c["kind"].as<std::string>();
      k.type = c["type"].as<uint8_t>();
      k.seq = c["seq"].as<uint8_t>();
      k.fields = c["fields"].as<JsonObject>();
      k.payload = fromHex(c["payload"].as<std::string>());
      k.frame = fromHex(c["frame"].as<std::string>());
      all.push_back(k);
    }
  }
  return all;
}

// The case of that kind, type and seq, or nullptr.
const Case* findCase(const char* kind, uint8_t type, uint8_t seq) {
  for (const Case& c : cases()) {
    if (c.kind == kind && c.type == type && c.seq == seq) return &c;
  }
  return nullptr;
}

const Case* findCaseNamed(const char* name) {
  for (const Case& c : cases()) {
    if (c.name == name) return &c;
  }
  return nullptr;
}

// ----------------------------------------------------------------------------- the fakes

class FakeSystem : public SystemAccess {
 public:
  uint32_t uptime_s = 0;
  uint8_t reset_reason = kResetUnknown;
  uint32_t boot_id = 0;
  bool radio_up = true;
  uint8_t highest_mode = kRebootOtaDfu;  // every mode up to this one is possible

  int restarts = 0;
  uint8_t restarted_in = 0xFF;

  uint32_t uptimeSeconds() const override { return uptime_s; }
  uint8_t resetReason() const override { return reset_reason; }
  uint32_t bootId() const override { return boot_id; }
  bool radioUp() const override { return radio_up; }
  bool canRestart(uint8_t mode) const override { return mode <= highest_mode; }
  void restartNow(uint8_t mode) override {
    restarts++;
    restarted_in = mode;
    // A fake returns, where the board never does; the session is written so that this is safe.
  }
};

class FakeIdentity : public IdentityAccess {
 public:
  uint8_t key[kPubKeyLen] = {0};
  std::string name = "";

  const uint8_t* publicKey() const override { return key; }
  const char* nodeName() const override { return name.c_str(); }
};

// The clock keeper's own rules - which values it accepts and what it saves - are
// test/test_clock's, with the real ClockKeeper behind a real session. Here the clock only has to
// answer, so TIME_SET moves it when `accept` says so and refuses when it does not.
class FakeClock : public ClockAccess {
 public:
  uint32_t epoch_s = 0;
  uint8_t state = kTimeUnset;
  bool held = false;
  bool accept = true;
  int sets = 0;

  uint32_t epoch() const override { return epoch_s; }
  uint8_t timeState() const override { return state; }
  bool advertsHeld() const override { return held; }
  bool setTime(uint32_t epoch, bool force, uint32_t& prev_epoch) override {
    (void)force;
    prev_epoch = epoch_s;
    sets++;
    if (!accept) return false;
    epoch_s = epoch;
    state = kTimeSet;
    return true;
  }
};

// A command interpreter that is not upstream's: it records the line it was given and answers
// whatever the test told it to. What upstream's own interpreter does with a line is upstream's
// business and is on the bench list; what this suite checks is the handler around it.
class FakeCli : public CliAccess {
 public:
  std::string answer;      // what the interpreter writes back
  std::string last_line;   // what it was given, NUL-terminated by the session
  size_t last_cap = 0;     // the window it was given: upstream's console gives 160
  int calls = 0;
  // A command that writes one byte past the window it was given, which is what two of upstream's
  // own commands can do on a console that has nothing behind it (reports/05.md).
  bool write_past_window = false;

  size_t runCommand(char* line, char* reply, size_t reply_cap) override {
    calls++;
    last_line = line;
    last_cap = reply_cap;
    if (reply_cap == 0) return 0;
    const size_t n = answer.size() > reply_cap - 1 ? reply_cap - 1 : answer.size();
    memcpy(reply, answer.data(), n);
    reply[n] = 0;
    if (write_past_window) reply[reply_cap] = 'X';
    return n;
  }
};

// The machine itself is test/test_attention's, with a fake clock and a real session. Here it is a
// fake that answers what the test sets and counts what the session tells it, so that the
// session's half of section 8 - the order of the calls, and which request clears what - is
// visible on its own.
class FakeAttention : public AttentionAccess {
 public:
  uint8_t reason = kAttentionNone;
  bool asleep = false;
  uint8_t due = kAttentionDueNothing;
  Attention event;

  int requests = 0;
  int answers = 0;
  int wakes = 0;
  int events = 0;
  int sleeps = 0;
  uint8_t last_request_type = 0;
  uint32_t last_expect_back = 0;

  uint8_t pendingReason() const override { return reason; }
  bool walterAsleep() const override { return asleep; }

  uint8_t dueNow(uint32_t) const override { return due; }
  Attention pendingEvent() const override { return event; }
  void wakeSent(uint32_t) override {
    wakes++;
    due = kAttentionDueNothing;
  }
  void eventSent(uint32_t) override {
    events++;
    due = kAttentionDueNothing;
  }

  void noteRequest(uint8_t request_type, uint32_t) override {
    requests++;
    last_request_type = request_type;
    if (request_type != kMsgSleepNotice) asleep = false;
  }
  void requestAnswered(uint32_t) override { answers++; }
  void sleepNotice(uint32_t expect_back_s, uint32_t) override {
    sleeps++;
    last_expect_back = expect_back_s;
    asleep = true;
  }
};

// The readers themselves are test/test_power's, against a fake I2C bus. Here they only have to
// answer, so these hold whatever the test - or the fixture - told them to.
class FakePower : public PowerAccess {
 public:
  PowerResponse answer;
  bool ok = true;
  int calls = 0;

  bool readPower(PowerResponse& out) override {
    calls++;
    out = answer;
    return ok;
  }
};

class FakeEnv : public EnvAccess {
 public:
  EnvResponse answer;
  bool ok = true;
  int calls = 0;

  bool readEnv(EnvResponse& out) override {
    calls++;
    out = answer;
    return ok;
  }
};

class FakeLpp : public LppAccess {
 public:
  std::vector<uint8_t> answer;
  int calls = 0;
  uint8_t last_mask = 0;
  size_t last_cap = 0;

  size_t readLpp(uint8_t perm_mask, uint8_t* out, size_t cap) override {
    calls++;
    last_mask = perm_mask;
    last_cap = cap;
    const size_t n = answer.size() > cap ? cap : answer.size();
    if (n > 0) memcpy(out, answer.data(), n);
    return n;
  }
};

class FakeMeshClient : public MeshClientAccess {
 public:
  bool job = false;
  bool awaiting = false;
  int link_requests = 0;

  bool jobBegun() const override { return job; }
  bool replyAwaited() const override { return awaiting; }

  // The generic sweep only reads the two bits above (for STATUS); the job requests, the sends,
  // the reply and the counters are driven against the real src/meshclient/ further down, so
  // these answer plainly.
  uint8_t beginJob(const JobBegin&, uint32_t) override {
    job = true;
    return kOk;
  }
  void endJob() override {
    job = false;
    awaiting = false;
  }
  uint8_t setPath(const Path&) override { return job ? kOk : kErrState; }
  uint8_t getPath(Path& out) override {
    out = Path();
    return job ? kOk : kErrState;
  }
  void noteLinkRequest(uint32_t) override { link_requests++; }
  uint8_t sendLogin(const SendLogin&, SendResponse&, uint32_t) override { return kErrState; }
  uint8_t sendRequest(const SendRequest&, SendResponse&, uint32_t) override { return kErrState; }
  uint8_t sendCli(const SendCli&, SendResponse&, uint32_t) override { return kErrState; }
  uint8_t sendDiscovery(const SendDiscovery&, SendResponse&, uint32_t) override {
    return kErrState;
  }
  uint8_t sendTrace(const SendTrace&, SendResponse&, uint32_t) override { return kErrState; }
  void getReply(ReplyRecord& out, uint32_t) override { out = ReplyRecord(); }
  bool takeReplyEvent(ReplyRecord&, uint32_t) override { return false; }
  void getCounters(MeshCountersResponse&) override {}
};

// The board's half of the five sends (src/meshclient/MeshPort.h), as a fake that records what it
// was asked to put on the air - the plaintext, the route, the trace's fields - and answers the
// time and the radio's counters with whatever the test set. It builds no packet: what MeshCore
// makes of the plaintext is upstream's, and src/app/TwinOakMesh.cpp cites it against the pin.
class FakeMeshPort : public twinoak::meshclient::MeshPort {
 public:
  struct Sent {
    enum Kind : uint8_t { kAnon, kDatagram, kTrace } kind = kAnon;
    uint8_t payload_type = 0xFF;
    std::vector<uint8_t> plain;  // the test's own copy, for checking the layout
    bool direct = false;
    std::vector<uint8_t> path;
    uint8_t path_len = 0;
    uint32_t tag = 0;
    uint32_t auth = 0;
    uint8_t flags = 0;
  };

  uint32_t time = 1791720050;  // the unique second the next send gets; counts up
  uint8_t random_fill = 0x5a;  // what randomBytes() writes
  bool fail = false;           // "the pool is empty": no packet can be built
  std::vector<Sent> sent;
  twinoak::meshclient::RadioCounters counters;

  uint32_t uniqueTime() override { return time++; }
  void randomBytes(uint8_t* dest, size_t len) override {
    for (size_t i = 0; i < len; i++) dest[i] = random_fill;
  }
  bool sendAnonRequest(const uint8_t* plain, size_t len,
                       const twinoak::meshclient::Route& route) override {
    if (fail) return false;
    Sent s;
    s.kind = Sent::kAnon;
    s.plain.assign(plain, plain + len);
    record(s, route);
    return true;
  }
  bool sendDatagram(uint8_t payload_type, const uint8_t* plain, size_t len,
                    const twinoak::meshclient::Route& route) override {
    if (fail) return false;
    Sent s;
    s.kind = Sent::kDatagram;
    s.payload_type = payload_type;
    s.plain.assign(plain, plain + len);
    record(s, route);
    return true;
  }
  bool sendTrace(uint32_t tag, uint32_t auth_code, uint8_t flags, const uint8_t* path,
                 uint8_t path_bytes) override {
    if (fail) return false;
    Sent s;
    s.kind = Sent::kTrace;
    s.direct = true;
    s.tag = tag;
    s.auth = auth_code;
    s.flags = flags;
    s.path.assign(path, path + path_bytes);
    s.path_len = path_bytes;
    sent.push_back(s);
    return true;
  }
  void radioCounters(twinoak::meshclient::RadioCounters& out) override { out = counters; }

 private:
  void record(Sent& s, const twinoak::meshclient::Route& route) {
    s.direct = route.direct;
    s.path_len = route.path_len;
    const size_t bytes = size_t(twinoak::meshclient::pathHashSize(route.path_len)) *
                         twinoak::meshclient::pathHopCount(route.path_len);
    if (route.path != nullptr && route.path_len != 0) {
      s.path.assign(route.path, route.path + bytes);
    }
    sent.push_back(s);
  }
};

// The ring itself is test/test_meshclient's. Here it is a fake that answers what the test - or
// the fixture - set, so that the session's half of section 7 is visible on its own; the real ring
// behind a real session is driven further down (ObsRig).
class FakeObservations : public ObservationAccess {
 public:
  uint8_t waiting = 0;
  uint16_t lost = 0;
  uint32_t heard = 0;
  uint32_t heard_dropped = 0;
  uint32_t obs_lost = 0;

  // What the next pull answers, and what it was asked for.
  uint8_t answer_remaining = 0;
  uint16_t answer_dropped = 0;
  std::vector<Observation> answer;
  std::vector<std::vector<uint8_t>> blobs;  // the bytes the views above point into
  int pulls = 0;
  uint8_t last_max = 0;
  uint32_t last_now = 0;

  uint8_t pending() const override { return waiting; }
  uint16_t droppedSinceBoot() const override { return lost; }
  uint32_t adverts() const override { return heard; }
  uint32_t advertsDropped() const override { return heard_dropped; }
  uint32_t observationsDropped() const override { return obs_lost; }

  void pull(uint8_t max, ObsPullResponse& out, uint32_t now_ms) override {
    pulls++;
    last_max = max;
    last_now = now_ms;
    out.remaining = answer_remaining;
    out.dropped = answer_dropped;
    out.records = answer.empty() ? nullptr : answer.data();
    out.count = uint8_t(answer.size());
  }
};

// ----------------------------------------------------------------------------- the harness

// A session with every interface present (so HELLO advertises all seven capability bits, as the
// fixture's HELLO response does) and a transport written exactly as src/link/SerialTransport.cpp
// writes it: read until a request is waiting, poll once, write the whole frame if there is one.
class Harness {
 public:
  Harness() {
    firmware.role = kRoleRepeater;
    firmware.slot = 0;
    firmware.version = version.c_str();
    firmware.board = board.c_str();
    firmware.upstream = upstream.c_str();

    services.firmware = &firmware;
    services.system = &system;
    services.identity = &identity;
    services.clock = &clock;
    services.cli = &cli;
    services.attention = &attention;
    services.power = &power;
    services.env = &env;
    services.lpp = &lpp;  // no capability bit of its own (LINK-PROTOCOL.md section 4)
    services.mesh = &mesh;
    services.observations = &observations;
  }

  // The strings HELLO reports. Set before the first pass; the Firmware struct points into them.
  void setStrings(const std::string& v, const std::string& b, const std::string& u) {
    version = v;
    board = b;
    upstream = u;
    firmware.version = version.c_str();
    firmware.board = board.c_str();
    firmware.upstream = upstream.c_str();
  }

  void feed(const std::vector<uint8_t>& data) {
    for (uint8_t b : data) in.push_back(b);
  }
  void feed(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) in.push_back(data[i]);
  }

  // One pass of loop(), as the transport does it.
  void pass() {
    size_t budget = kReceiveBuffer;
    while (budget > 0 && !session.requestWaiting() && !in.empty()) {
      budget--;
      session.feedByte(in.front(), now);
      in.pop_front();
    }
    session.poll(now);
    if (deliver_writes) takeOutgoing();
  }

  // Passes until the input is drained and nothing more is produced.
  void run() {
    for (int i = 0; i < 2000; i++) {
      const bool had = !in.empty() || session.hasOutgoing() || session.requestWaiting();
      pass();
      if (!had) return;
    }
    ADD_FAILURE() << "the session did not settle";
  }

  // Hands whatever is on hand to the "UART" and records it.
  bool takeOutgoing() {
    if (!session.hasOutgoing()) return false;
    sent.push_back(toHex(session.outgoing(), session.outgoingSize()));
    session.outgoingSent(now);
    return true;
  }

  // The frame last recorded, with the leading zero of an idle line removed. A COBS frame never
  // begins with 0x00 otherwise (LINK-PROTOCOL.md section 2), so this is unambiguous.
  std::string lastFrame() const {
    if (sent.empty()) return "";
    const std::string& f = sent.back();
    return f.rfind("00", 0) == 0 ? f.substr(2) : f;
  }
  bool lastHadLeadingZero() const {
    return !sent.empty() && sent.back().rfind("00", 0) == 0;
  }

  Firmware firmware;
  FakeSystem system;
  FakeIdentity identity;
  FakeClock clock;
  FakeCli cli;
  FakeAttention attention;
  FakePower power;
  FakeEnv env;
  FakeLpp lpp;
  FakeMeshClient mesh;
  FakeObservations observations;
  Services services;
  Session session{services};

  std::deque<uint8_t> in;
  std::vector<std::string> sent;
  uint32_t now = 1000;
  bool deliver_writes = true;

 private:
  std::string version = "0.0.0-dev";
  std::string board = "rak4630-single";
  std::string upstream = "repeater-v1.17.1+d9296435";
};

// Sets the fakes so that the session's answer to `request` is exactly `response`.
void arrange(Harness& h, const Case& response) {
  JsonObject f = response.fields;
  switch (response.type) {
    case kMsgHello:
      h.system.boot_id = f["boot_id"].as<uint32_t>();
      h.firmware.role = f["role"].as<uint8_t>();
      h.firmware.slot = f["slot"].as<uint8_t>();
      h.setStrings(f["fw_version"].as<std::string>(), f["board"].as<std::string>(),
                   f["upstream"].as<std::string>());
      break;
    case kMsgPing:
      h.system.uptime_s = f["uptime_s"].as<uint32_t>();
      break;
    case kMsgStatus: {
      h.system.uptime_s = f["uptime_s"].as<uint32_t>();
      h.system.reset_reason = f["reset_reason"].as<uint8_t>();
      h.clock.state = f["time_state"].as<uint8_t>();
      h.clock.epoch_s = f["epoch"].as<uint32_t>();
      h.observations.waiting = f["obs_pending"].as<uint8_t>();
      h.observations.lost = f["obs_dropped"].as<uint16_t>();
      h.attention.reason = f["attention"].as<uint8_t>();
      const uint16_t flags = f["flags"].as<uint16_t>();
      h.mesh.job = (flags & kFlagJobBegun) != 0;
      h.mesh.awaiting = (flags & kFlagReplyAwaited) != 0;
      h.attention.asleep = (flags & kFlagWalterAsleep) != 0;
      h.clock.held = (flags & kFlagAdvertsHeld) != 0;
      h.system.radio_up = (flags & kFlagRadioUp) != 0;
      // link_errors is the receiver's own count and cannot be set: it is made to happen. A
      // three-byte packet is refused as `short` (section 2).
      const uint16_t want_errors = f["link_errors"].as<uint16_t>();
      for (uint16_t i = 0; i < want_errors; i++) {
        const std::vector<uint8_t> bad = {0x04, 0x11, 0x22, 0x33, 0x00};
        h.feed(bad);
        h.pass();
      }
      EXPECT_EQ(want_errors, h.session.linkErrors());
      EXPECT_TRUE(h.sent.empty()) << "a dropped frame is never answered";
      break;
    }
    case kMsgTimeSet:
      // The clock the fixture's prev_epoch says the node had; the keeper's own rules about which
      // values it accepts are test/test_clock's.
      h.clock.epoch_s = f["prev_epoch"].as<uint32_t>();
      h.clock.accept = true;
      break;
    case kMsgTimeGet:
      h.clock.epoch_s = f["epoch"].as<uint32_t>();
      h.clock.state = f["time_state"].as<uint8_t>();
      break;
    case kMsgIdentity: {
      const std::vector<uint8_t> key = fromHex(f["pubkey"].as<std::string>());
      ASSERT_EQ(size_t(kPubKeyLen), key.size());
      memcpy(h.identity.key, key.data(), kPubKeyLen);
      h.identity.name = f["name"].as<std::string>();
      break;
    }
    case kMsgReboot:
      break;  // the fixture's mode 1 is one this board can do, which is the default
    case kMsgCli:
      h.cli.answer = f["text"].as<std::string>();
      break;
    case kMsgPowerGet: {
      h.power.answer.valid = f["valid"].as<uint8_t>();
      JsonArray channels = f["channels"].as<JsonArray>();
      ASSERT_EQ(size_t(kPowerChannels), channels.size());
      uint8_t i = 0;
      for (JsonObject c : channels) {
        h.power.answer.channels[i].bus_mv = c["bus_mv"].as<uint16_t>();
        h.power.answer.channels[i].current_ma = c["current_ma"].as<int16_t>();
        i++;
      }
      break;
    }
    case kMsgEnvGet:
      h.env.answer.valid = f["valid"].as<uint8_t>();
      h.env.answer.temp_centi = f["temp_centi"].as<int16_t>();
      h.env.answer.rh_centi = f["rh_centi"].as<uint16_t>();
      h.env.answer.pressure_pa = f["pressure_pa"].as<uint32_t>();
      break;
    case kMsgLppGet:
      h.lpp.answer = fromHex(f["lpp"].as<std::string>());
      break;
    case kMsgObsPull: {
      // Section 7, as a shape: the fixture's own two records, handed to the session by the fake
      // ring so that what this suite checks is the handler and the encoding. Which records a real
      // ring hands out, and how many fit, is test/test_meshclient's and ObsRig's below.
      h.observations.answer_remaining = f["remaining"].as<uint8_t>();
      h.observations.answer_dropped = f["dropped"].as<uint16_t>();
      JsonArray records = f["records"].as<JsonArray>();
      h.observations.blobs.reserve(3 * records.size());
      for (JsonObject r : records) {
        Observation o;
        h.observations.blobs.push_back(fromHex(r["pubkey"].as<std::string>()));
        o.pubkey = h.observations.blobs.back().data();
        o.adv_ts = r["adv_ts"].as<uint32_t>();
        o.age_s = r["age_s"].as<uint32_t>();
        o.snr_q4 = r["snr_q4"].as<int8_t>();
        o.rssi = r["rssi"].as<int16_t>();
        o.path_len = r["path_len"].as<uint8_t>();
        h.observations.blobs.push_back(fromHex(r["path"].as<std::string>()));
        o.path = bytesOf(h.observations.blobs.back().data(),
                         h.observations.blobs.back().size());
        h.observations.blobs.push_back(fromHex(r["app_data"].as<std::string>()));
        o.app_data = bytesOf(h.observations.blobs.back().data(),
                             h.observations.blobs.back().size());
        h.observations.answer.push_back(o);
      }
      break;
    }

    case kMsgSleepNotice:
      break;  // its response is a status byte and nothing else
    default:
      ADD_FAILURE() << response.name << ": nothing here arranges this type";
      break;
  }
}

// The request types this firmware answers: task 03's five, task 04's two, task 05's two,
// task 06's three.
bool handledHere(uint8_t type) {
  return type == kMsgHello || type == kMsgPing || type == kMsgStatus || type == kMsgIdentity ||
         type == kMsgReboot || type == kMsgTimeSet || type == kMsgTimeGet || type == kMsgCli ||
         type == kMsgSleepNotice || type == kMsgPowerGet || type == kMsgEnvGet ||
         type == kMsgLppGet || type == kMsgObsPull;
}

}  // namespace

// ----------------------------------------------------------------------------- the fixtures

TEST(TheFixtures, EveryRequestThisTaskAnswersGivesTheFixturesResponseFrame) {
  int n = 0;
  for (const Case& c : cases()) {
    if (c.kind != "request" || !handledHere(c.type)) continue;
    const Case* want = findCase("response", c.type, c.seq);
    ASSERT_NE(nullptr, want) << c.name << ": no response case with the same type and seq";

    Harness h;
    arrange(h, *want);
    h.feed(c.frame);
    h.run();

    ASSERT_EQ(1u, h.sent.size()) << c.name << ": exactly one answer to one request";
    EXPECT_EQ(toHex(want->frame), h.lastFrame()) << c.name << " -> " << want->name;
    EXPECT_TRUE(h.lastHadLeadingZero())
        << c.name << ": the line was idle, so the frame carries the leading 0x00 (section 2)";
    EXPECT_EQ(1u, h.session.executed()) << c.name << ": one handler call";
    n++;
  }
  EXPECT_EQ(13, n) << "HELLO, PING, STATUS, IDENTITY, REBOOT, TIME_SET, TIME_GET, CLI,"
                      " SLEEP_NOTICE, POWER_GET, ENV_GET, LPP_GET, OBS_PULL";
}

TEST(TheFixtures, TheBootEventIsTheFixturesFrame) {
  const Case* want = findCase("event", kMsgEvBoot, 0);
  ASSERT_NE(nullptr, want);
  JsonObject f = want->fields;

  Harness h;
  h.system.reset_reason = f["reset_reason"].as<uint8_t>();
  h.system.boot_id = f["boot_id"].as<uint32_t>();
  h.firmware.role = f["role"].as<uint8_t>();
  h.firmware.slot = f["slot"].as<uint8_t>();
  h.setStrings(f["fw_version"].as<std::string>(), f["board"].as<std::string>(),
               f["upstream"].as<std::string>());

  EXPECT_EQ(0, h.session.nextEventSeq());
  h.session.begin(h.now);
  ASSERT_TRUE(h.takeOutgoing());
  EXPECT_EQ(toHex(want->frame), h.lastFrame());
  EXPECT_TRUE(h.lastHadLeadingZero()) << "the first thing the link ever sends leads with 0x00";
  EXPECT_EQ(1, h.session.nextEventSeq()) << "events are counted from 0 and wrap (section 3)";

  // Once, and only once: a pass after it produces nothing.
  h.run();
  EXPECT_EQ(1u, h.sent.size());
}

TEST(TheFixtures, AnUnknownTypeIsAnsweredUnsupportedWithThatTypesResponseCode) {
  const Case* want = findCaseNamed("unknown request type answered");
  ASSERT_NE(nullptr, want);
  EXPECT_EQ(0x5E, want->type);

  Harness h;
  h.feed(frameOf(want->type, want->seq, {}));
  h.run();
  ASSERT_EQ(1u, h.sent.size());
  EXPECT_EQ(toHex(want->frame), h.lastFrame());
}

TEST(TheFixtures, HelloWithAMajorWeDoNotSpeakIsRefused) {
  const Case* want = findCaseNamed("hello refused: major not spoken");
  ASSERT_NE(nullptr, want);

  // The fixture's own hello request, with its major changed to one we do not speak and the seq
  // the refusal carries.
  const Case* req = findCase("request", kMsgHello, 1);
  ASSERT_NE(nullptr, req);
  std::vector<uint8_t> body = req->payload;
  ASSERT_FALSE(body.empty());
  body[0] = 2;  // proto_major

  Harness h;
  h.feed(frameOf(kMsgHello, want->seq, body));
  h.run();
  ASSERT_EQ(1u, h.sent.size());
  EXPECT_EQ(toHex(want->frame), h.lastFrame());
  EXPECT_EQ(1u, h.session.executed());
}

// ----------------------------------------------------------------------------- HELLO's shape

TEST(Hello, AHigherMinorIsAnsweredWithTheHighestBothKnow) {
  EXPECT_EQ(0, highestMinorBothKnow(0, 0));
  EXPECT_EQ(0, highestMinorBothKnow(7, 0));
  EXPECT_EQ(3, highestMinorBothKnow(3, 9));
  EXPECT_EQ(4, highestMinorBothKnow(9, 4));

  Harness h;
  std::vector<uint8_t> body = {kProtoMajor, 7, kPeerBenchTool, uint8_t(kMaxPayload), 0};
  h.feed(frameOf(kMsgHello, 42, body));
  h.run();
  ASSERT_EQ(1u, h.sent.size());

  const std::vector<uint8_t> frame = fromHex(h.lastFrame());
  // The response body: status, proto_major, proto_minor, ... (section 5.1). Read it back out of
  // the frame through the codec rather than by counting bytes.
  FrameReceiver rx;
  for (uint8_t b : frame) rx.feedByte(b);
  ASSERT_TRUE(rx.hasPacket());
  EXPECT_EQ(responseType(kMsgHello), rx.packet().type);
  EXPECT_EQ(42, rx.packet().seq);
  ASSERT_GE(rx.packet().body.len, 3);
  EXPECT_EQ(kOk, rx.packet().body.ptr[0]);
  EXPECT_EQ(kProtoMajor, rx.packet().body.ptr[1]);
  EXPECT_EQ(kProtoMinor, rx.packet().body.ptr[2]) << "the highest minor both ends know";
}

TEST(Hello, ACapabilityWhoseInterfaceIsAbsentIsNotAdvertised) {
  Harness h;
  EXPECT_EQ(uint32_t(kCapCli | kCapPower | kCapEnv | kCapMeshClient | kCapObservations |
                     kCapAttention | kCapTimePersist),
            capsOf(h.services));

  h.services.cli = nullptr;
  EXPECT_EQ(0u, capsOf(h.services) & kCapCli);
  h.services.clock = nullptr;
  EXPECT_EQ(0u, capsOf(h.services) & kCapTimePersist);
  h.services.power = nullptr;
  h.services.env = nullptr;
  h.services.mesh = nullptr;
  h.services.observations = nullptr;
  h.services.attention = nullptr;
  EXPECT_EQ(0u, capsOf(h.services)) << "a build with nothing but the link advertises nothing";

  // ... and HELLO says so.
  std::vector<uint8_t> body = {kProtoMajor, kProtoMinor, kPeerWalter, uint8_t(kMaxPayload), 0};
  h.feed(frameOf(kMsgHello, 1, body));
  h.run();
  ASSERT_EQ(1u, h.sent.size());
  FrameReceiver rx;
  for (uint8_t b : fromHex(h.lastFrame())) rx.feedByte(b);
  ASSERT_TRUE(rx.hasPacket());
  Reader r(rx.packet().body);
  EXPECT_EQ(kOk, r.u8());
  r.u8();
  r.u8();
  r.u8();
  r.u8();
  r.u8();
  EXPECT_EQ(0u, r.u32()) << "caps";
  EXPECT_TRUE(r.ok());
}

TEST(Status, WhatALaterTaskOwnsReadsZeroAndItsFlagBitIsClear) {
  Harness h;
  // The board of this task: only the three interfaces it implements.
  h.services.clock = nullptr;
  h.services.cli = nullptr;
  h.services.attention = nullptr;
  h.services.power = nullptr;
  h.services.env = nullptr;
  h.services.mesh = nullptr;
  h.services.observations = nullptr;
  h.system.uptime_s = 7;
  h.system.reset_reason = kResetPin;

  h.feed(frameOf(kMsgStatus, 3, {}));
  h.run();
  ASSERT_EQ(1u, h.sent.size());

  FrameReceiver rx;
  for (uint8_t b : fromHex(h.lastFrame())) rx.feedByte(b);
  ASSERT_TRUE(rx.hasPacket());
  Reader r(rx.packet().body);
  EXPECT_EQ(kOk, r.u8());
  EXPECT_EQ(7u, r.u32()) << "uptime_s";
  EXPECT_EQ(kResetPin, r.u8()) << "reset_reason";
  EXPECT_EQ(kTimeUnset, r.u8()) << "time_state: no clock interface";
  EXPECT_EQ(0u, r.u32()) << "epoch";
  EXPECT_EQ(uint16_t(kFlagRadioUp), r.u16()) << "only the radio bit";
  EXPECT_EQ(0, r.u8()) << "obs_pending";
  EXPECT_EQ(0, r.u16()) << "obs_dropped";
  EXPECT_EQ(0, r.u16()) << "link_errors";
  EXPECT_EQ(kAttentionNone, r.u8()) << "attention";
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(0u, r.remaining()) << "and nothing after it";
}

// ----------------------------------------------------------------------------- the retry memory

TEST(TheRetryMemory, TheSameRequestTwiceIsExecutedOnce) {
  Harness h;
  h.system.uptime_s = 11;
  const std::vector<uint8_t> ping = frameOf(kMsgPing, 77, {0x01, 0x02, 0x03, 0x04});

  h.feed(ping);
  h.run();
  ASSERT_EQ(1u, h.sent.size());
  EXPECT_EQ(1u, h.session.executed());
  const std::string first = h.lastFrame();

  // The Walter heard nothing and sends the same frame again, same seq (section 3).
  h.system.uptime_s = 12;  // the world moved on; the remembered answer did not
  h.feed(ping);
  h.run();
  ASSERT_EQ(2u, h.sent.size());
  EXPECT_EQ(first, h.lastFrame()) << "the remembered response, byte for byte";
  EXPECT_EQ(1u, h.session.executed()) << "and the handler was not called again";

  // A different seq is a new request.
  h.feed(frameOf(kMsgPing, 78, {0x01, 0x02, 0x03, 0x04}));
  h.run();
  ASSERT_EQ(3u, h.sent.size());
  EXPECT_NE(first, h.lastFrame()) << "a new uptime, so a different answer";
  EXPECT_EQ(2u, h.session.executed());

  // The memory is one entry: the old seq is no longer remembered and runs again.
  h.feed(ping);
  h.run();
  ASSERT_EQ(4u, h.sent.size());
  EXPECT_EQ(3u, h.session.executed());
}

TEST(TheRetryMemory, ADifferentTypeWithTheSameSeqIsANewRequest) {
  Harness h;
  h.feed(frameOf(kMsgPing, 5, {0, 0, 0, 0}));
  h.run();
  h.feed(frameOf(kMsgStatus, 5, {}));
  h.run();
  ASSERT_EQ(2u, h.sent.size());
  EXPECT_EQ(2u, h.session.executed());
}

TEST(TheRetryMemory, ARepeatedRebootIsNotScheduledTwice) {
  Harness h;
  const std::vector<uint8_t> reboot = frameOf(kMsgReboot, 9, {kRebootApplication, 0x00, 0x00});
  h.feed(reboot);
  h.run();
  EXPECT_EQ(1, h.system.restarts) << "the delay was 0, so the restart follows the response";
  EXPECT_EQ(kRebootApplication, h.system.restarted_in);
  EXPECT_EQ(1u, h.session.executed());

  // The same frame again: the remembered response, and no second restart from the handler.
  h.feed(reboot);
  h.run();
  EXPECT_EQ(1u, h.session.executed());
  EXPECT_EQ(1, h.system.restarts);
}

// ----------------------------------------------------------------------------- REBOOT

TEST(Reboot, TheResponseLeavesBeforeTheRestartAndTheDelayIsKept) {
  Harness h;
  h.deliver_writes = false;  // the UART is held busy, as a full transmit buffer would hold it
  h.feed(frameOf(kMsgReboot, 8, {kRebootUf2, 0xF4, 0x01}));  // mode 1, 500 ms
  h.pass();
  ASSERT_TRUE(h.session.hasOutgoing());
  EXPECT_EQ(0, h.system.restarts) << "nothing restarts while the response is unsent";

  h.pass();
  EXPECT_EQ(0, h.system.restarts);

  ASSERT_TRUE(h.takeOutgoing());  // the response goes out at now = 1000
  h.now = 1000 + 499;
  h.session.poll(h.now);
  EXPECT_EQ(0, h.system.restarts) << "and not before the delay has passed";

  h.now = 1000 + 500;
  h.session.poll(h.now);
  EXPECT_EQ(1, h.system.restarts);
  EXPECT_EQ(kRebootUf2, h.system.restarted_in);

  h.session.poll(h.now + 1);
  EXPECT_EQ(1, h.system.restarts) << "once";
}

TEST(Reboot, AModeTheBoardCannotDoIsRefusedAndNothingIsScheduled) {
  Harness h;
  h.system.highest_mode = kRebootApplication;  // a board with no bootloader of its own
  h.feed(frameOf(kMsgReboot, 1, {kRebootOtaDfu, 0x00, 0x00}));
  h.run();
  ASSERT_EQ(1u, h.sent.size());

  FrameReceiver rx;
  for (uint8_t b : fromHex(h.lastFrame())) rx.feedByte(b);
  ASSERT_TRUE(rx.hasPacket());
  EXPECT_EQ(responseType(kMsgReboot), rx.packet().type);
  ASSERT_EQ(1u, unsigned(rx.packet().body.len)) << "a status that is not OK is one byte";
  EXPECT_EQ(kErrRange, rx.packet().body.ptr[0]);
  EXPECT_EQ(0, h.system.restarts);

  // And the mode it can do still works.
  h.feed(frameOf(kMsgReboot, 2, {kRebootApplication, 0x00, 0x00}));
  h.run();
  EXPECT_EQ(1, h.system.restarts);
}

// ----------------------------------------------------------------------------- malformed

TEST(Malformed, APayloadOneByteShortOfItsLayoutIsAnsweredMalformed) {
  struct Short {
    uint8_t type;
    std::vector<uint8_t> body;
  };
  const Short shorts[] = {
      {kMsgHello, {kProtoMajor, kProtoMinor, kPeerWalter}},  // no max_payload, no fw_version
      {kMsgPing, {0x01, 0x02, 0x03}},                        // three bytes of a u32
      {kMsgReboot, {kRebootApplication, 0x00}},              // one byte of a u16
  };
  for (const Short& s : shorts) {
    Harness h;
    h.feed(frameOf(s.type, 3, s.body));
    h.run();
    ASSERT_EQ(1u, h.sent.size()) << unsigned(s.type);

    FrameReceiver rx;
    for (uint8_t b : fromHex(h.lastFrame())) rx.feedByte(b);
    ASSERT_TRUE(rx.hasPacket());
    EXPECT_EQ(responseType(s.type), rx.packet().type);
    ASSERT_EQ(1u, unsigned(rx.packet().body.len));
    EXPECT_EQ(kErrMalformed, rx.packet().body.ptr[0]) << unsigned(s.type);
    EXPECT_EQ(0, h.system.restarts);
  }
}

TEST(Malformed, AFrameThatIsNotARequestIsDroppedUnanswered) {
  Harness h;
  // A response type and an event type: in 1.0 only the Walter sends requests and only the RAK
  // sends events (section 3), so neither is ours to answer.
  h.feed(frameOf(responseType(kMsgPing), 3, {kOk}));
  h.run();
  h.feed(frameOf(kMsgEvBoot, 0, {kResetPowerOn}));
  h.run();
  EXPECT_TRUE(h.sent.empty()) << "nothing is sent in reply";
  EXPECT_EQ(0u, h.session.executed());
  EXPECT_EQ(0, h.session.linkErrors()) << "and it is not a frame the receiver refused";
}

// ----------------------------------------------------------------------------- the byte stream

TEST(TheByteStream, TwoFramesInOneRead) {
  Harness h;
  std::vector<uint8_t> both = frameOf(kMsgPing, 1, {0x01, 0x00, 0x00, 0x00});
  const std::vector<uint8_t> second = frameOf(kMsgStatus, 2, {});
  both.insert(both.end(), second.begin(), second.end());

  h.feed(both);
  h.run();
  ASSERT_EQ(2u, h.sent.size());
  EXPECT_EQ(2u, h.session.executed());

  FrameReceiver rx;
  for (uint8_t b : fromHex(h.sent[0])) rx.feedByte(b);
  ASSERT_TRUE(rx.hasPacket());
  EXPECT_EQ(responseType(kMsgPing), rx.packet().type);
  EXPECT_EQ(1, rx.packet().seq);
  for (uint8_t b : fromHex(h.sent[1])) rx.feedByte(b);
  ASSERT_TRUE(rx.hasPacket());
  EXPECT_EQ(responseType(kMsgStatus), rx.packet().type);
  EXPECT_EQ(2, rx.packet().seq);
}

TEST(TheByteStream, AFrameSplitOverThreeReads) {
  Harness h;
  const std::vector<uint8_t> frame = frameOf(kMsgStatus, 4, {});
  ASSERT_GE(frame.size(), 3u);
  const size_t a = 1, b = 2;

  h.feed(frame.data(), a);
  h.pass();
  EXPECT_TRUE(h.sent.empty()) << "half a frame is not a request";
  h.feed(frame.data() + a, b - a + 1);
  h.pass();
  EXPECT_TRUE(h.sent.empty());
  h.feed(frame.data() + b + 1, frame.size() - b - 1);
  h.run();
  EXPECT_EQ(1u, h.sent.size());
  EXPECT_EQ(0, h.session.linkErrors()) << "a split frame is not a broken one";
}

TEST(TheByteStream, TheFixturesStreamWithNoiseInIt) {
  // The fixture's own stream: two terminators, a good PING frame, seven bytes of garbage and a
  // terminator, another terminator, a good STATUS frame.
  JsonObject s = fixture()["stream"].as<JsonObject>();
  const std::vector<uint8_t> bytes = fromHex(s["bytes"].as<std::string>());
  JsonArray want = s["packets"].as<JsonArray>();

  Harness h;
  h.feed(bytes);
  h.run();

  ASSERT_EQ(want.size(), h.sent.size()) << "one answer per packet the stream carries";
  size_t i = 0;
  for (JsonObject p : want) {
    FrameReceiver rx;
    for (uint8_t b : fromHex(h.sent[i])) rx.feedByte(b);
    ASSERT_TRUE(rx.hasPacket()) << i;
    EXPECT_EQ(responseType(p["wire_type"].as<uint8_t>()), rx.packet().type);
    EXPECT_EQ(p["seq"].as<uint8_t>(), rx.packet().seq);
    i++;
  }
  EXPECT_EQ(s["bad_frames"].as<uint16_t>(), h.session.linkErrors())
      << "the noise is counted, and never answered";
}

// ----------------------------------------------------------------------------- the writer

TEST(TheWriter, ARequestThatArrivesWithAResponseStillUnsentIsNeitherLostNorBlocking) {
  Harness h;
  h.deliver_writes = false;  // the UART has no room: availableForWrite() would be 0
  h.system.uptime_s = 1;

  h.feed(frameOf(kMsgPing, 10, {0x11, 0x00, 0x00, 0x00}));
  h.pass();
  ASSERT_TRUE(h.session.hasOutgoing());
  const std::string ping_answer = toHex(h.session.outgoing(), h.session.outgoingSize());
  EXPECT_EQ(1u, h.session.executed());

  // The second request arrives in full while the first answer is still queued.
  h.feed(frameOf(kMsgStatus, 11, {}));
  h.pass();
  EXPECT_TRUE(h.session.requestWaiting()) << "it is held, not dropped";
  EXPECT_EQ(1u, h.session.executed()) << "and not executed yet";
  EXPECT_EQ(ping_answer, toHex(h.session.outgoing(), h.session.outgoingSize()))
      << "the unsent answer is untouched";
  EXPECT_TRUE(h.in.empty()) << "and the transport read the whole frame out of the UART";

  // Several more passes change nothing: nothing spins and nothing is lost.
  h.pass();
  h.pass();
  EXPECT_EQ(1u, h.session.executed());
  EXPECT_EQ(ping_answer, toHex(h.session.outgoing(), h.session.outgoingSize()));

  // The UART drains, and the held request is answered on the next pass.
  ASSERT_TRUE(h.takeOutgoing());
  h.pass();
  ASSERT_TRUE(h.takeOutgoing());
  ASSERT_EQ(2u, h.sent.size());
  EXPECT_EQ(ping_answer, h.sent[0]);
  EXPECT_EQ(2u, h.session.executed());

  FrameReceiver rx;
  for (uint8_t b : fromHex(h.sent[1])) rx.feedByte(b);
  ASSERT_TRUE(rx.hasPacket());
  EXPECT_EQ(responseType(kMsgStatus), rx.packet().type);
  EXPECT_EQ(11, rx.packet().seq);
}

TEST(TheWriter, TheLeadingZeroIsSentOnlyAfterAHundredMillisecondsOfQuiet) {
  Harness h;
  h.feed(frameOf(kMsgStatus, 1, {}));
  h.run();
  ASSERT_EQ(1u, h.sent.size());
  EXPECT_TRUE(h.lastHadLeadingZero()) << "nothing had ever been sent";

  h.now += kIdleBeforeLeadingZeroMs - 1;
  h.feed(frameOf(kMsgStatus, 2, {}));
  h.run();
  ASSERT_EQ(2u, h.sent.size());
  EXPECT_FALSE(h.lastHadLeadingZero()) << "99 ms is not idle";

  h.now += kIdleBeforeLeadingZeroMs;
  h.feed(frameOf(kMsgStatus, 3, {}));
  h.run();
  ASSERT_EQ(3u, h.sent.size());
  EXPECT_TRUE(h.lastHadLeadingZero()) << "100 ms or more is";
}

// ----------------------------------------------------------------------------- walter.seen

TEST(WalterSeen, CountsFromTheLastValidRequestAndNotFromNoise) {
  Harness h;
  EXPECT_FALSE(h.session.everSawRequest());

  const std::vector<uint8_t> bad = {0x04, 0x11, 0x22, 0x33, 0x00};
  h.feed(bad);
  h.run();
  EXPECT_FALSE(h.session.everSawRequest()) << "a frame the receiver refused is not a request";
  EXPECT_EQ(1, h.session.linkErrors());

  h.now = 50000;
  h.feed(frameOf(kMsgStatus, 1, {}));
  h.run();
  EXPECT_TRUE(h.session.everSawRequest());
  EXPECT_EQ(0u, h.session.secondsSinceRequest(h.now));
  EXPECT_EQ(90u, h.session.secondsSinceRequest(h.now + 90500));
}

// ----------------------------------------------------------------------------- the clock

TEST(TheClock, ARefusedTimeSetIsTheFixturesErrRange) {
  const Case* want = findCaseNamed("time set refused: backwards without force");
  ASSERT_NE(nullptr, want);
  const Case* req = findCase("request", kMsgTimeSet, 5);
  ASSERT_NE(nullptr, req);

  Harness h;
  h.clock.epoch_s = 1791720000;
  h.clock.accept = false;  // what the keeper answers for a backwards set without force

  h.feed(frameOf(kMsgTimeSet, want->seq, req->payload));
  h.run();
  ASSERT_EQ(1u, h.sent.size());
  EXPECT_EQ(toHex(want->frame), h.lastFrame());
  EXPECT_EQ(1, h.clock.sets) << "the keeper was asked, and said no";
}

TEST(TheClock, WithNoClockInterfaceBothAreUnsupported) {
  // "A capability is an interface that is there": a build with no clock keeper does not
  // advertise TIME_PERSIST and answers kErrUnsupported for the two messages that need one.
  const Case* req = findCase("request", kMsgTimeSet, 5);
  ASSERT_NE(nullptr, req);

  Harness h;
  h.services.clock = nullptr;

  h.feed(frameOf(kMsgTimeSet, 11, req->payload));
  h.run();
  ASSERT_EQ(1u, h.sent.size());
  EXPECT_EQ(toHex(frameOf(responseType(kMsgTimeSet), 11, {kErrUnsupported})), h.lastFrame());

  h.feed(frameOf(kMsgTimeGet, 12, {}));
  h.run();
  ASSERT_EQ(2u, h.sent.size());
  EXPECT_EQ(toHex(frameOf(responseType(kMsgTimeGet), 12, {kErrUnsupported})), h.lastFrame());

  EXPECT_EQ(0u, uint32_t(capsOf(h.services) & kCapTimePersist));
}

TEST(TheClock, AMalformedTimeSetIsMalformedAndTheClockIsNotAsked) {
  Harness h;
  h.feed(frameOf(kMsgTimeSet, 13, {0x01, 0x02, 0x03}));  // five bytes short of a payload
  h.run();
  ASSERT_EQ(1u, h.sent.size());
  EXPECT_EQ(toHex(frameOf(responseType(kMsgTimeSet), 13, {kErrMalformed})), h.lastFrame());
  EXPECT_EQ(0, h.clock.sets);
}

// ----------------------------------------------------------------------------- the console

namespace {

// The status byte of the last answer, and its type and seq, read back through the codec.
uint8_t answeredStatus(Harness& h, uint8_t& type, uint8_t& seq) {
  FrameReceiver rx;
  for (uint8_t b : fromHex(h.lastFrame())) rx.feedByte(b);
  EXPECT_TRUE(rx.hasPacket());
  type = rx.packet().type;
  seq = rx.packet().seq;
  EXPECT_GE(rx.packet().body.len, 1);
  return rx.packet().body.len >= 1 ? rx.packet().body.ptr[0] : 0xFF;
}

// The reply text of the last CLI answer.
std::string answeredCliText(Harness& h) {
  FrameReceiver rx;
  for (uint8_t b : fromHex(h.lastFrame())) rx.feedByte(b);
  EXPECT_TRUE(rx.hasPacket());
  Reader r(rx.packet().body);
  EXPECT_EQ(kOk, r.u8());
  const Bytes text = r.rest();
  return std::string(reinterpret_cast<const char*>(text.ptr), text.len);
}

std::vector<uint8_t> cliFrame(uint8_t seq, const std::string& line) {
  return frameOf(kMsgCli, seq, std::vector<uint8_t>(line.begin(), line.end()));
}

}  // namespace

TEST(TheConsole, TheLineIsRunAsTypedAndTheWindowIsUpstreamsOwn) {
  Harness h;
  h.cli.answer = "> 869.618,62.5,8,8";
  h.feed(cliFrame(11, "get radio"));
  h.run();
  ASSERT_EQ(1u, h.sent.size());
  EXPECT_EQ(1, h.cli.calls);
  EXPECT_EQ("get radio", h.cli.last_line) << "the line, NUL-terminated and nothing more";
  EXPECT_EQ(size_t(kCliReplyMax), h.cli.last_cap)
      << "the 160 bytes upstream's own console loop gives its interpreter";
  EXPECT_EQ("> 869.618,62.5,8,8", answeredCliText(h));
  EXPECT_EQ(0u, h.session.cliOverruns());
}

TEST(TheConsole, AnEmptyReplyIsTheFixturesZeroLengthResponse) {
  const Case* want = findCaseNamed("cli response with a zero-length reply");
  ASSERT_NE(nullptr, want);

  Harness h;
  h.cli.answer = "";  // `get acl` prints to the console and answers nothing
  h.feed(cliFrame(want->seq, "get acl"));
  h.run();
  ASSERT_EQ(1u, h.sent.size());
  EXPECT_EQ(toHex(want->frame), h.lastFrame()) << "status OK and no text at all";
  EXPECT_EQ(1, h.cli.calls);
}

TEST(TheConsole, AReplyAtTheWindowsLimitComesBackWhole) {
  Harness h;
  h.cli.answer = std::string(kCliReplyMax - 1, 'x');  // 159 bytes and its NUL
  h.feed(cliFrame(1, "neighbors"));
  h.run();
  ASSERT_EQ(1u, h.sent.size());
  EXPECT_EQ(h.cli.answer, answeredCliText(h));
  EXPECT_EQ(size_t(kCliReplyMax - 1), answeredCliText(h).size())
      << "159 bytes is the most a console reply can be, and it fits the protocol's 160";
  EXPECT_EQ(1u, h.session.cliTruncations()) << "it filled the window, which is worth counting";
  EXPECT_EQ(0u, h.session.cliOverruns()) << "but it did not write past it";
}

TEST(TheConsole, AReplyWrittenPastTheWindowIsAbsorbedCountedAndCutBack) {
  Harness h;
  h.cli.answer = std::string(kCliReplyMax - 1, 'y');
  h.cli.write_past_window = true;  // one byte into the guard
  h.feed(cliFrame(2, "sensor list"));
  h.run();
  ASSERT_EQ(1u, h.sent.size());
  EXPECT_EQ(h.cli.answer, answeredCliText(h)) << "what the console itself would have printed";
  EXPECT_EQ(1u, h.session.cliOverruns());

  // And the session is unharmed: the next request is answered as usual.
  h.cli.write_past_window = false;
  h.cli.answer = "ok";
  h.feed(cliFrame(3, "sensor list 4"));
  h.run();
  ASSERT_EQ(2u, h.sent.size());
  EXPECT_EQ("ok", answeredCliText(h));
}

TEST(TheConsole, ALineThatIsNotALegalValueIsMalformedAndIsNeverRun) {
  struct Bad {
    const char* what;
    std::string line;
  };
  const Bad bad[] = {
      {"empty", ""},
      {"161 bytes", std::string(kCliLineMax + 1, 'a')},
      {"a newline in it", std::string("get radio\n")},
      {"a tab in it", std::string("get\tradio")},
      {"a NUL in it", std::string("get\0radio", 9)},
      {"DEL", std::string("get radio\x7F")},
      {"a C1 control, as UTF-8", std::string("get \xC2\x85 radio")},
      {"a truncated UTF-8 sequence", std::string("get \xC3")},
      {"a continuation byte on its own", std::string("get \x80 radio")},
      {"an overlong encoding of '/'", std::string("get \xC0\xAF")},
      {"a surrogate half", std::string("get \xED\xA0\x80")},
  };
  for (const Bad& b : bad) {
    Harness h;
    h.feed(cliFrame(7, b.line));
    h.run();
    ASSERT_EQ(1u, h.sent.size()) << b.what;
    uint8_t type = 0, seq = 0;
    EXPECT_EQ(kErrMalformed, answeredStatus(h, type, seq)) << b.what;
    EXPECT_EQ(responseType(kMsgCli), type) << b.what;
    EXPECT_EQ(7, seq) << b.what;
    EXPECT_EQ(0, h.cli.calls) << b.what << ": the interpreter was never asked";
  }

  // And the two that are legal and nearly were not: 160 bytes, and UTF-8 above ASCII.
  Harness h;
  h.cli.answer = "ok";
  const std::string longest(kCliLineMax, 'b');
  h.feed(cliFrame(8, longest));
  h.run();
  EXPECT_EQ(1, h.cli.calls);
  EXPECT_EQ(longest, h.cli.last_line);
  h.feed(cliFrame(9, "set name Bænk \xE2\x9C\x93"));
  h.run();
  EXPECT_EQ(2, h.cli.calls);
  EXPECT_EQ("set name Bænk \xE2\x9C\x93", h.cli.last_line);
}

TEST(TheConsole, WithNoCliInterfaceItIsUnsupported) {
  Harness h;
  h.services.cli = nullptr;
  EXPECT_EQ(0u, capsOf(h.services) & kCapCli);

  h.feed(cliFrame(4, "get radio"));
  h.run();
  ASSERT_EQ(1u, h.sent.size());
  uint8_t type = 0, seq = 0;
  EXPECT_EQ(kErrUnsupported, answeredStatus(h, type, seq));
  EXPECT_EQ(responseType(kMsgCli), type);
  EXPECT_EQ(0, h.cli.calls);
}

TEST(TheConsole, ARetriedLineIsNotRunTwice) {
  Harness h;
  h.cli.answer = "OK - stats reset";
  const std::vector<uint8_t> frame = cliFrame(5, "clear stats");
  h.feed(frame);
  h.run();
  const std::string first = h.lastFrame();
  EXPECT_EQ(1, h.cli.calls);

  h.cli.answer = "something else entirely";
  h.feed(frame);
  h.run();
  ASSERT_EQ(2u, h.sent.size());
  EXPECT_EQ(first, h.lastFrame()) << "the remembered response";
  EXPECT_EQ(1, h.cli.calls) << "and the command was run once - it is not idempotent";
}

// ----------------------------------------------------------------------------- a sleeping Walter

TEST(TheSleepingWalter, SleepNoticeSetsTheBeliefAndTheNextRequestClearsIt) {
  Harness h;
  const Case* want = findCase("response", kMsgSleepNotice, 10);
  const Case* req = findCase("request", kMsgSleepNotice, 10);
  ASSERT_NE(nullptr, want);
  ASSERT_NE(nullptr, req);

  h.feed(req->frame);
  h.run();
  ASSERT_EQ(1u, h.sent.size());
  EXPECT_EQ(toHex(want->frame), h.lastFrame());
  EXPECT_EQ(1, h.attention.sleeps);
  EXPECT_EQ(900u, h.attention.last_expect_back) << "the fixture's expect_back_s";
  EXPECT_TRUE(h.attention.asleep);
  EXPECT_EQ(kMsgSleepNotice, h.attention.last_request_type)
      << "the machine is told which request it was, because this one does not wake it";

  // STATUS then carries flag bit 2.
  h.feed(frameOf(kMsgStatus, 11, {}));
  h.run();
  ASSERT_EQ(2u, h.sent.size());
  FrameReceiver rx;
  for (uint8_t b : fromHex(h.lastFrame())) rx.feedByte(b);
  ASSERT_TRUE(rx.hasPacket());
  Reader r(rx.packet().body);
  r.u8();
  r.u32();
  r.u8();
  r.u8();
  r.u32();
  EXPECT_EQ(0u, uint32_t(r.u16() & kFlagWalterAsleep))
      << "that request itself woke it, so its own answer already says awake";
  EXPECT_FALSE(h.attention.asleep);
  EXPECT_EQ(2, h.attention.requests);
}

TEST(TheSleepingWalter, OnlyEvBootAndEvAttentionLeaveWhileItSleeps) {
  Harness h;
  EXPECT_TRUE(h.session.eventAllowed(kMsgEvBoot));
  EXPECT_TRUE(h.session.eventAllowed(kMsgEvAttention));
  EXPECT_TRUE(h.session.eventAllowed(kMsgEvMeshReply)) << "awake, everything leaves";

  h.attention.asleep = true;
  EXPECT_TRUE(h.session.eventAllowed(kMsgEvBoot)) << "section 8 lets this one through";
  EXPECT_TRUE(h.session.eventAllowed(kMsgEvAttention)) << "and this one";
  EXPECT_FALSE(h.session.eventAllowed(kMsgEvMeshReply))
      << "phase 2's event waits for the Walter to ask";

  // EV_BOOT really does leave: the link coming up while the belief is set still says so.
  h.session.begin(h.now);
  ASSERT_TRUE(h.takeOutgoing());
  FrameReceiver rx;
  for (uint8_t b : fromHex(h.lastFrame())) rx.feedByte(b);
  ASSERT_TRUE(rx.hasPacket());
  EXPECT_EQ(kMsgEvBoot, rx.packet().type);

  // With no machine at all there is no belief to hold, and nothing is ever held.
  h.services.attention = nullptr;
  EXPECT_TRUE(h.session.eventAllowed(kMsgEvMeshReply));
}

TEST(TheSleepingWalter, WithNoAttentionInterfaceSleepNoticeIsUnsupported) {
  Harness h;
  h.services.attention = nullptr;
  EXPECT_EQ(0u, capsOf(h.services) & kCapAttention);

  h.feed(frameOf(kMsgSleepNotice, 1, {0x84, 0x03, 0x00, 0x00}));
  h.run();
  ASSERT_EQ(1u, h.sent.size());
  uint8_t type = 0, seq = 0;
  EXPECT_EQ(kErrUnsupported, answeredStatus(h, type, seq));
  EXPECT_EQ(responseType(kMsgSleepNotice), type);
  EXPECT_EQ(0, h.attention.sleeps);
}

TEST(TheSleepingWalter, AMalformedSleepNoticeIsMalformedAndNothingIsBelieved) {
  Harness h;
  h.feed(frameOf(kMsgSleepNotice, 2, {0x01, 0x02, 0x03}));  // three bytes of a u32
  h.run();
  ASSERT_EQ(1u, h.sent.size());
  uint8_t type = 0, seq = 0;
  EXPECT_EQ(kErrMalformed, answeredStatus(h, type, seq));
  EXPECT_EQ(0, h.attention.sleeps);
  EXPECT_FALSE(h.attention.asleep);
}

// ----------------------------------------------------------------------------- attention, on the
// wire. The machine's own rules are test/test_attention's; this is the session's half: what it
// puts on the wire when the machine says so, and in which order.

TEST(Attention, TheWakeBytesThenTheEventTheMachineAsksFor) {
  Harness h;
  h.deliver_writes = false;
  h.attention.due = kAttentionDueWake;

  h.session.poll(h.now);
  ASSERT_TRUE(h.session.hasOutgoing());
  ASSERT_EQ(2u, h.session.outgoingSize()) << "0x00 0x00 is not a frame";
  EXPECT_EQ(0, h.session.outgoing()[0]);
  EXPECT_EQ(0, h.session.outgoing()[1]);
  h.session.outgoingSent(h.now);
  EXPECT_EQ(1, h.attention.wakes) << "and the machine starts its 300 ms from here";
  EXPECT_EQ(0, h.attention.events);

  // 300 ms later the machine asks for the event, and the session frames it.
  h.now += kAttentionWakeGapMs;
  h.attention.due = kAttentionDueEvent;
  h.attention.event.reason = kAttentionPersonAsked;
  h.attention.event.detail = 0;
  h.attention.event.repeat = 0;
  h.session.poll(h.now);
  ASSERT_TRUE(h.session.hasOutgoing());
  const std::string frame = toHex(h.session.outgoing(), h.session.outgoingSize());
  h.session.outgoingSent(h.now);
  EXPECT_EQ(1, h.attention.events) << "counted when the bytes went, not when they were built";
  EXPECT_EQ(0, h.attention.answers) << "an event is not a response";

  FrameReceiver rx;
  for (uint8_t b : fromHex(frame)) rx.feedByte(b);
  ASSERT_TRUE(rx.hasPacket());
  EXPECT_EQ(kMsgEvAttention, rx.packet().type);
  EXPECT_EQ(0, rx.packet().seq) << "events are counted from 0";
  Reader r(rx.packet().body);
  EXPECT_EQ(kAttentionPersonAsked, r.u8());
  EXPECT_EQ(0u, r.u16());
  EXPECT_EQ(0, r.u8());
  EXPECT_TRUE(r.ok());
}

TEST(Attention, TheFixturesTwoEventsByteForByte) {
  struct Want {
    const char* name;
  };
  const Want wants[] = {{"attention: the battery is low"},
                        {"attention: an admin asked for the walter, third time"}};
  for (const Want& w : wants) {
    const Case* want = findCaseNamed(w.name);
    ASSERT_NE(nullptr, want) << w.name;
    JsonObject f = want->fields;

    Harness h;
    h.deliver_writes = false;
    // The fixture's events carry a seq of their own, and the session counts its own from 0, so
    // the frames are compared without it: the payload is what the machine decides.
    h.attention.due = kAttentionDueEvent;
    h.attention.event.reason = f["reason"].as<uint8_t>();
    h.attention.event.detail = f["detail"].as<uint16_t>();
    h.attention.event.repeat = f["repeat"].as<uint8_t>();

    h.session.poll(h.now);
    ASSERT_TRUE(h.session.hasOutgoing()) << w.name;
    FrameReceiver rx;
    for (size_t i = 0; i < h.session.outgoingSize(); i++) rx.feedByte(h.session.outgoing()[i]);
    ASSERT_TRUE(rx.hasPacket()) << w.name;
    EXPECT_EQ(kMsgEvAttention, rx.packet().type);
    EXPECT_EQ(toHex(want->payload), toHex(rx.packet().body.ptr, rx.packet().body.len)) << w.name;
  }
}

TEST(Attention, AnAnsweredRequestIsWhatTellsTheMachineTheReasonWasDelivered) {
  Harness h;
  h.deliver_writes = false;
  h.attention.reason = kAttentionPersonAsked;

  h.feed(frameOf(kMsgStatus, 3, {}));
  h.pass();
  EXPECT_EQ(1, h.attention.requests) << "the request stops the repeats at once";
  EXPECT_EQ(0, h.attention.answers) << "but its answer has not gone yet";

  // The answer still carries the reason, which is the whole point of the order
  // (section 8: "a Walter that woke for its own reasons sees it too").
  FrameReceiver rx;
  for (size_t i = 0; i < h.session.outgoingSize(); i++) rx.feedByte(h.session.outgoing()[i]);
  ASSERT_TRUE(rx.hasPacket());
  Reader r(rx.packet().body);
  r.u8();
  r.u32();
  r.u8();
  r.u8();
  r.u32();
  r.u16();
  r.u8();
  r.u16();
  r.u16();
  EXPECT_EQ(kAttentionPersonAsked, r.u8()) << "STATUS's attention field";

  ASSERT_TRUE(h.takeOutgoing());
  EXPECT_EQ(1, h.attention.answers) << "and now the machine may forget it";
}

TEST(Attention, NothingIsSentWhileTheWireIsBusyOrARequestIsWaiting) {
  Harness h;
  h.deliver_writes = false;
  h.attention.due = kAttentionDueWake;

  // A request waiting beats an attention: the Walter is talking.
  h.feed(frameOf(kMsgStatus, 1, {}));
  h.pass();
  EXPECT_EQ(0, h.attention.wakes);
  EXPECT_TRUE(h.session.hasOutgoing()) << "what is on hand is the response";

  // And with something unsent, nothing else is built.
  h.session.poll(h.now);
  EXPECT_EQ(0, h.attention.wakes);
}

// ----------------------------------------------------------------------------- the reset reason

TEST(TheResetReason, EveryBitOfResetreasMapsToTheCodeOfSection53) {
  // An empty register is a power-on reset: the nRF52 sets no bit for it.
  EXPECT_EQ(kResetPowerOn, resetReasonFromNrf52(0));
  EXPECT_EQ(kResetPin, resetReasonFromNrf52(kReasResetPin));
  EXPECT_EQ(kResetWatchdog, resetReasonFromNrf52(kReasDog));
  EXPECT_EQ(kResetSoftware, resetReasonFromNrf52(kReasSoftReset));
  EXPECT_EQ(kResetLockup, resetReasonFromNrf52(kReasLockup));
  EXPECT_EQ(kResetSystemOff, resetReasonFromNrf52(kReasOff));
  EXPECT_EQ(kResetSystemOff, resetReasonFromNrf52(kReasLpComp));
  EXPECT_EQ(kResetSystemOff, resetReasonFromNrf52(kReasNfc));
  EXPECT_EQ(kResetSystemOff, resetReasonFromNrf52(kReasVbus));
  // A debugger's own reset, and anything Nordic adds later, is honestly unknown.
  EXPECT_EQ(kResetUnknown, resetReasonFromNrf52(kReasDif));
  EXPECT_EQ(kResetUnknown, resetReasonFromNrf52(0x00800000));
}

TEST(TheResetReason, SeveralBitsAtOnceReadAsWhatMattersMost) {
  // RESETREAS accumulates until it is cleared, so more than one bit can be set.
  EXPECT_EQ(kResetWatchdog, resetReasonFromNrf52(kReasDog | kReasSoftReset));
  EXPECT_EQ(kResetLockup, resetReasonFromNrf52(kReasLockup | kReasSoftReset));
  EXPECT_EQ(kResetSystemOff, resetReasonFromNrf52(kReasOff | kReasResetPin));
  EXPECT_EQ(kResetPin, resetReasonFromNrf52(kReasResetPin | kReasSoftReset));
  // Every code this mapping can produce is one of section 5.3's, and none of them is 7: the
  // register has no bit for "left the bootloader".
  for (uint32_t bit = 1; bit != 0; bit <<= 1) {
    const uint8_t code = resetReasonFromNrf52(bit);
    EXPECT_LE(code, kResetSystemOff) << "bit 0x" << std::hex << bit;
    EXPECT_NE(kResetBootloader, code);
  }
}


// ----------------------------------------------------------------------------- telemetry (5.8)

TEST(Telemetry, TheLargestLppPayloadIsTheFixturesFrame) {
  // The fixture's own 239-byte case: the whole of a response payload but its status byte, which
  // is the largest frame this link can carry (section 2).
  const Case* want = findCaseNamed("the largest payload");
  ASSERT_NE(nullptr, want);
  ASSERT_EQ(kMsgLppGet, want->type);

  Harness h;
  h.lpp.answer = fromHex(want->fields["lpp"].as<std::string>());
  ASSERT_EQ(239u, h.lpp.answer.size());

  h.feed(frameOf(kMsgLppGet, want->seq, {0x05}));
  h.run();
  ASSERT_EQ(1u, h.sent.size());
  EXPECT_EQ(toHex(want->frame), h.lastFrame());
  EXPECT_EQ(239u, h.lpp.last_cap) << "the whole payload but its status byte";
}

TEST(Telemetry, ThePermissionMaskIsHandedThroughUntouched) {
  Harness h;
  h.lpp.answer = {0x01, 0x74, 0x01, 0x90};

  // Every mask, the unknown bits included: "unknown bits are ignored" is what handing it through
  // means (section 5.8), and it is the node's telemetry path that decides what they mean.
  for (uint8_t mask = 0; mask < 16; mask++) {
    h.feed(frameOf(kMsgLppGet, uint8_t(100 + mask), {mask}));
    h.run();
    EXPECT_EQ(mask, h.lpp.last_mask) << "mask 0x0" << int(mask);
  }
  EXPECT_EQ(16, h.lpp.calls);
}

TEST(Telemetry, AnEmptyLppIsAStatusByteAndNothingElse) {
  // A node with no readings at all - nothing valid, no permission - answers OK with no bytes
  // after it, which is a legal body and not a failure.
  Harness h;
  h.lpp.answer.clear();

  h.feed(frameOf(kMsgLppGet, 7, {0x00}));
  h.run();
  ASSERT_EQ(1u, h.sent.size());
  EXPECT_EQ(toHex(frameOf(responseType(kMsgLppGet), 7, {kOk})), h.lastFrame());
}

TEST(Telemetry, AMalformedLppGetIsMalformedAndNothingIsAsked) {
  Harness h;
  h.feed(frameOf(kMsgLppGet, 8, {}));  // perm_mask missing
  h.run();
  ASSERT_EQ(1u, h.sent.size());
  EXPECT_EQ(toHex(frameOf(responseType(kMsgLppGet), 8, {kErrMalformed})), h.lastFrame());
  EXPECT_EQ(0, h.lpp.calls);
}

TEST(Telemetry, WithNoInterfaceEachOfTheThreeIsUnsupported) {
  // "A capability is an interface that is there": a build with no readers does not advertise
  // POWER or ENV, and answers kErrUnsupported for all three messages - LPP_GET included, which
  // section 4 gates on CLI and which therefore has no bit of its own to go missing.
  Harness h;
  h.services.power = nullptr;
  h.services.env = nullptr;
  h.services.lpp = nullptr;
  EXPECT_EQ(0u, capsOf(h.services) & kCapPower);
  EXPECT_EQ(0u, capsOf(h.services) & kCapEnv);
  EXPECT_NE(0u, capsOf(h.services) & kCapCli) << "and CLI is still there";

  const uint8_t types[] = {kMsgPowerGet, kMsgEnvGet, kMsgLppGet};
  uint8_t seq = 20;
  for (uint8_t type : types) {
    const std::vector<uint8_t> body = type == kMsgLppGet ? std::vector<uint8_t>{0x05}
                                                         : std::vector<uint8_t>{};
    h.feed(frameOf(type, seq, body));
    h.run();
    EXPECT_EQ(toHex(frameOf(responseType(type), seq, {kErrUnsupported})), h.lastFrame())
        << "type 0x0" << std::hex << int(type);
    seq++;
  }
  EXPECT_EQ(3u, h.sent.size());
}

TEST(Telemetry, AReaderThatCannotAnswerAtAllIsErrFailed) {
  // The readers always have an answer - a channel that could not be read has its bit clear - so
  // this is the shape of the seam and not a case the board reaches (src/power/LinkPower.cpp).
  Harness h;
  h.power.ok = false;
  h.env.ok = false;

  h.feed(frameOf(kMsgPowerGet, 30, {}));
  h.run();
  EXPECT_EQ(toHex(frameOf(responseType(kMsgPowerGet), 30, {kErrFailed})), h.lastFrame());

  h.feed(frameOf(kMsgEnvGet, 31, {}));
  h.run();
  EXPECT_EQ(toHex(frameOf(responseType(kMsgEnvGet), 31, {kErrFailed})), h.lastFrame());
}

TEST(Telemetry, ARetriedRequestDoesNotAskTheReadersAgain) {
  // Section 3's one-entry memory. It matters here because a POWER_GET and its retry must answer
  // with the same numbers: a Walter that lost a response and asked again is comparing the two.
  Harness h;
  h.power.answer.valid = 0x07;
  h.power.answer.channels[1].bus_mv = 3912;
  const std::vector<uint8_t> frame = frameOf(kMsgPowerGet, 44, {});

  h.feed(frame);
  h.run();
  const std::string first = h.lastFrame();
  EXPECT_EQ(1, h.power.calls);

  h.power.answer.channels[1].bus_mv = 3700;  // the pack moved on
  h.feed(frame);
  h.run();
  ASSERT_EQ(2u, h.sent.size());
  EXPECT_EQ(first, h.lastFrame()) << "the remembered response, to the byte";
  EXPECT_EQ(1, h.power.calls);
}

// ----------------------------------------------------------------------------- the mesh client

// The four job requests of section 6.1 driven against the **real** src/meshclient/ through a real
// Session: the Harness's FakeMeshClient is replaced with a LinkMeshClient over a real Job, so
// JOB_BEGIN, JOB_END, PATH_SET and PATH_GET are answered by the code the board runs and held
// against spec/fixtures/link/link-v1.json byte for byte. (The peer overrides of TwinOakMesh need
// MeshCore and are not here; their arithmetic and state are test/test_meshclient's.)
namespace {

// Feeds a request frame to a harness whose mesh service is `client`, and returns the one answer.
std::string answerWithClient(Harness& h, twinoak::meshclient::LinkMeshClient& client,
                             const std::vector<uint8_t>& frame) {
  h.services.mesh = &client;  // the session reads s_.mesh through the reference every pass
  h.sent.clear();
  h.feed(frame);
  h.run();
  EXPECT_EQ(1u, h.sent.size());
  return h.lastFrame();
}

}  // namespace

TEST(TheMeshClient, TheFourJobRequestsAreTheFixturesFrames) {
  // JOB_BEGIN, the two-hops shape (seq 18), answered kOk - the fixture's job begin response.
  {
    Harness h;
    twinoak::meshclient::Job job;
    FakeMeshPort port;
    twinoak::meshclient::LinkMeshClient client(job, port);
    const Case* req = findCase("request", kMsgJobBegin, 18);
    const Case* resp = findCase("response", kMsgJobBegin, 18);
    ASSERT_NE(nullptr, req);
    ASSERT_NE(nullptr, resp);
    EXPECT_EQ(toHex(resp->frame), answerWithClient(h, client, req->frame));
    EXPECT_TRUE(job.begun());
    EXPECT_EQ(twinoak::meshclient::kPathHops, job.pathMode());
    EXPECT_EQ(2u, job.pathHopCount());
  }
  // JOB_END (seq 19): the job is forgotten, answered kOk.
  {
    Harness h;
    twinoak::meshclient::Job job;
    FakeMeshPort port;
    twinoak::meshclient::LinkMeshClient client(job, port);
    const uint8_t key[twinoak::meshclient::kPeerKeyLen] = {0x11};
    ASSERT_TRUE(job.begin(key, twinoak::meshclient::kPathNone, 0, 0, nullptr, 0, 0));
    const Case* req = findCase("request", kMsgJobEnd, 19);
    const Case* resp = findCase("response", kMsgJobEnd, 19);
    ASSERT_NE(nullptr, req);
    ASSERT_NE(nullptr, resp);
    EXPECT_EQ(toHex(resp->frame), answerWithClient(h, client, req->frame));
    EXPECT_FALSE(job.begun());
  }
  // PATH_GET (seq 21): the job holds the route the fixture's response carries (hops, size 2,
  // one hop, 7f3a), and PATH_GET reads it back byte for byte.
  {
    Harness h;
    twinoak::meshclient::Job job;
    FakeMeshPort port;
    twinoak::meshclient::LinkMeshClient client(job, port);
    const uint8_t key[twinoak::meshclient::kPeerKeyLen] = {0x22};
    const uint8_t hashes[] = {0x7f, 0x3a};
    ASSERT_TRUE(job.begin(key, twinoak::meshclient::kPathHops, 2, 1, hashes, sizeof hashes, 0));
    const Case* req = findCase("request", kMsgPathGet, 21);
    const Case* resp = findCase("response", kMsgPathGet, 21);
    ASSERT_NE(nullptr, req);
    ASSERT_NE(nullptr, resp);
    EXPECT_EQ(toHex(resp->frame), answerWithClient(h, client, req->frame));
  }
}

TEST(TheMeshClient, JobBeginFloodAndDirectBothSucceed) {
  for (uint8_t seq : {16, 17}) {  // flood (seq 16) and direct neighbour (seq 17)
    Harness h;
    twinoak::meshclient::Job job;
    FakeMeshPort port;
    twinoak::meshclient::LinkMeshClient client(job, port);
    const Case* req = findCase("request", kMsgJobBegin, seq);
    ASSERT_NE(nullptr, req) << "seq " << int(seq);
    const std::string frame = answerWithClient(h, client, req->frame);
    // kOk is a status-only response: type JOB_BEGIN|0x80, seq, one byte 0x00.
    FrameReceiver rx;
    for (uint8_t b : fromHex(frame)) rx.feedByte(b);
    ASSERT_TRUE(rx.hasPacket());
    EXPECT_EQ(responseType(kMsgJobBegin), rx.packet().type);
    EXPECT_EQ(seq, rx.packet().seq);
    ASSERT_EQ(1u, rx.packet().body.len);
    EXPECT_EQ(kOk, rx.packet().body.ptr[0]);
    EXPECT_TRUE(job.begun());
    EXPECT_EQ(seq == 16 ? twinoak::meshclient::kPathNone : twinoak::meshclient::kPathDirect,
              job.pathMode());
  }
}

TEST(TheMeshClient, PathSetForgetIsOkWithAJobAndErrStateWithout) {
  const Case* req = findCase("request", kMsgPathSet, 20);  // "path set: forget"
  ASSERT_NE(nullptr, req);

  // With no job: ERR_STATE, and nothing changes.
  {
    Harness h;
    twinoak::meshclient::Job job;
    FakeMeshPort port;
    twinoak::meshclient::LinkMeshClient client(job, port);
    const std::string frame = answerWithClient(h, client, req->frame);
    FrameReceiver rx;
    for (uint8_t b : fromHex(frame)) rx.feedByte(b);
    ASSERT_TRUE(rx.hasPacket());
    ASSERT_EQ(1u, rx.packet().body.len);
    EXPECT_EQ(kErrState, rx.packet().body.ptr[0]) << "PATH_SET with no job is ERR_STATE (6.1)";
  }
  // With a job: kOk, and the path is forgotten.
  {
    Harness h;
    twinoak::meshclient::Job job;
    FakeMeshPort port;
    twinoak::meshclient::LinkMeshClient client(job, port);
    const uint8_t key[twinoak::meshclient::kPeerKeyLen] = {0x33};
    const uint8_t hashes[] = {0x7f, 0x3a};
    ASSERT_TRUE(job.begin(key, twinoak::meshclient::kPathHops, 2, 1, hashes, sizeof hashes, 0));
    const std::string frame = answerWithClient(h, client, req->frame);
    FrameReceiver rx;
    for (uint8_t b : fromHex(frame)) rx.feedByte(b);
    ASSERT_TRUE(rx.hasPacket());
    ASSERT_EQ(1u, rx.packet().body.len);
    EXPECT_EQ(kOk, rx.packet().body.ptr[0]);
    EXPECT_EQ(twinoak::meshclient::kPathNone, job.pathMode());
  }
}

TEST(TheMeshClient, AnIllegalPathInJobBeginIsMalformedAndChangesNothing) {
  Harness h;
  twinoak::meshclient::Job job;
  FakeMeshPort port;
  twinoak::meshclient::LinkMeshClient client(job, port);
  // A path 6.1 does not allow: hops, size 2, two hops, but only three bytes of hashes. The codec
  // refuses it, so the session answers ERR_MALFORMED and the client is never asked.
  std::vector<uint8_t> body(kPubKeyLen, 0x44);
  body.push_back(kPathHops);
  body.push_back(2);  // hash_size
  body.push_back(2);  // hop_count
  body.push_back(0xa1);
  body.push_back(0xb2);
  body.push_back(0x7f);  // three bytes, not four
  const std::string frame = answerWithClient(h, client, frameOf(kMsgJobBegin, 40, body));
  FrameReceiver rx;
  for (uint8_t b : fromHex(frame)) rx.feedByte(b);
  ASSERT_TRUE(rx.hasPacket());
  ASSERT_EQ(1u, rx.packet().body.len);
  EXPECT_EQ(kErrMalformed, rx.packet().body.ptr[0]);
  EXPECT_FALSE(job.begun()) << "a malformed JOB_BEGIN begins no job";
}

TEST(TheMeshClient, ASecondJobBeginEndsTheFirstThroughTheSession) {
  Harness h;
  twinoak::meshclient::Job job;
  FakeMeshPort port;
  twinoak::meshclient::LinkMeshClient client(job, port);
  const Case* first = findCase("request", kMsgJobBegin, 18);   // two hops
  const Case* second = findCase("request", kMsgJobBegin, 16);  // flood
  ASSERT_NE(nullptr, first);
  ASSERT_NE(nullptr, second);
  answerWithClient(h, client, first->frame);
  EXPECT_EQ(twinoak::meshclient::kPathHops, job.pathMode());
  answerWithClient(h, client, second->frame);
  EXPECT_TRUE(job.begun());
  EXPECT_EQ(twinoak::meshclient::kPathNone, job.pathMode()) << "the second job's path";
}

TEST(TheMeshClient, AnyRequestRestartsTheJobsSilenceCountThroughTheSession) {
  Harness h;
  twinoak::meshclient::Job job;
  FakeMeshPort port;
  twinoak::meshclient::LinkMeshClient client(job, port);
  h.services.mesh = &client;
  h.now = 1000;

  // Begin a job, then let the clock run almost to the 120-second limit.
  const Case* begin = findCase("request", kMsgJobBegin, 16);
  ASSERT_NE(nullptr, begin);
  h.feed(begin->frame);
  h.run();
  ASSERT_TRUE(job.begun());

  h.now = 1000 + twinoak::meshclient::kIdleEndMs - 1;
  client.loop(h.now);
  EXPECT_TRUE(job.begun()) << "one millisecond short of the limit";

  // A PING at that moment is a valid request: the session calls noteLinkRequest and the count
  // starts again, so the job survives another near-two-minutes.
  h.feed(frameOf(kMsgPing, 7, {0, 0, 0, 0}));
  h.run();
  client.loop(h.now);
  EXPECT_TRUE(job.begun());

  h.now += twinoak::meshclient::kIdleEndMs;  // now silent again for the full span
  client.loop(h.now);
  EXPECT_FALSE(job.begun()) << "120 s of silence after the last request ends the job";
}

// ----------------------------------------------------------------------------- the five sends

// The five sends, the reply and the counters of sections 6.2 to 6.4 driven against the **real**
// src/meshclient/ - a LinkMeshClient over a real Job and a real Reply - through a real Session,
// with a FakeMeshPort where the board builds the packet. What goes to the port is the plaintext
// the agent firmware's AgentMesh builds (tasks/phase-2/02-the-five-sends.md); what comes back on
// the link is held against spec/fixtures/link/link-v1.json byte for byte.
namespace {

using twinoak::meshclient::Job;
using twinoak::meshclient::LinkMeshClient;

struct ClientRig {
  Harness h;
  Job job;
  FakeMeshPort port;
  LinkMeshClient client;

  ClientRig() : client(job, port) { h.services.mesh = &client; }

  // Begins a job with the fixture's peer key and the path mode asked for.
  void beginJob(uint8_t mode, const std::vector<uint8_t>& hashes = {}, uint8_t hash_size = 2) {
    const uint8_t key[kPubKeyLen] = {0xa1, 0xb2, 0xc3, 0xd4};
    const uint8_t hops = uint8_t(hashes.empty() ? 0 : hashes.size() / hash_size);
    ASSERT_TRUE(job.begin(key, mode, hashes.empty() ? 0 : hash_size, hops,
                          hashes.empty() ? nullptr : hashes.data(), hashes.size(), h.now));
  }

  // Feeds one request frame and returns the one answer.
  std::string answer(const std::vector<uint8_t>& frame) {
    h.sent.clear();
    h.feed(frame);
    h.run();
    EXPECT_EQ(1u, h.sent.size());
    return h.lastFrame();
  }

  // The status byte of the last answer.
  uint8_t lastStatus() {
    uint8_t type = 0, seq = 0;
    return answeredStatus(h, type, seq);
  }

  // Lets the send ceiling's two-second gap pass (src/meshclient/SendCeiling.h). Every test that
  // makes a second successful send through one rig says this, because the firmware refuses one
  // that follows another inside two seconds - and saying it is cheaper than a seam that would
  // let a test turn the ceiling off.
  void gap() { h.now += twinoak::meshclient::kCeilingGapMs; }
};

// The fixture's request frame of this type and seq, with the seq changed: the payload is the
// case's, the seq is the test's.
std::vector<uint8_t> requestFrame(uint8_t type, uint8_t fixture_seq, uint8_t seq) {
  const Case* c = findCase("request", type, fixture_seq);
  EXPECT_NE(nullptr, c) << "type 0x" << std::hex << int(type) << " seq " << std::dec
                        << int(fixture_seq);
  if (c == nullptr) return {};
  return frameOf(type, seq, c->payload);
}

// A SEND_* payload built by hand: timeout_ms, then the body.
std::vector<uint8_t> sendPayload(uint32_t timeout_ms, const std::vector<uint8_t>& body) {
  std::vector<uint8_t> p = {uint8_t(timeout_ms), uint8_t(timeout_ms >> 8),
                            uint8_t(timeout_ms >> 16), uint8_t(timeout_ms >> 24)};
  p.insert(p.end(), body.begin(), body.end());
  return p;
}

std::vector<uint8_t> le32(uint32_t v) {
  return {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
}

// The body of the last frame the session sent, through the codec.
std::vector<uint8_t> lastBody(Harness& h, uint8_t& type, uint8_t& seq) {
  FrameReceiver rx;
  for (uint8_t b : fromHex(h.lastFrame())) rx.feedByte(b);
  EXPECT_TRUE(rx.hasPacket());
  type = rx.packet().type;
  seq = rx.packet().seq;
  return std::vector<uint8_t>(rx.packet().body.ptr, rx.packet().body.ptr + rx.packet().body.len);
}

// Whether `needle` occurs anywhere in the `size` bytes of memory at `obj`: the search the wipe
// test makes over the objects the session and the client are.
bool holdsBytes(const void* obj, size_t size, const std::vector<uint8_t>& needle) {
  const uint8_t* p = static_cast<const uint8_t*>(obj);
  if (needle.empty() || size < needle.size()) return false;
  for (size_t i = 0; i + needle.size() <= size; i++) {
    if (memcmp(p + i, needle.data(), needle.size()) == 0) return true;
  }
  return false;
}

}  // namespace

TEST(TheFiveSends, LoginIsTheFixturesFramesAndThePlaintextIsTheAgentsLayout) {
  // "login with a password" (seq 23), with a job and no path: sent as a flood at the fixture's
  // sender_ts, tag 0. The fixture's own password is a test string of the specification's.
  ClientRig r;
  r.beginJob(kPathNone);
  r.port.time = 1791720050;
  const Case* req = findCase("request", kMsgSendLogin, 23);
  const Case* resp = findCase("response", kMsgSendLogin, 23);
  ASSERT_NE(nullptr, req);
  ASSERT_NE(nullptr, resp);
  EXPECT_EQ(toHex(resp->frame), r.answer(req->frame));

  ASSERT_EQ(1u, r.port.sent.size()) << "one packet per send";
  const FakeMeshPort::Sent& s = r.port.sent[0];
  EXPECT_EQ(FakeMeshPort::Sent::kAnon, s.kind) << "a login is an anonymous request";
  EXPECT_FALSE(s.direct) << "no path: a flood";
  // The plaintext BaseChatMesh::sendLogin builds: the timestamp, then the password's bytes.
  std::vector<uint8_t> want = le32(1791720050);
  const std::vector<uint8_t> pw = fromHex(req->fields["password"].as<std::string>());
  want.insert(want.end(), pw.begin(), pw.end());
  EXPECT_EQ(toHex(want), toHex(s.plain));
  EXPECT_EQ(1u, r.client.clientTx());
  EXPECT_TRUE(r.job.awaiting()) << "timeout_ms 12000: a response is awaited";
  EXPECT_EQ(twinoak::meshclient::kAwaitResponse, r.job.awaitKind());
  EXPECT_EQ(0u, r.job.awaitTag()) << "a login reply carries the repeater's timestamp, not ours";
  EXPECT_EQ(twinoak::meshclient::kReplyPending, r.client.reply().state());

  // "login with no password (the acl login)" (seq 22): four bytes of timestamp and nothing else.
  ClientRig acl;
  acl.beginJob(kPathDirect);
  acl.port.time = 77;
  const Case* blank = findCase("request", kMsgSendLogin, 22);
  ASSERT_NE(nullptr, blank);
  acl.answer(blank->frame);
  EXPECT_EQ(kOk, acl.lastStatus());
  ASSERT_EQ(1u, acl.port.sent.size());
  EXPECT_EQ(toHex(le32(77)), toHex(acl.port.sent[0].plain)) << "the ACL login: the key alone";
  EXPECT_TRUE(acl.port.sent[0].direct) << "a direct neighbour: sent direct, zero hops";
  EXPECT_EQ(0u, acl.port.sent[0].path_len);
}

TEST(TheFiveSends, RefusedWithoutAJobAndWhileAReplyIsAwaited) {
  // "send refused: no job" (SEND_LOGIN, seq 24) and "send refused: a reply is still awaited"
  // (SEND_REQUEST, seq 25) - the fixture's two refusal frames, and nothing reaches the port.
  ClientRig r;
  const Case* no_job = findCase("response", kMsgSendLogin, 24);
  ASSERT_NE(nullptr, no_job);
  EXPECT_EQ(toHex(no_job->frame), r.answer(requestFrame(kMsgSendLogin, 23, 24)));
  EXPECT_TRUE(r.port.sent.empty());
  EXPECT_EQ(0u, r.client.clientTx()) << "a refused send is not counted";

  r.beginJob(kPathNone);
  r.answer(requestFrame(kMsgSendLogin, 23, 40));
  EXPECT_EQ(kOk, r.lastStatus());
  EXPECT_TRUE(r.job.awaiting());
  const Case* busy = findCase("response", kMsgSendRequest, 25);
  ASSERT_NE(nullptr, busy);
  EXPECT_EQ(toHex(busy->frame), r.answer(requestFrame(kMsgSendRequest, 26, 25)));
  EXPECT_EQ(1u, r.port.sent.size()) << "the second send put nothing on the air";
  EXPECT_EQ(1u, r.client.clientTx());

  // Every other send is busy too while the reply is awaited - a trace included, which needs no
  // job but still waits its turn.
  r.answer(requestFrame(kMsgSendCli, 28, 41));
  EXPECT_EQ(kErrBusy, r.lastStatus());
  r.answer(requestFrame(kMsgSendDiscovery, 30, 42));
  EXPECT_EQ(kErrBusy, r.lastStatus());
  r.answer(requestFrame(kMsgSendTrace, 31, 43));
  EXPECT_EQ(kErrBusy, r.lastStatus());
  EXPECT_EQ(1u, r.port.sent.size());

  // Without a job, the other three are ERR_STATE and a trace is not.
  ClientRig t;
  t.answer(requestFrame(kMsgSendRequest, 26, 1));
  EXPECT_EQ(kErrState, t.lastStatus());
  t.answer(requestFrame(kMsgSendCli, 28, 2));
  EXPECT_EQ(kErrState, t.lastStatus());
  t.answer(requestFrame(kMsgSendDiscovery, 30, 3));
  EXPECT_EQ(kErrState, t.lastStatus());
  EXPECT_TRUE(t.port.sent.empty());
  t.answer(requestFrame(kMsgSendTrace, 31, 4));
  EXPECT_EQ(kOk, t.lastStatus()) << "a trace needs no job (6.2)";
  EXPECT_EQ(1u, t.port.sent.size());
}

TEST(TheFiveSends, RequestCliDiscoveryAndTraceAreTheFixturesFrames) {
  // "request sent direct" (seq 27, the neighbours request) with a direct path.
  {
    ClientRig r;
    r.beginJob(kPathHops, {0x7f, 0x3a}, 2);
    r.port.time = 1791720051;
    const Case* req = findCase("request", kMsgSendRequest, 27);
    const Case* resp = findCase("response", kMsgSendRequest, 27);
    ASSERT_NE(nullptr, req);
    ASSERT_NE(nullptr, resp);
    EXPECT_EQ(toHex(resp->frame), r.answer(req->frame));
    ASSERT_EQ(1u, r.port.sent.size());
    const FakeMeshPort::Sent& s = r.port.sent[0];
    EXPECT_EQ(FakeMeshPort::Sent::kDatagram, s.kind);
    EXPECT_EQ(twinoak::meshclient::kPayloadTypeReq, s.payload_type);
    EXPECT_TRUE(s.direct);
    EXPECT_EQ("7f3a", toHex(s.path)) << "along the job's path";
    EXPECT_EQ(twinoak::meshclient::encodePathLen(2, 1), s.path_len);
    // AgentMesh::sendRequest: the tag, the type, the data.
    std::vector<uint8_t> want = le32(1791720051);
    want.push_back(0x06);
    const std::vector<uint8_t> data = fromHex(req->fields["data"].as<std::string>());
    want.insert(want.end(), data.begin(), data.end());
    EXPECT_EQ(toHex(want), toHex(s.plain));
    EXPECT_EQ(1791720051u, r.job.awaitTag()) << "the reply is matched by the timestamp";
  }
  // "status request to the peer" (seq 26): type 1 and no data, answered kOk.
  {
    ClientRig r;
    r.beginJob(kPathNone);
    r.answer(requestFrame(kMsgSendRequest, 26, 26));
    EXPECT_EQ(kOk, r.lastStatus());
    ASSERT_EQ(1u, r.port.sent.size());
    EXPECT_EQ(5u, r.port.sent[0].plain.size());
    EXPECT_EQ(0x01, r.port.sent[0].plain[4]);
    EXPECT_FALSE(r.port.sent[0].direct);
  }
  // "cli line sent" (seq 29, the line flagged sensitive) with a direct path: tag 0.
  {
    ClientRig r;
    r.beginJob(kPathDirect);
    r.port.time = 1791720052;
    const Case* req = findCase("request", kMsgSendCli, 29);
    const Case* resp = findCase("response", kMsgSendCli, 29);
    ASSERT_NE(nullptr, req);
    ASSERT_NE(nullptr, resp);
    EXPECT_EQ(toHex(resp->frame), r.answer(req->frame));
    ASSERT_EQ(1u, r.port.sent.size());
    const FakeMeshPort::Sent& s = r.port.sent[0];
    EXPECT_EQ(twinoak::meshclient::kPayloadTypeTxtMsg, s.payload_type);
    EXPECT_TRUE(s.direct);
    EXPECT_EQ(0u, s.path_len) << "zero hops";
    // BaseChatMesh::sendCommandData's shape: timestamp, TXT_TYPE_CLI_DATA << 2, the line.
    std::vector<uint8_t> want = le32(1791720052);
    want.push_back(0x04);
    const std::string text = req->fields["text"].as<std::string>();
    want.insert(want.end(), text.begin(), text.end());
    EXPECT_EQ(toHex(want), toHex(s.plain));
    EXPECT_EQ(twinoak::meshclient::kAwaitText, r.job.awaitKind());
    EXPECT_EQ(0u, r.job.awaitTag()) << "a CLI reply carries no tag";
  }
  // "discovery sent" (seq 30): always a flood, even with a direct path in the job.
  {
    ClientRig r;
    r.beginJob(kPathHops, {0x7f, 0x3a}, 2);
    r.port.time = 1791720053;
    r.port.random_fill = 0xc3;
    const Case* req = findCase("request", kMsgSendDiscovery, 30);
    const Case* resp = findCase("response", kMsgSendDiscovery, 30);
    ASSERT_NE(nullptr, req);
    ASSERT_NE(nullptr, resp);
    EXPECT_EQ(toHex(resp->frame), r.answer(req->frame));
    ASSERT_EQ(1u, r.port.sent.size());
    const FakeMeshPort::Sent& s = r.port.sent[0];
    EXPECT_EQ(twinoak::meshclient::kPayloadTypeReq, s.payload_type);
    EXPECT_FALSE(s.direct) << "discovery always floods, whatever path the job holds";
    // The companion's path-discovery request: the telemetry type, the inverse base mask, three
    // reserved bytes, four random ones.
    std::vector<uint8_t> want = le32(1791720053);
    const uint8_t tail[] = {0x03, 0xfe, 0, 0, 0, 0xc3, 0xc3, 0xc3, 0xc3};
    want.insert(want.end(), tail, tail + sizeof tail);
    EXPECT_EQ(toHex(want), toHex(s.plain));
  }
  // "trace over a two-byte round trip" (seq 31): direct, flags 1, the hashes as the path, the
  // tag the timestamp; and "trace refused: three-byte hashes" (seq 32).
  {
    ClientRig r;
    r.port.time = 1791720054;
    r.port.random_fill = 0x11;
    const Case* req = findCase("request", kMsgSendTrace, 31);
    ASSERT_NE(nullptr, req);
    r.answer(req->frame);
    uint8_t type = 0, seq = 0;
    const std::vector<uint8_t> body = lastBody(r.h, type, seq);
    ASSERT_EQ(10u, body.size());
    EXPECT_EQ(kOk, body[0]);
    EXPECT_EQ(kSendDirect, body[1]) << "a trace is always direct";
    EXPECT_EQ(toHex(le32(1791720054)), toHex(std::vector<uint8_t>(body.begin() + 2, body.begin() + 6)))
        << "tag";
    EXPECT_EQ(toHex(le32(1791720054)), toHex(std::vector<uint8_t>(body.begin() + 6, body.end())))
        << "sender_ts";
    ASSERT_EQ(1u, r.port.sent.size());
    const FakeMeshPort::Sent& s = r.port.sent[0];
    EXPECT_EQ(FakeMeshPort::Sent::kTrace, s.kind);
    EXPECT_EQ(1791720054u, s.tag);
    EXPECT_EQ(0x11111111u, s.auth) << "four random bytes";
    EXPECT_EQ(1, s.flags) << "two-byte hashes: the exponent 1";
    EXPECT_EQ("7f3aa1b27f3a46f6", toHex(s.path));
    EXPECT_EQ(twinoak::meshclient::kAwaitTrace, r.job.awaitKind());

    ClientRig three;
    const Case* refused = findCase("response", kMsgSendTrace, 32);
    ASSERT_NE(nullptr, refused);
    std::vector<uint8_t> payload = req->payload;
    payload[4] = 3;  // hash_size
    payload.push_back(0x00);
    payload.push_back(0x00);
    payload.push_back(0x00);
    payload.push_back(0x00);  // 12 bytes: four three-byte hops
    EXPECT_EQ(toHex(refused->frame), three.answer(frameOf(kMsgSendTrace, 32, payload)));
    EXPECT_TRUE(three.port.sent.empty());
  }
}

TEST(TheFiveSends, WhatCannotBeSentIsRefusedBeforeAnythingIsBuilt) {
  ClientRig r;
  r.beginJob(kPathNone);
  struct Bad {
    const char* what;
    uint8_t type;
    std::vector<uint8_t> payload;
    uint8_t status;
  };
  const std::vector<Bad> bad = {
      {"a password of 16 bytes", kMsgSendLogin,
       sendPayload(1000, std::vector<uint8_t>(16, 'p')), kErrMalformed},
      {"a CLI line of 149 bytes", kMsgSendCli,
       sendPayload(1000, [] {
         std::vector<uint8_t> b = {0x00};
         b.insert(b.end(), 149, 'x');
         return b;
       }()),
       kErrMalformed},
      {"an empty CLI line", kMsgSendCli, sendPayload(1000, {0x00}), kErrMalformed},
      {"request data of 163 bytes", kMsgSendRequest,
       sendPayload(1000, [] {
         std::vector<uint8_t> b = {0x01};
         b.insert(b.end(), 163, 0xab);
         return b;
       }()),
       kErrRange},
      {"a trace with hash_size 5", kMsgSendTrace, sendPayload(1000, {5, 1, 2, 3, 4, 5}),
       kErrMalformed},
      {"a trace with hash_size 0", kMsgSendTrace, sendPayload(1000, {0, 1, 2}), kErrMalformed},
      {"a trace with no hops", kMsgSendTrace, sendPayload(1000, {2}), kErrMalformed},
      {"a trace whose bytes are not whole hops", kMsgSendTrace, sendPayload(1000, {2, 1, 2, 3}),
       kErrMalformed},
      {"a trace of 176 bytes of hashes", kMsgSendTrace,
       sendPayload(1000, [] {
         std::vector<uint8_t> b = {1};
         b.insert(b.end(), 176, 0x7f);
         return b;
       }()),
       kErrRange},
      {"a send three bytes short of its timeout", kMsgSendDiscovery, {0x01, 0x02, 0x03},
       kErrMalformed},
  };
  uint8_t seq = 50;
  for (const Bad& b : bad) {
    r.answer(frameOf(b.type, seq++, b.payload));
    EXPECT_EQ(b.status, r.lastStatus()) << b.what;
  }
  EXPECT_TRUE(r.port.sent.empty()) << "nothing reached the port";
  EXPECT_EQ(0u, r.client.clientTx());
  EXPECT_FALSE(r.job.awaiting());

  // Ten refusals and the ceiling is untouched: none of them was a send, so the next legitimate
  // one is admitted at once (src/meshclient/SendCeiling.h).
  EXPECT_EQ(0u, r.client.refused()) << "nothing above was refused by the ceiling";

  // And the two that are legal and nearly were not: 148 bytes of line, 162 bytes of data.
  std::vector<uint8_t> line = {0x00};
  line.insert(line.end(), 148, 'y');
  r.answer(frameOf(kMsgSendCli, 70, sendPayload(0, line)));
  EXPECT_EQ(kOk, r.lastStatus());
  r.gap();
  std::vector<uint8_t> data = {0x01};
  data.insert(data.end(), 162, 0xab);
  r.answer(frameOf(kMsgSendRequest, 71, sendPayload(0, data)));
  EXPECT_EQ(kOk, r.lastStatus());
  EXPECT_EQ(2u, r.port.sent.size());
  EXPECT_EQ(167u, r.port.sent[1].plain.size()) << "the longest plaintext a datagram carries";

  // A pool with no packet left is ERR_FAILED: tried and failed, nothing counted, nothing awaited.
  r.gap();
  r.port.fail = true;
  r.answer(requestFrame(kMsgSendLogin, 23, 72));
  EXPECT_EQ(kErrFailed, r.lastStatus());
  EXPECT_EQ(2u, r.client.clientTx());
  EXPECT_FALSE(r.job.awaiting());
}

TEST(TheFiveSends, TheCeilingRefusesTheSeventhSendOfAMinuteAndTheOneInsideTwoSeconds) {
  // The backstop of tasks/phase-2/04-the-rails.md, through the real session and the real client:
  // at most six sends in any sixty seconds, at least two seconds apart, over the ceiling
  // ERR_BUSY and counted. The limiter itself is tested against a fake clock in
  // test/test_meshclient; this is that limiter where the Walter meets it.
  ClientRig r;
  r.beginJob(kPathNone);
  const uint32_t first = r.h.now;
  uint8_t seq = 80;
  for (uint8_t i = 0; i < twinoak::meshclient::kCeilingSends; i++) {
    if (i > 0) r.gap();
    r.answer(frameOf(kMsgSendRequest, seq++, sendPayload(0, {0x01})));
    ASSERT_EQ(kOk, r.lastStatus()) << "send " << int(i) << " of the minute";
  }
  EXPECT_EQ(uint32_t(twinoak::meshclient::kCeilingSends), r.client.clientTx());
  EXPECT_EQ(size_t(twinoak::meshclient::kCeilingSends), r.port.sent.size());
  EXPECT_EQ(0u, r.client.refused());

  // The seventh, with the gap kept and nothing awaited: refused, nothing on the air, counted.
  r.gap();
  r.answer(frameOf(kMsgSendRequest, seq++, sendPayload(0, {0x01})));
  EXPECT_EQ(kErrBusy, r.lastStatus()) << "the seventh send inside the window";
  EXPECT_FALSE(r.job.awaiting()) << "STATUS flag bit 1 is clear: this ERR_BUSY is the ceiling";
  EXPECT_EQ(size_t(twinoak::meshclient::kCeilingSends), r.port.sent.size());
  EXPECT_EQ(uint32_t(twinoak::meshclient::kCeilingSends), r.client.clientTx())
      << "a refused send is not counted as one";
  EXPECT_EQ(1u, r.client.refused());

  // Every one of the five is refused the same way while the window is full, and each try costs
  // the Walter nothing but the count.
  struct Refused {
    uint8_t type;
    uint8_t fixture_seq;
  };
  const Refused others[] = {{kMsgSendLogin, 23},
                            {kMsgSendCli, 28},
                            {kMsgSendDiscovery, 30},
                            {kMsgSendTrace, 31}};
  for (const Refused& o : others) {
    r.answer(requestFrame(o.type, o.fixture_seq, seq++));
    EXPECT_EQ(kErrBusy, r.lastStatus()) << "type 0x" << std::hex << int(o.type) << std::dec;
  }
  EXPECT_EQ(5u, r.client.refused());
  EXPECT_EQ(size_t(twinoak::meshclient::kCeilingSends), r.port.sent.size());

  // The window slides. The six went at `first`, `first + 2000`, ... `first + 10000`; one window
  // after the first of them that one has left it and exactly one more send goes.
  r.h.now = first + twinoak::meshclient::kCeilingWindowMs;
  r.answer(frameOf(kMsgSendRequest, seq++, sendPayload(0, {0x01})));
  EXPECT_EQ(kOk, r.lastStatus()) << "the oldest of the six has left the window";
  EXPECT_EQ(uint32_t(twinoak::meshclient::kCeilingSends) + 1, r.client.clientTx());

  // And the gap, which is the limit a mistake meets first: a millisecond short of two seconds the
  // refusal is the gap's, not the window's - four of the six are still inside it.
  r.h.now += twinoak::meshclient::kCeilingGapMs - 1;
  r.answer(frameOf(kMsgSendRequest, seq++, sendPayload(0, {0x01})));
  EXPECT_EQ(kErrBusy, r.lastStatus()) << "a millisecond short of the two-second gap";
  EXPECT_EQ(6u, r.client.refused());
  r.h.now += 1;
  r.answer(frameOf(kMsgSendRequest, seq++, sendPayload(0, {0x01})));
  EXPECT_EQ(kOk, r.lastStatus()) << "the gap, exactly";
}

TEST(TheFiveSends, ARetriedSendTransmitsNothingAndAnswersTheSame) {
  // Section 3's one-entry memory, where it matters most: "a lost response must never become a
  // second transmission".
  ClientRig r;
  r.beginJob(kPathNone);
  const std::vector<uint8_t> frame = requestFrame(kMsgSendRequest, 26, 90);
  const std::string first = r.answer(frame);
  EXPECT_EQ(kOk, r.lastStatus());
  EXPECT_EQ(1u, r.port.sent.size());
  EXPECT_EQ(1u, r.h.session.executed());

  r.port.time += 100;  // the clock moved on; the remembered answer did not
  EXPECT_EQ(first, r.answer(frame)) << "the remembered response, byte for byte";
  EXPECT_EQ(1u, r.port.sent.size()) << "one call into the mesh";
  EXPECT_EQ(1u, r.client.clientTx()) << "client_tx rose by exactly one for the two frames";
  EXPECT_EQ(1u, r.h.session.executed());
}

TEST(TheFiveSends, NoBufferTheSessionOrTheClientOwnsHoldsThePasswordAfterwards) {
  // The two sends that carry a password: a login, and a CLI line flagged sensitive. After each,
  // the bytes of the password are searched for in the whole of the Session, the client, the job
  // and the reply record - and in the receiver's buffers the request arrived in - and found
  // nowhere. The fake port's own copy of the plaintext is the test's and is what proves the
  // password *did* go into the packet.
  {
    ClientRig r;
    r.beginJob(kPathNone);
    const Case* req = findCase("request", kMsgSendLogin, 23);
    ASSERT_NE(nullptr, req);
    const std::vector<uint8_t> pw = fromHex(req->fields["password"].as<std::string>());
    ASSERT_GE(pw.size(), 4u);
    r.answer(req->frame);
    EXPECT_EQ(kOk, r.lastStatus());
    ASSERT_EQ(1u, r.port.sent.size());
    EXPECT_TRUE(holdsBytes(r.port.sent[0].plain.data(), r.port.sent[0].plain.size(), pw))
        << "the plaintext handed to the board carried it";
    EXPECT_FALSE(holdsBytes(&r.h.session, sizeof(r.h.session), pw)) << "the session";
    EXPECT_FALSE(holdsBytes(&r.client, sizeof(r.client), pw)) << "the client and its reply";
    EXPECT_FALSE(holdsBytes(&r.job, sizeof(r.job), pw)) << "the job";
    // The retry memory holds the response and not the request: a retry answers the same
    // without the password ever having been kept.
    const std::string first = r.h.lastFrame();
    EXPECT_EQ(first, r.answer(req->frame));
    EXPECT_FALSE(holdsBytes(&r.h.session, sizeof(r.h.session), pw));
  }
  {
    ClientRig r;
    r.beginJob(kPathDirect);
    const Case* req = findCase("request", kMsgSendCli, 29);  // "cli line made from a password"
    ASSERT_NE(nullptr, req);
    const std::string text = req->fields["text"].as<std::string>();
    const std::vector<uint8_t> line(text.begin(), text.end());
    EXPECT_EQ(1, req->fields["flags"].as<int>()) << "flagged sensitive";
    r.answer(req->frame);
    EXPECT_EQ(kOk, r.lastStatus());
    ASSERT_EQ(1u, r.port.sent.size());
    EXPECT_TRUE(holdsBytes(r.port.sent[0].plain.data(), r.port.sent[0].plain.size(), line));
    EXPECT_FALSE(holdsBytes(&r.h.session, sizeof(r.h.session), line)) << "the session";
    EXPECT_FALSE(holdsBytes(&r.client, sizeof(r.client), line)) << "the client";
    EXPECT_FALSE(holdsBytes(&r.job, sizeof(r.job), line)) << "the job";
    // Eight bytes of the line - the password's own - are not there either, in case the whole
    // line was merely split.
    const std::vector<uint8_t> tail(line.end() - 8, line.end());
    EXPECT_FALSE(holdsBytes(&r.h.session, sizeof(r.h.session), tail));
    EXPECT_FALSE(holdsBytes(&r.client, sizeof(r.client), tail));
  }
  // And the receiver itself: fed a frame and wiped, its bytes are zero.
  {
    FrameReceiver rx;
    const std::vector<uint8_t> frame = frameOf(kMsgPing, 1, {0x11, 0x22, 0x33, 0x44});
    for (uint8_t b : frame) rx.feedByte(b);
    ASSERT_TRUE(rx.hasPacket());
    EXPECT_TRUE(holdsBytes(&rx, sizeof(rx), {0x11, 0x22, 0x33, 0x44}));
    rx.wipe();
    EXPECT_FALSE(rx.hasPacket());
    EXPECT_FALSE(holdsBytes(&rx, sizeof(rx), {0x11, 0x22, 0x33, 0x44}));
    EXPECT_EQ(0, rx.drops()) << "the counters are kept";
  }
}

// ----------------------------------------------------------------------------- the reply

TEST(TheReply, ReplyGetIsTheFixturesFramesWhileWaitingAndWhenNothingIsHeld) {
  // "reply get: still waiting" (seq 33): the neighbours request of seq 27 sent 1200 ms ago.
  {
    ClientRig r;
    r.beginJob(kPathHops, {0x7f, 0x3a}, 2);
    r.port.time = 1791720051;
    r.answer(requestFrame(kMsgSendRequest, 27, 27));
    EXPECT_EQ(kOk, r.lastStatus());
    r.h.now += 1200;
    const Case* req = findCase("request", kMsgReplyGet, 33);
    const Case* resp = findCase("response", kMsgReplyGet, 33);
    ASSERT_NE(nullptr, req);
    ASSERT_NE(nullptr, resp);
    EXPECT_EQ(toHex(resp->frame), r.answer(req->frame));
  }
  // "reply get: nothing held" (seq 34).
  {
    ClientRig r;
    const Case* resp = findCase("response", kMsgReplyGet, 34);
    ASSERT_NE(nullptr, resp);
    EXPECT_EQ(toHex(resp->frame), r.answer(frameOf(kMsgReplyGet, 34, {})));
  }
}

TEST(TheReply, TheFourEventsOfTheFixtureByteForByte) {
  // Each event's payload, with the arrival scripted the way the board's peer overrides would
  // report it (src/app/TwinOakMesh.cpp). The frames are compared by payload: the fixture's events
  // carry a seq of their own and the session counts its own from 0.
  //
  // "a response that came back as a path": a flooded request answered with a returned path, the
  // response riding in it; the path is learned before the event is sent.
  {
    ClientRig r;
    r.beginJob(kPathNone);
    r.port.time = 1791720051;
    const uint32_t sent_at = r.h.now;
    r.answer(requestFrame(kMsgSendRequest, 27, 27));
    EXPECT_EQ(kOk, r.lastStatus());
    const Case* want = findCaseNamed("mesh reply: a response that came back as a path");
    ASSERT_NE(nullptr, want);
    r.h.now = sent_at + want->fields["elapsed_ms"].as<uint32_t>();
    const uint8_t hops[] = {0x7f, 0x3a};
    EXPECT_TRUE(r.client.onReturnedPath(twinoak::meshclient::encodePathLen(2, 1), hops));
    std::vector<uint8_t> plain = le32(want->fields["tag"].as<uint32_t>());
    const std::vector<uint8_t> data = fromHex(want->fields["data"].as<std::string>());
    plain.insert(plain.end(), data.begin(), data.end());
    EXPECT_TRUE(r.client.onResponse(plain.data(), plain.size(), /*via_path=*/true,
                                    want->fields["snr_q4"].as<int8_t>(),
                                    want->fields["rssi"].as<int16_t>(), r.h.now));
    EXPECT_FALSE(r.job.awaiting()) << "the reply arrived: the extra peer leaves the front";
    EXPECT_EQ(kPathHops, r.job.pathMode()) << "the returned path is the job's now";
    EXPECT_EQ(1u, r.job.pathHopCount());

    r.h.sent.clear();
    r.h.run();
    ASSERT_EQ(1u, r.h.sent.size()) << "one EV_MESH_REPLY";
    uint8_t type = 0, seq = 0;
    EXPECT_EQ(toHex(want->payload), toHex(lastBody(r.h, type, seq)));
    EXPECT_EQ(kMsgEvMeshReply, type);
    r.h.run();
    EXPECT_EQ(1u, r.h.sent.size()) << "and only one";

    // REPLY_GET afterwards is the same record, and PATH_GET reads the learned route.
    r.answer(frameOf(kMsgReplyGet, 60, {}));
    std::vector<uint8_t> body = lastBody(r.h, type, seq);
    ASSERT_GE(body.size(), 2u);
    EXPECT_EQ(kOk, body[0]);
    EXPECT_EQ(kReplyResponse, body[1]);
    EXPECT_EQ(toHex(want->payload), toHex(std::vector<uint8_t>(body.begin() + 1, body.end())));
    r.answer(frameOf(kMsgPathGet, 61, {}));
    body = lastBody(r.h, type, seq);
    EXPECT_EQ("000202017f3a", toHex(body)) << "OK; hops, size 2, one hop, 7f3a";
  }
  // "a cli reply": the first text of CLI data from the peer while a text is awaited.
  {
    ClientRig r;
    r.beginJob(kPathDirect);
    const uint32_t sent_at = r.h.now;
    r.answer(requestFrame(kMsgSendCli, 28, 28));
    EXPECT_EQ(kOk, r.lastStatus());
    const Case* want = findCaseNamed("mesh reply: a cli reply");
    ASSERT_NE(nullptr, want);
    r.h.now = sent_at + want->fields["elapsed_ms"].as<uint32_t>();
    // The repeater's TXT_MSG: its own timestamp, TXT_TYPE_CLI_DATA << 2, the text, and the
    // zero padding MeshCore adds to a cipher block.
    std::vector<uint8_t> plain = le32(1791720099);
    plain.push_back(0x04);
    const std::string text = want->fields["text"].as<std::string>();
    plain.insert(plain.end(), text.begin(), text.end());
    while (plain.size() % 16 != 0) plain.push_back(0);
    EXPECT_TRUE(r.client.onText(plain.data(), plain.size(), want->fields["snr_q4"].as<int8_t>(),
                                want->fields["rssi"].as<int16_t>(), r.h.now));
    r.h.sent.clear();
    r.h.run();
    ASSERT_EQ(1u, r.h.sent.size());
    uint8_t type = 0, seq = 0;
    EXPECT_EQ(toHex(want->payload), toHex(lastBody(r.h, type, seq)));
    EXPECT_EQ(kMsgEvMeshReply, type);
  }
  // "a trace": the completed trace with the awaited tag, one SNR per hop.
  {
    ClientRig r;
    r.port.time = 1791720054;
    const uint32_t sent_at = r.h.now;
    r.answer(requestFrame(kMsgSendTrace, 31, 31));
    EXPECT_EQ(kOk, r.lastStatus());
    const Case* want = findCaseNamed("mesh reply: a trace");
    ASSERT_NE(nullptr, want);
    r.h.now = sent_at + want->fields["elapsed_ms"].as<uint32_t>();
    const std::vector<uint8_t> hashes = fromHex(want->fields["hashes"].as<std::string>());
    const std::vector<uint8_t> snrs = fromHex(want->fields["snrs"].as<std::string>());
    EXPECT_TRUE(r.client.onTrace(want->fields["tag"].as<uint32_t>(),
                                 want->fields["trace_flags"].as<uint8_t>(), hashes.data(),
                                 hashes.size(), snrs.data(), snrs.size(),
                                 want->fields["final_snr_q4"].as<int8_t>(),
                                 want->fields["rssi"].as<int16_t>(), r.h.now));
    r.h.sent.clear();
    r.h.run();
    ASSERT_EQ(1u, r.h.sent.size());
    uint8_t type = 0, seq = 0;
    EXPECT_EQ(toHex(want->payload), toHex(lastBody(r.h, type, seq)));
  }
  // "silence": the wait ran out, found by the loop and not by a reply.
  {
    ClientRig r;
    r.beginJob(kPathNone);
    r.port.time = 1791720055;
    const Case* want = findCaseNamed("mesh reply: silence");
    ASSERT_NE(nullptr, want);
    const uint32_t timeout = want->fields["elapsed_ms"].as<uint32_t>();
    r.answer(frameOf(kMsgSendRequest, 35, sendPayload(timeout, {0x01})));
    EXPECT_EQ(kOk, r.lastStatus());
    r.h.now += timeout - 1;
    r.h.sent.clear();
    r.h.run();
    EXPECT_TRUE(r.h.sent.empty()) << "one millisecond short";
    EXPECT_TRUE(r.job.awaiting());
    r.h.now += 1;
    r.h.run();
    ASSERT_EQ(1u, r.h.sent.size());
    uint8_t type = 0, seq = 0;
    EXPECT_EQ(toHex(want->payload), toHex(lastBody(r.h, type, seq)));
    EXPECT_EQ(kMsgEvMeshReply, type);
    EXPECT_FALSE(r.job.awaiting());
    EXPECT_EQ(kReplyTimeout, r.client.reply().state());
    // A reply that arrives late is dropped: the record is no longer pending.
    const std::vector<uint8_t> late = le32(1791720055);
    EXPECT_FALSE(r.client.onResponse(late.data(), late.size(), false, 0, 0, r.h.now));
    EXPECT_EQ(kReplyTimeout, r.client.reply().state());
  }
}

TEST(TheReply, WhatIsNotTheAwaitedReplyIsDroppedThroughTheClient) {
  ClientRig r;
  r.beginJob(kPathNone);
  r.port.time = 1000;
  r.answer(requestFrame(kMsgSendRequest, 26, 26));  // awaits a response tagged 1000
  EXPECT_EQ(kOk, r.lastStatus());
  const std::vector<uint8_t> other = le32(999);
  EXPECT_FALSE(r.client.onResponse(other.data(), other.size(), false, 0, 0, r.h.now))
      << "another tag";
  std::vector<uint8_t> text = le32(5);
  text.push_back(0x04);
  text.push_back('x');
  EXPECT_FALSE(r.client.onText(text.data(), text.size(), 0, 0, r.h.now))
      << "a text while a response is awaited";
  EXPECT_FALSE(r.client.onTrace(1000, 1, nullptr, 0, nullptr, 0, 0, 0, r.h.now))
      << "a trace while a response is awaited";
  EXPECT_TRUE(r.job.awaiting());
  EXPECT_EQ(kReplyPending, r.client.reply().state());
  const std::vector<uint8_t> right = le32(1000);
  EXPECT_TRUE(r.client.onResponse(right.data(), right.size(), false, 0, 0, r.h.now));
  EXPECT_FALSE(r.client.onResponse(right.data(), right.size(), false, 0, 0, r.h.now))
      << "the first wins";

  // A CLI line: a response is dropped, a text of another type is dropped.
  r.h.sent.clear();
  r.h.run();  // the event goes
  r.gap();
  r.answer(requestFrame(kMsgSendCli, 28, 28));
  EXPECT_EQ(kOk, r.lastStatus());
  EXPECT_FALSE(r.client.onResponse(right.data(), right.size(), false, 0, 0, r.h.now))
      << "a response while a text is awaited";
  text[4] = uint8_t(2 << 2);  // TXT_TYPE_SIGNED_PLAIN
  EXPECT_FALSE(r.client.onText(text.data(), text.size(), 0, 0, r.h.now))
      << "a text that is not CLI data";
  text[4] = 0x04;
  EXPECT_TRUE(r.client.onText(text.data(), text.size(), 0, 0, r.h.now));
  EXPECT_EQ(kReplyText, r.client.reply().state());
}

TEST(TheReply, WhileTheWalterSleepsTheEventIsHeldAndReplyGetIsTheFallback) {
  ClientRig r;
  r.beginJob(kPathNone);
  r.port.time = 2000;
  r.answer(requestFrame(kMsgSendRequest, 26, 26));
  r.h.attention.asleep = true;  // a SLEEP_NOTICE arrived meanwhile
  const std::vector<uint8_t> reply = le32(2000);
  EXPECT_TRUE(r.client.onResponse(reply.data(), reply.size(), false, 8, -90, r.h.now));
  r.h.sent.clear();
  r.h.run();
  EXPECT_TRUE(r.h.sent.empty()) << "no event leaves while the Walter is believed asleep";
  r.h.attention.asleep = false;
  r.h.run();
  EXPECT_TRUE(r.h.sent.empty()) << "and the outcome is reported once: it is not sent later";
  r.answer(frameOf(kMsgReplyGet, 5, {}));
  uint8_t type = 0, seq = 0;
  const std::vector<uint8_t> body = lastBody(r.h, type, seq);
  ASSERT_GE(body.size(), 2u);
  EXPECT_EQ(kOk, body[0]);
  EXPECT_EQ(kReplyResponse, body[1]) << "REPLY_GET has the record";
}

TEST(TheReply, TheRecordIsLetGoByTheNextSendJobBeginAndJobEnd) {
  ClientRig r;
  r.beginJob(kPathNone);
  r.port.time = 3000;
  r.answer(requestFrame(kMsgSendRequest, 26, 26));
  const std::vector<uint8_t> reply = le32(3000);
  EXPECT_TRUE(r.client.onResponse(reply.data(), reply.size(), false, 0, 0, r.h.now));
  r.h.run();
  EXPECT_EQ(kReplyResponse, r.client.reply().state());

  // A send with timeout 0 awaits nothing and lets the record go.
  r.gap();
  r.answer(frameOf(kMsgSendRequest, 27, sendPayload(0, {0x01})));
  EXPECT_EQ(kOk, r.lastStatus());
  EXPECT_FALSE(r.job.awaiting());
  EXPECT_EQ(kReplyNone, r.client.reply().state());
  EXPECT_EQ(2u, r.client.clientTx()) << "counted all the same";

  // JOB_END while a reply is awaited: nothing awaited, nothing held, no event.
  r.gap();
  r.answer(frameOf(kMsgSendRequest, 28, sendPayload(5000, {0x01})));
  EXPECT_TRUE(r.job.awaiting());
  r.answer(frameOf(kMsgJobEnd, 29, {}));
  EXPECT_FALSE(r.job.awaiting());
  EXPECT_EQ(kReplyNone, r.client.reply().state());
  r.h.now += 10000;
  r.h.sent.clear();
  r.h.run();
  EXPECT_TRUE(r.h.sent.empty()) << "no timeout event for a wait JOB_END ended";

  // JOB_BEGIN likewise.
  r.beginJob(kPathNone);
  r.answer(frameOf(kMsgSendRequest, 30, sendPayload(5000, {0x01})));
  EXPECT_TRUE(r.job.awaiting());
  r.answer(requestFrame(kMsgJobBegin, 16, 31));
  EXPECT_EQ(kOk, r.lastStatus());
  EXPECT_FALSE(r.job.awaiting());
  EXPECT_EQ(kReplyNone, r.client.reply().state());

  // And the silence rule, which ends the job by itself.
  r.gap();
  r.answer(frameOf(kMsgSendRequest, 32, sendPayload(500000, {0x01})));
  EXPECT_TRUE(r.job.awaiting());
  r.h.now += twinoak::meshclient::kIdleEndMs;
  r.client.loop(r.h.now);
  EXPECT_FALSE(r.job.begun());
  EXPECT_FALSE(r.job.awaiting());
  EXPECT_EQ(kReplyNone, r.client.reply().state());
}

TEST(TheReply, AReturnedPathOfZeroHopsIsTheDirectMode) {
  ClientRig r;
  r.beginJob(kPathNone);
  EXPECT_TRUE(r.client.onReturnedPath(0, nullptr));
  EXPECT_EQ(kPathDirect, r.job.pathMode()) << "a direct neighbour answered";
  const uint8_t three[] = {1, 2, 3, 4, 5, 6};
  EXPECT_TRUE(r.client.onReturnedPath(twinoak::meshclient::encodePathLen(3, 2), three));
  EXPECT_EQ(kPathHops, r.job.pathMode());
  EXPECT_EQ(3u, r.job.pathHashSize());
  // A hash size the link does not allow (4, reserved upstream too) leaves the path as it was.
  EXPECT_FALSE(r.client.onReturnedPath(twinoak::meshclient::encodePathLen(4, 1), three));
  EXPECT_EQ(3u, r.job.pathHashSize());
  EXPECT_EQ(2u, r.job.pathHopCount());
}

// ----------------------------------------------------------------------------- the counters

TEST(TheCounters, MeshCountersReadsTheRadioTheClientAndTheRing) {
  // The fixture's "mesh counters response" (seq 37), field by field: rx, tx, noise floor, RSSI
  // and SNR are the radio's and the fake port answers them; client_tx is this client's and is
  // made to happen with 42 sends; the three advert counts are the observation ring's (task 03)
  // and the fake ring answers them. A comparison of fields and not of the frame, because
  // client_tx has to be *made* to happen.
  const Case* want = findCase("response", kMsgMeshCounters, 37);
  ASSERT_NE(nullptr, want);
  JsonObject f = want->fields;

  ClientRig r;
  r.beginJob(kPathNone);
  r.port.counters.rx = f["rx"].as<uint32_t>();
  r.port.counters.tx = f["tx"].as<uint32_t>();
  r.port.counters.noise_floor = f["noise_floor"].as<int16_t>();
  r.port.counters.last_rssi = f["last_rssi"].as<int16_t>();
  r.port.counters.last_snr_q4 = f["last_snr_q4"].as<int8_t>();
  r.h.observations.heard = f["adverts"].as<uint32_t>();
  r.h.observations.heard_dropped = f["adverts_dropped"].as<uint32_t>();
  r.h.observations.obs_lost = f["obs_dropped"].as<uint32_t>();
  const uint32_t sends = f["client_tx"].as<uint32_t>();
  for (uint32_t i = 0; i < sends; i++) {
    // Twenty seconds apart, which is the gap the MeshMaster server's default budget asks of the
    // Walter: 42 sends at that pace never meet the RAK's ceiling of six a minute, which is the
    // whole point of where the ceiling was put (src/meshclient/SendCeiling.h).
    if (i > 0) r.h.now += 20000;
    r.answer(frameOf(kMsgSendRequest, uint8_t(100 + i), sendPayload(0, {0x01})));
    ASSERT_EQ(kOk, r.lastStatus()) << "send " << i;
  }
  EXPECT_EQ(0u, r.client.refused()) << "a paced Walter is never refused";
  EXPECT_EQ(sends, r.client.clientTx());
  EXPECT_EQ(size_t(sends), r.port.sent.size());

  r.answer(frameOf(kMsgMeshCounters, 37, {}));
  uint8_t type = 0, seq = 0;
  const std::vector<uint8_t> body = lastBody(r.h, type, seq);
  EXPECT_EQ(responseType(kMsgMeshCounters), type);
  Reader rd(body.data(), body.size());
  EXPECT_EQ(kOk, rd.u8());
  EXPECT_EQ(f["rx"].as<uint32_t>(), rd.u32());
  EXPECT_EQ(f["tx"].as<uint32_t>(), rd.u32());
  EXPECT_EQ(f["client_tx"].as<uint32_t>(), rd.u32());
  EXPECT_EQ(f["adverts"].as<uint32_t>(), rd.u32());
  EXPECT_EQ(f["adverts_dropped"].as<uint32_t>(), rd.u32());
  EXPECT_EQ(f["obs_dropped"].as<uint32_t>(), rd.u32());
  EXPECT_EQ(f["noise_floor"].as<int16_t>(), rd.i16());
  EXPECT_EQ(f["last_rssi"].as<int16_t>(), rd.i16());
  EXPECT_EQ(f["last_snr_q4"].as<int8_t>(), rd.i8());
  EXPECT_TRUE(rd.ok());
  EXPECT_EQ(0u, rd.remaining());
  EXPECT_EQ(want->payload.size(), body.size()) << "the same shape as the fixture's";
}

TEST(TheCounters, WithNoMeshClientTheEightAreUnsupported) {
  Harness h;
  h.services.mesh = nullptr;
  const uint8_t types[] = {kMsgSendLogin, kMsgSendRequest, kMsgSendCli, kMsgSendDiscovery,
                           kMsgSendTrace, kMsgReplyGet, kMsgMeshCounters};
  uint8_t seq = 1;
  for (uint8_t type : types) {
    h.feed(frameOf(type, seq, sendPayload(0, {0x01, 0x02})));
    h.run();
    EXPECT_EQ(toHex(frameOf(responseType(type), seq, {kErrUnsupported})), h.lastFrame())
        << "type 0x" << std::hex << int(type);
    seq++;
  }
  EXPECT_EQ(7u, h.sent.size());
}

// ----------------------------------------------------------------------------- observations

namespace {

// A session with the **real** observation ring behind it (src/meshclient/), so that OBS_PULL, the
// retry, STATUS's two fields and MESH_COUNTERS' three are the firmware's own code end to end.
struct ObsRig {
  Harness h;
  twinoak::meshclient::ObservationRing ring;
  twinoak::meshclient::LinkObservations obs{ring};

  ObsRig() { h.services.observations = &obs; }

  // One advert heard, as TwinOakMesh::onAdvertRecv builds the record.
  void hear(uint8_t key_byte, uint32_t adv_ts, uint32_t heard_ms, uint8_t app_len = 4) {
    twinoak::meshclient::HeardAdvert a;
    for (uint8_t i = 0; i < twinoak::meshclient::kObsPubKeyLen; i++) {
      a.pubkey[i] = uint8_t(key_byte + i);
    }
    a.adv_ts = adv_ts;
    a.heard_ms = heard_ms;
    a.snr_q4 = 24;
    a.rssi = -90;
    a.app_len = app_len;
    for (uint8_t i = 0; i < app_len; i++) a.app_data[i] = uint8_t(0x30 + i);
    EXPECT_TRUE(ring.push(a));
  }

  // One request in, its response body out.
  std::vector<uint8_t> answer(const std::vector<uint8_t>& frame) {
    h.feed(frame);
    h.run();
    uint8_t type = 0, seq = 0;
    return lastBody(h, type, seq);
  }
};

}  // namespace

TEST(Observations, TheRingIsEmptiedOldestFirstAndTheRecordsLeaveIt) {
  ObsRig r;
  r.h.now = 100000;
  for (uint8_t i = 0; i < 3; i++) r.hear(i, 1791719000u + i, 100000u - 1000u * (3 - i));
  EXPECT_EQ(3u, r.obs.pending());

  const std::vector<uint8_t> body = r.answer(frameOf(kMsgObsPull, 7, {2}));
  Reader rd(body.data(), body.size());
  EXPECT_EQ(kOk, rd.u8());
  EXPECT_EQ(1u, rd.u8()) << "remaining";
  EXPECT_EQ(0u, rd.u16()) << "dropped";
  ASSERT_EQ(2u, rd.u8()) << "count";
  // The oldest first, with the age measured on the millisecond clock.
  Bytes key = rd.take(kPubKeyLen);
  ASSERT_EQ(size_t(kPubKeyLen), key.len);
  EXPECT_EQ(0u, key.ptr[0]) << "the first advert heard";
  EXPECT_EQ(1791719000u, rd.u32()) << "adv_ts";
  EXPECT_EQ(3u, rd.u32()) << "age_s: heard 3000 ms ago";
  EXPECT_EQ(24, rd.i8());
  EXPECT_EQ(-90, rd.i16());
  EXPECT_EQ(0u, rd.u8()) << "path_len: a direct neighbour";
  EXPECT_EQ(4u, rd.u8()) << "app_len";
  rd.take(4);
  key = rd.take(kPubKeyLen);
  EXPECT_EQ(1u, key.ptr[0]) << "then the second";
  EXPECT_TRUE(rd.ok());

  EXPECT_EQ(1u, r.obs.pending()) << "the two handed out left the ring";
}

TEST(Observations, ARepeatedPullGetsTheSameRecordsAndTheRingIsDrainedOnce) {
  // "Because a response can be repeated and the records are gone from the ring once sent, the
  // remembered response of a pull is what a retry gets - the same records, not the next ones"
  // (section 7). The frame is byte for byte the same and the ring is one pull lighter, not two.
  ObsRig r;
  r.h.now = 50000;
  for (uint8_t i = 0; i < 5; i++) r.hear(uint8_t(0x10 + i), 1700u + i, 50000u);
  const std::vector<uint8_t> frame = frameOf(kMsgObsPull, 9, {2});

  r.h.feed(frame);
  r.h.run();
  ASSERT_EQ(1u, r.h.sent.size());
  const std::string first = r.h.lastFrame();
  EXPECT_EQ(3u, r.obs.pending());
  EXPECT_EQ(1u, r.h.session.executed());

  // The same (type, seq) again: no handler runs, the remembered response goes out again.
  r.h.feed(frame);
  r.h.run();
  ASSERT_EQ(2u, r.h.sent.size());
  EXPECT_EQ(first, r.h.lastFrame()) << "the same records, byte for byte";
  EXPECT_EQ(3u, r.obs.pending()) << "and the ring was drained once";
  EXPECT_EQ(1u, r.h.session.executed()) << "one handler call for two frames";

  // A new seq is a new pull, and it hands out the next ones.
  r.h.feed(frameOf(kMsgObsPull, 10, {2}));
  r.h.run();
  EXPECT_NE(first, r.h.lastFrame());
  EXPECT_EQ(1u, r.obs.pending());
  EXPECT_EQ(2u, r.h.session.executed());
}

TEST(Observations, StatusAndMeshCountersReadTheRealRing) {
  ObsRig r;
  r.h.now = 10000;
  // Overflow it: 64 kept, three lost.
  for (int i = 0; i < twinoak::meshclient::kObsCapacity + 3; i++) {
    r.hear(uint8_t(i), uint32_t(2000 + i), 10000u);
  }
  // And one record the ring cannot carry, which is an advert dropped and not a ring overflow.
  twinoak::meshclient::HeardAdvert bad;
  bad.path_len = 0xFF;  // a four-byte hash size: 252 bytes of path
  EXPECT_FALSE(r.ring.push(bad));

  const std::vector<uint8_t> status = r.answer(frameOf(kMsgStatus, 11, {}));
  Reader rd(status.data(), status.size());
  EXPECT_EQ(kOk, rd.u8());
  rd.u32();  // uptime_s
  rd.u8();   // reset_reason
  rd.u8();   // time_state
  rd.u32();  // epoch
  rd.u16();  // flags
  EXPECT_EQ(twinoak::meshclient::kObsCapacity, rd.u8()) << "obs_pending";
  EXPECT_EQ(3u, rd.u16()) << "obs_dropped, saturating";

  r.h.sent.clear();
  const std::vector<uint8_t> counters = r.answer(frameOf(kMsgMeshCounters, 12, {}));
  Reader rc(counters.data(), counters.size());
  EXPECT_EQ(kOk, rc.u8());
  rc.u32();  // rx
  rc.u32();  // tx
  rc.u32();  // client_tx
  EXPECT_EQ(uint32_t(twinoak::meshclient::kObsCapacity) + 4u, rc.u32()) << "adverts";
  EXPECT_EQ(1u, rc.u32()) << "adverts_dropped";
  EXPECT_EQ(3u, rc.u32()) << "obs_dropped";
  EXPECT_TRUE(rc.ok());
}

TEST(Observations, WithNoRingObsPullIsUnsupportedAndTheCountsReadZero) {
  Harness h;
  h.services.observations = nullptr;
  EXPECT_EQ(0u, capsOf(h.services) & kCapObservations);

  h.feed(frameOf(kMsgObsPull, 3, {4}));
  h.run();
  EXPECT_EQ(toHex(frameOf(responseType(kMsgObsPull), 3, {kErrUnsupported})), h.lastFrame());

  h.sent.clear();
  h.feed(frameOf(kMsgMeshCounters, 4, {}));
  h.run();
  uint8_t type = 0, seq = 0;
  const std::vector<uint8_t> body = lastBody(h, type, seq);
  Reader rd(body.data(), body.size());
  EXPECT_EQ(kOk, rd.u8());
  rd.u32();
  rd.u32();
  rd.u32();
  EXPECT_EQ(0u, rd.u32()) << "adverts";
  EXPECT_EQ(0u, rd.u32()) << "adverts_dropped";
  EXPECT_EQ(0u, rd.u32()) << "obs_dropped";
}

TEST(Observations, AMalformedPullIsRefusedAndNothingLeavesTheRing) {
  ObsRig r;
  r.h.now = 1000;
  r.hear(1, 2000, 1000);
  // An empty payload: `max` is a u8 and there is none (section 3, kErrMalformed).
  r.h.feed(frameOf(kMsgObsPull, 5, {}));
  r.h.run();
  EXPECT_EQ(toHex(frameOf(responseType(kMsgObsPull), 5, {kErrMalformed})), r.h.lastFrame());
  EXPECT_EQ(1u, r.obs.pending()) << "a refused request takes nothing";
  // And trailing bytes a newer Walter appended are ignored (section 5.1, "Versions").
  r.h.sent.clear();
  r.h.feed(frameOf(kMsgObsPull, 6, {1, 0xAA, 0xBB}));
  r.h.run();
  uint8_t type = 0, seq = 0;
  const std::vector<uint8_t> body = lastBody(r.h, type, seq);
  ASSERT_GE(body.size(), 4u);
  EXPECT_EQ(kOk, body[0]);
  EXPECT_EQ(0u, r.obs.pending());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
