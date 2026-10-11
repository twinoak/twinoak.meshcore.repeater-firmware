// src/link/session/: the whole box-link session, driven the way src/link/SerialTransport.cpp
// drives it, against fakes - and held against spec/fixtures/link/link-v1.json for every request
// this firmware answers today (HELLO, PING, STATUS, IDENTITY, REBOOT) and for EV_BOOT.
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
//     reads zero.

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

class FakeCli : public CliAccess {
 public:
  size_t runCommand(const char* line, char* reply, size_t reply_cap) override {
    (void)line;
    if (reply_cap > 0) reply[0] = 0;
    return 0;  // task 05 owns the console over the link; this fake is here for the caps bit
  }
};

class FakeAttention : public AttentionAccess {
 public:
  uint8_t reason = kAttentionNone;
  bool asleep = false;

  uint8_t pendingReason() const override { return reason; }
  bool walterAsleep() const override { return asleep; }
};

class FakePower : public PowerAccess {
 public:
  bool readPower(PowerResponse& out) override {
    (void)out;
    return false;  // task 06 owns POWER_GET; this fake is here for the caps bit
  }
};

class FakeEnv : public EnvAccess {
 public:
  bool readEnv(EnvResponse& out) override {
    (void)out;
    return false;
  }
};

class FakeMeshClient : public MeshClientAccess {
 public:
  bool job = false;
  bool awaiting = false;

  bool jobBegun() const override { return job; }
  bool replyAwaited() const override { return awaiting; }
};

class FakeObservations : public ObservationAccess {
 public:
  uint8_t waiting = 0;
  uint16_t lost = 0;

  uint8_t pending() const override { return waiting; }
  uint16_t droppedSinceBoot() const override { return lost; }
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
    services.lpp = nullptr;  // no capability bit of its own (LINK-PROTOCOL.md section 4)
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
    default:
      ADD_FAILURE() << response.name << ": nothing here arranges this type";
      break;
  }
}

// The request types this firmware answers: task 03's five, and task 04's two.
bool handledHere(uint8_t type) {
  return type == kMsgHello || type == kMsgPing || type == kMsgStatus || type == kMsgIdentity ||
         type == kMsgReboot || type == kMsgTimeSet || type == kMsgTimeGet;
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
  EXPECT_EQ(7, n) << "HELLO, PING, STATUS, IDENTITY, REBOOT, TIME_SET and TIME_GET";
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

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
