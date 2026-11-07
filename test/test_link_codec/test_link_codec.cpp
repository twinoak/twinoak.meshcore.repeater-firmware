// src/common/link/codec/: the box link as bytes, held against *every* file of
// spec/fixtures/link/ - link-v1.json, which is 1.0's and whose bytes never change because
// firmware that speaks 1.0 has been released; link-v1.1.json, which is every case of 1.0 and the
// four 1.1 adds; and link-v1.2.json, which is every case of 1.1 and the fourteen 1.2 adds
// (spec/LINK-PROTOCOL.md section 11). spec/tools/linkref.py generates them from that document,
// and the Walter's firmware tests its own codec against the same files in its own repository.
//
// They are read side by side and not one in place of another: a 1.0 Walter - the one on the
// bench - must still be answered the bytes it has always been answered, and that is only
// checkable while 1.0's file is still run.
//
// The fixtures are read at run time from FIXTURES_DIR (platformio.ini's [env:native]), so a case
// added to the specification is a case this suite runs without a line changing here - and
// `CasesOfTheFixture.EveryCaseIsExercised` fails if one of them is of a shape no function here
// knows, which is what keeps "every case passes" an honest claim.
//
// What is checked, in the order tasks/phase-1/02-the-link-codec.md asks for it:
//   - every crc16 and cobs vector, both directions;
//   - every `request` case: decoding its payload gives its fields;
//   - every `response` and `event` case: encoding its fields gives its payload, and the frame
//     built from (wire_type, seq, payload) is the fixture's frame byte for byte;
//   - every `invalid` frame: refused by the receiver and counted;
//   - the `stream`: fed whole and fed one byte at a time, the same packets and the same count of
//     bad frames;
//   - a request payload one byte short of its layout is malformed, never a read out of bounds
//     (the suite also runs under AddressSanitizer in the container; see the task's report).

#include <gtest/gtest.h>

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "common/link/codec/Cobs.h"
#include "common/link/codec/Crc16.h"
#include "common/link/codec/FrameReceiver.h"
#include "common/link/codec/FrameWriter.h"
#include "common/link/codec/Messages.h"
#include "common/link/codec/Numbers.h"
#include "common/link/codec/Packet.h"

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
std::string toHex(Bytes b) { return toHex(b.ptr, b.len); }

Bytes viewOf(const std::vector<uint8_t>& v) { return bytesOf(v.data(), v.size()); }
Bytes viewOf(const std::string& s) { return bytesOf(s.data(), s.size()); }

std::string asText(Bytes b) {
  return std::string(reinterpret_cast<const char*>(b.ptr), b.len);
}

// ----------------------------------------------------------------------------- the fixture file

// The two files of spec/fixtures/link/, and what each says its `link` is. `least_cases` is the
// count this task was written against: fewer is a copy that lost something.
struct FixtureFile {
  const char* file;
  const char* link;
  size_t least_cases;
};
const FixtureFile kFixtures[] = {
    {"link-v1.json", "1.0", 69},
    {"link-v1.1.json", "1.1", 73},
    {"link-v1.2.json", "1.2", 87},
};
constexpr size_t kFixtureCount = sizeof(kFixtures) / sizeof(kFixtures[0]);

JsonDocument& fixture(size_t which) {
  static JsonDocument docs[kFixtureCount];
  static bool loaded[kFixtureCount] = {false};
  JsonDocument& doc = docs[which];
  if (!loaded[which]) {
    loaded[which] = true;
    const std::string path = std::string(FIXTURES_DIR) + "/" + kFixtures[which].file;
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

// One case of a fixture's `cases`, read into something a test can hold. `file` is which of the
// two it came from, so a failure names the file as well as the case.
struct Case {
  std::string file;
  std::string name;
  std::string kind;
  std::string type_name;
  uint8_t type = 0;
  uint8_t wire_type = 0;
  uint8_t seq = 0;
  JsonObject fields;
  std::vector<uint8_t> payload;
  std::vector<uint8_t> packet;
  std::vector<uint8_t> frame;

  std::string where() const { return file + ": " + name; }
};

const std::vector<Case>& casesOf(size_t which) {
  static std::vector<Case> all[kFixtureCount];
  std::vector<Case>& mine = all[which];
  if (mine.empty()) {
    JsonArray from = fixture(which)["cases"].as<JsonArray>();
    for (JsonObject c : from) {
      Case k;
      k.file = kFixtures[which].file;
      k.name = c["name"].as<std::string>();
      k.kind = c["kind"].as<std::string>();
      k.type_name = c["type_name"].as<std::string>();
      k.type = c["type"].as<uint8_t>();
      k.wire_type = c["wire_type"].as<uint8_t>();
      k.seq = c["seq"].as<uint8_t>();
      k.fields = c["fields"].as<JsonObject>();
      k.payload = fromHex(c["payload"].as<std::string>());
      k.packet = fromHex(c["packet"].as<std::string>());
      k.frame = fromHex(c["frame"].as<std::string>());
      mine.push_back(k);
    }
  }
  return mine;
}

// Every case of every file, 1.0's first. The cases a later minor repeats from an earlier one are
// run again, which is the point: the same bytes have to come out whichever file asked for them.
const std::vector<Case>& cases() {
  static std::vector<Case> all;
  if (all.empty()) {
    for (size_t i = 0; i < kFixtureCount; i++) {
      for (const Case& c : casesOf(i)) all.push_back(c);
    }
  }
  return all;
}

// ----------------------------------------------------------------------------- field matchers

void expectNum(const Case& c, const char* key, long long got) {
  ASSERT_TRUE(c.fields[key].is<long long>()) << c.where() << ": the fixture has no " << key;
  EXPECT_EQ(c.fields[key].as<long long>(), got) << c.where() << "." << key;
}

void expectHex(const Case& c, const char* key, Bytes got) {
  ASSERT_TRUE(c.fields[key].is<const char*>()) << c.where() << ": the fixture has no " << key;
  EXPECT_EQ(c.fields[key].as<std::string>(), toHex(got)) << c.where() << "." << key;
}

void expectText(const Case& c, const char* key, Bytes got) {
  ASSERT_TRUE(c.fields[key].is<const char*>()) << c.where() << ": the fixture has no " << key;
  EXPECT_EQ(c.fields[key].as<std::string>(), asText(got)) << c.where() << "." << key;
}

void expectPath(const Case& c, const Path& p) {
  expectNum(c, "path_mode", p.mode);
  if (p.mode == kPathHops) {
    expectNum(c, "hash_size", p.hash_size);
    expectNum(c, "hop_count", p.hop_count);
    expectHex(c, "hashes", p.hashes);
    EXPECT_EQ(size_t(p.hash_size) * size_t(p.hop_count), size_t(p.hashes.len)) << c.where();
  } else {
    EXPECT_EQ(0, p.hash_size) << c.where();
    EXPECT_EQ(0, p.hop_count) << c.where();
    EXPECT_EQ(0u, unsigned(p.hashes.len)) << c.where();
    EXPECT_FALSE(c.fields["hashes"].is<const char*>())
        << c.where() << ": path_mode " << unsigned(p.mode) << " carries no hashes";
  }
}

// ----------------------------------------------------------------------------- fields -> structs

// The strings and byte runs an encoded message points at have to outlive the call that encodes
// it, so every "from the fixture's fields" helper takes a store the caller keeps on its stack.
struct Store {
  std::vector<std::string> texts;
  std::vector<std::vector<uint8_t>> blobs;

  Store() {
    texts.reserve(8);
    blobs.reserve(8);
  }
  // std::string and std::vector keep their buffers where they are; the reserves above keep the
  // vectors of them from moving as more are added.
  Bytes text(JsonObject f, const char* key) {
    texts.push_back(f[key].as<std::string>());
    return viewOf(texts.back());
  }
  Bytes blob(JsonObject f, const char* key) {
    blobs.push_back(fromHex(f[key].as<std::string>()));
    return viewOf(blobs.back());
  }
  const uint8_t* key32(JsonObject f, const char* key) {
    blobs.push_back(fromHex(f[key].as<std::string>()));
    return blobs.back().size() == kPubKeyLen ? blobs.back().data() : nullptr;
  }
};

Path pathFromFields(JsonObject f, Store& store) {
  Path p;
  p.mode = f["path_mode"].as<uint8_t>();
  if (p.mode == kPathHops) {
    p.hash_size = f["hash_size"].as<uint8_t>();
    p.hop_count = f["hop_count"].as<uint8_t>();
    p.hashes = store.blob(f, "hashes");
  }
  return p;
}

HelloBody helloBodyFromFields(JsonObject f, Store& store) {
  HelloBody b;
  b.proto_major = f["proto_major"].as<uint8_t>();
  b.proto_minor = f["proto_minor"].as<uint8_t>();
  b.role = f["role"].as<uint8_t>();
  b.slot = f["slot"].as<uint8_t>();
  b.max_payload = f["max_payload"].as<uint8_t>();
  b.caps = f["caps"].as<uint32_t>();
  b.boot_id = f["boot_id"].as<uint32_t>();
  b.fw_version = store.text(f, "fw_version");
  b.board = store.text(f, "board");
  b.upstream = store.text(f, "upstream");
  // 1.1's `band`, at the end and only in a body that says 1.1 or later (section 5.1). A 1.0
  // case has no `band` field at all, and leaving it empty here is what the encoder is then held
  // to: it must write nothing for it.
  if (f["band"].is<const char*>()) b.band = store.text(f, "band");
  return b;
}

ReplyRecord replyFromFields(JsonObject f, Store& store) {
  ReplyRecord r;
  r.state = f["state"].as<uint8_t>();
  r.tag = f["tag"].as<uint32_t>();
  r.via_path = f["via_path"].as<uint8_t>();
  r.snr_q4 = int8_t(f["snr_q4"].as<int>());
  r.rssi = int16_t(f["rssi"].as<int>());
  r.elapsed_ms = f["elapsed_ms"].as<uint32_t>();
  if (r.state == kReplyResponse) {
    r.data = store.blob(f, "data");
  } else if (r.state == kReplyText) {
    r.data = store.text(f, "text");
  } else if (r.state == kReplyTrace) {
    r.trace_flags = f["trace_flags"].as<uint8_t>();
    r.final_snr_q4 = int8_t(f["final_snr_q4"].as<int>());
    r.hashes = store.blob(f, "hashes");
    r.snrs = store.blob(f, "snrs");
  }
  return r;
}

// ----------------------------------------------------------------------------- the dispatchers

// Decodes one `request` case's payload and holds every field against the fixture's. Returns false
// when no decoder here knows this type - which is what the coverage test is looking for.
bool exerciseRequest(const Case& c) {
  Bytes in = viewOf(c.payload);
  switch (c.type) {
    case kMsgHello: {
      Hello m;
      EXPECT_TRUE(decodeHello(in, m)) << c.where();
      expectNum(c, "proto_major", m.proto_major);
      expectNum(c, "proto_minor", m.proto_minor);
      expectNum(c, "peer_kind", m.peer_kind);
      expectNum(c, "max_payload", m.max_payload);
      expectText(c, "fw_version", m.fw_version);
      // 1.1 changed the *answer*, not the request: the request's layout is 1.0's whatever minor
      // it asks for (section 5.1).
      EXPECT_EQ(size_t(5) + size_t(m.fw_version.len), c.payload.size()) << c.where();
      return true;
    }
    case kMsgPing: {
      Ping m;
      EXPECT_TRUE(decodePing(in, m)) << c.where();
      expectNum(c, "nonce", m.nonce);
      return true;
    }
    case kMsgTimeSet: {
      TimeSet m;
      EXPECT_TRUE(decodeTimeSet(in, m)) << c.where();
      expectNum(c, "epoch", m.epoch);
      expectNum(c, "source", m.source);
      expectNum(c, "flags", m.flags);
      return true;
    }
    case kMsgReboot: {
      Reboot m;
      EXPECT_TRUE(decodeReboot(in, m)) << c.where();
      expectNum(c, "mode", m.mode);
      expectNum(c, "delay_ms", m.delay_ms);
      return true;
    }
    case kMsgSleepNotice: {
      SleepNotice m;
      EXPECT_TRUE(decodeSleepNotice(in, m)) << c.where();
      expectNum(c, "expect_back_s", m.expect_back_s);
      return true;
    }
    case kMsgCli: {
      Cli m;
      EXPECT_TRUE(decodeCli(in, m)) << c.where();
      expectText(c, "text", m.text);
      return true;
    }
    case kMsgLppGet: {
      LppGet m;
      EXPECT_TRUE(decodeLppGet(in, m)) << c.where();
      expectNum(c, "perm_mask", m.perm_mask);
      return true;
    }
    case kMsgLocalRequest: {
      LocalRequest m;
      EXPECT_TRUE(decodeLocalRequest(in, m)) << c.where();
      expectNum(c, "req_type", m.req_type);
      expectHex(c, "data", m.data);
      return true;
    }
    case kMsgJobBegin: {
      JobBegin m;
      EXPECT_TRUE(decodeJobBegin(in, m)) << c.where();
      EXPECT_NE(nullptr, m.pubkey) << c.where();
      if (m.pubkey != nullptr) {
        EXPECT_EQ(c.fields["pubkey"].as<std::string>(), toHex(m.pubkey, kPubKeyLen)) << c.where();
      }
      expectPath(c, m.path);
      return true;
    }
    case kMsgPathSet: {
      Path p;
      EXPECT_TRUE(decodePathSet(in, p)) << c.where();
      expectPath(c, p);
      return true;
    }
    case kMsgSendLogin: {
      SendLogin m;
      EXPECT_TRUE(decodeSendLogin(in, m)) << c.where();
      expectNum(c, "timeout_ms", m.timeout_ms);
      expectHex(c, "password", m.password);
      return true;
    }
    case kMsgSendRequest: {
      SendRequest m;
      EXPECT_TRUE(decodeSendRequest(in, m)) << c.where();
      expectNum(c, "timeout_ms", m.timeout_ms);
      expectNum(c, "req_type", m.req_type);
      expectHex(c, "data", m.data);
      return true;
    }
    case kMsgSendCli: {
      SendCli m;
      EXPECT_TRUE(decodeSendCli(in, m)) << c.where();
      expectNum(c, "timeout_ms", m.timeout_ms);
      expectNum(c, "flags", m.flags);
      expectText(c, "text", m.text);
      return true;
    }
    case kMsgSendDiscovery: {
      SendDiscovery m;
      EXPECT_TRUE(decodeSendDiscovery(in, m)) << c.where();
      expectNum(c, "timeout_ms", m.timeout_ms);
      return true;
    }
    case kMsgSendTrace: {
      SendTrace m;
      EXPECT_TRUE(decodeSendTrace(in, m)) << c.where();
      expectNum(c, "timeout_ms", m.timeout_ms);
      expectNum(c, "hash_size", m.hash_size);
      expectHex(c, "hashes", m.hashes);
      return true;
    }
    case kMsgObsPull: {
      ObsPull m;
      EXPECT_TRUE(decodeObsPull(in, m)) << c.where();
      expectNum(c, "max", m.max);
      return true;
    }
    // The requests with no fields at all (section 5.1, "Versions": trailing bytes are ignored).
    case kMsgStatus:
    case kMsgTimeGet:
    case kMsgIdentity:
    case kMsgPowerGet:
    case kMsgEnvGet:
    case kMsgJobEnd:
    case kMsgPathGet:
    case kMsgReplyGet:
    case kMsgMeshCounters:
      EXPECT_TRUE(c.payload.empty()) << c.where() << ": this request carries no fields";
      EXPECT_TRUE(decodeNoFields(in)) << c.where();
      EXPECT_EQ(0u, c.fields.size()) << c.where() << ": the fixture gives it fields";
      return true;
    default:
      return false;
  }
}

// Encodes one `response` or `event` case from its fields into out. Returns false when no encoder
// here knows this shape.
bool exerciseOutgoing(const Case& c, std::vector<uint8_t>& out) {
  uint8_t buf[kMaxPayload];
  size_t n = 0;
  JsonObject f = c.fields;
  Store store;

  if (c.kind == "response" && f["status"].as<uint8_t>() != kOk) {
    // "When status is not OK the payload is that one byte and nothing else" (section 3) - the
    // same encoder for every type, the unknown ones included (0x5E in the fixture).
    n = encodeStatus(f["status"].as<uint8_t>(), buf, sizeof(buf));
    out.assign(buf, buf + n);
    return n > 0;
  }

  if (c.kind == "event") {
    switch (c.type) {
      case kMsgEvBoot:
        n = encodeEvBoot(f["reset_reason"].as<uint8_t>(), helloBodyFromFields(f, store), buf,
                         sizeof(buf));
        break;
      case kMsgEvMeshReply:
        n = encodeEvMeshReply(replyFromFields(f, store), buf, sizeof(buf));
        break;
      case kMsgEvAttention: {
        Attention a;
        a.reason = f["reason"].as<uint8_t>();
        a.detail = f["detail"].as<uint16_t>();
        a.repeat = f["repeat"].as<uint8_t>();
        n = encodeEvAttention(a, buf, sizeof(buf));
        break;
      }
      default:
        return false;
    }
    out.assign(buf, buf + n);
    return n > 0;
  }

  switch (c.type) {
    case kMsgHello:
      n = encodeHelloResponse(helloBodyFromFields(f, store), buf, sizeof(buf));
      break;
    case kMsgPing: {
      PingResponse m;
      m.nonce = f["nonce"].as<uint32_t>();
      m.uptime_s = f["uptime_s"].as<uint32_t>();
      n = encodePingResponse(m, buf, sizeof(buf));
      break;
    }
    case kMsgStatus: {
      StatusResponse m;
      m.uptime_s = f["uptime_s"].as<uint32_t>();
      m.reset_reason = f["reset_reason"].as<uint8_t>();
      m.time_state = f["time_state"].as<uint8_t>();
      m.epoch = f["epoch"].as<uint32_t>();
      m.flags = f["flags"].as<uint16_t>();
      m.obs_pending = f["obs_pending"].as<uint8_t>();
      m.obs_dropped = f["obs_dropped"].as<uint16_t>();
      m.link_errors = f["link_errors"].as<uint16_t>();
      m.attention = f["attention"].as<uint8_t>();
      n = encodeStatusResponse(m, buf, sizeof(buf));
      break;
    }
    case kMsgTimeSet: {
      TimeSetResponse m;
      m.prev_epoch = f["prev_epoch"].as<uint32_t>();
      m.epoch = f["epoch"].as<uint32_t>();
      m.time_state = f["time_state"].as<uint8_t>();
      n = encodeTimeSetResponse(m, buf, sizeof(buf));
      break;
    }
    case kMsgTimeGet: {
      TimeGetResponse m;
      m.epoch = f["epoch"].as<uint32_t>();
      m.time_state = f["time_state"].as<uint8_t>();
      n = encodeTimeGetResponse(m, buf, sizeof(buf));
      break;
    }
    case kMsgIdentity: {
      IdentityResponse m;
      m.pubkey = store.key32(f, "pubkey");
      m.name = store.text(f, "name");
      n = encodeIdentityResponse(m, buf, sizeof(buf));
      break;
    }
    case kMsgCli: {
      CliResponse m;
      m.text = store.text(f, "text");
      n = encodeCliResponse(m, buf, sizeof(buf));
      break;
    }
    case kMsgPowerGet: {
      PowerResponse m;
      m.valid = f["valid"].as<uint8_t>();
      uint8_t i = 0;
      JsonArray channels = f["channels"].as<JsonArray>();
      for (JsonObject ch : channels) {
        if (i >= kPowerChannels) break;
        m.channels[i].bus_mv = ch["bus_mv"].as<uint16_t>();
        m.channels[i].current_ma = int16_t(ch["current_ma"].as<int>());
        i++;
      }
      EXPECT_EQ(kPowerChannels, i) << c.where() << ": three channels, always";
      n = encodePowerResponse(m, buf, sizeof(buf));
      break;
    }
    case kMsgEnvGet: {
      EnvResponse m;
      m.valid = f["valid"].as<uint8_t>();
      m.temp_centi = int16_t(f["temp_centi"].as<int>());
      m.rh_centi = f["rh_centi"].as<uint16_t>();
      m.pressure_pa = f["pressure_pa"].as<uint32_t>();
      n = encodeEnvResponse(m, buf, sizeof(buf));
      break;
    }
    case kMsgLppGet: {
      LppResponse m;
      m.lpp = store.blob(f, "lpp");
      n = encodeLppResponse(m, buf, sizeof(buf));
      break;
    }
    case kMsgLocalRequest: {
      LocalResponse m;
      m.data = store.blob(f, "data");
      n = encodeLocalResponse(m, buf, sizeof(buf));
      break;
    }
    case kMsgPathGet:
      n = encodePathGetResponse(pathFromFields(f, store), buf, sizeof(buf));
      break;
    case kMsgSendLogin:
    case kMsgSendRequest:
    case kMsgSendCli:
    case kMsgSendDiscovery:
    case kMsgSendTrace: {
      SendResponse m;
      m.send = f["send"].as<uint8_t>();
      m.tag = f["tag"].as<uint32_t>();
      m.sender_ts = f["sender_ts"].as<uint32_t>();
      n = encodeSendResponse(m, buf, sizeof(buf));
      break;
    }
    case kMsgReplyGet:
      n = encodeReplyGetResponse(replyFromFields(f, store), buf, sizeof(buf));
      break;
    case kMsgObsPull: {
      // The records' own views have to outlive the call too, so their stores are here.
      std::vector<Observation> records;
      std::vector<std::vector<uint8_t>> keys, paths, apps;
      JsonArray from = f["records"].as<JsonArray>();
      records.reserve(from.size());
      keys.reserve(from.size());
      paths.reserve(from.size());
      apps.reserve(from.size());
      for (JsonObject o : from) {
        keys.push_back(fromHex(o["pubkey"].as<std::string>()));
        paths.push_back(fromHex(o["path"].as<std::string>()));
        apps.push_back(fromHex(o["app_data"].as<std::string>()));
        Observation rec;
        rec.pubkey = keys.back().size() == kPubKeyLen ? keys.back().data() : nullptr;
        rec.adv_ts = o["adv_ts"].as<uint32_t>();
        rec.age_s = o["age_s"].as<uint32_t>();
        rec.snr_q4 = int8_t(o["snr_q4"].as<int>());
        rec.rssi = int16_t(o["rssi"].as<int>());
        rec.path_len = o["path_len"].as<uint8_t>();
        rec.path = viewOf(paths.back());
        rec.app_data = viewOf(apps.back());
        records.push_back(rec);
      }
      ObsPullResponse m;
      m.remaining = f["remaining"].as<uint8_t>();
      m.dropped = f["dropped"].as<uint16_t>();
      m.records = records.empty() ? nullptr : records.data();
      m.count = uint8_t(records.size());
      n = encodeObsPullResponse(m, buf, sizeof(buf));
      break;
    }
    case kMsgMeshCounters: {
      MeshCountersResponse m;
      m.rx = f["rx"].as<uint32_t>();
      m.tx = f["tx"].as<uint32_t>();
      m.client_tx = f["client_tx"].as<uint32_t>();
      m.adverts = f["adverts"].as<uint32_t>();
      m.adverts_dropped = f["adverts_dropped"].as<uint32_t>();
      m.obs_dropped = f["obs_dropped"].as<uint32_t>();
      m.noise_floor = int16_t(f["noise_floor"].as<int>());
      m.last_rssi = int16_t(f["last_rssi"].as<int>());
      m.last_snr_q4 = int8_t(f["last_snr_q4"].as<int>());
      n = encodeMeshCountersResponse(m, buf, sizeof(buf));
      break;
    }
    // The responses whose whole payload is the status byte (section 3).
    case kMsgReboot:
    case kMsgSleepNotice:
    case kMsgJobBegin:
    case kMsgJobEnd:
    case kMsgPathSet:
      n = encodeStatus(kOk, buf, sizeof(buf));
      break;
    default:
      return false;
  }
  out.assign(buf, buf + n);
  return n > 0;
}

// ----------------------------------------------------------------------------- the receiver

// Feeds a whole buffer through one receiver and collects what it delivered, as "type/seq/payload".
std::vector<std::string> receiveAll(FrameReceiver& rx, const std::vector<uint8_t>& data,
                                    bool one_byte_at_a_time) {
  std::vector<std::string> got;
  const uint8_t* p = data.data();
  size_t left = data.size();
  while (left > 0) {
    size_t used = 0;
    if (one_byte_at_a_time) {
      rx.feedByte(*p);
      used = 1;
    } else {
      used = rx.feed(p, left);
    }
    if (rx.hasPacket()) {
      const ParsedPacket& pkt = rx.packet();
      got.push_back(std::to_string(unsigned(pkt.type)) + "/" + std::to_string(unsigned(pkt.seq)) +
                    "/" + toHex(pkt.body));
    }
    p += used;
    left -= used;
  }
  return got;
}

}  // namespace

// ----------------------------------------------------------------------------- crc and cobs

// `cases` is the one part the files differ in (section 11 lists the five parts). The tests below
// therefore read `crc16`, `cobs`, `invalid` and `stream` from 1.0's file alone - and this is what
// makes that sound: if the generator ever makes one of them depend on the minor, this fails and
// says which part, and the test that reads it has to run every file instead.
TEST(TheFixtureFiles, DifferOnlyInTheirCases) {
  for (const char* part : {"crc16", "cobs", "invalid", "stream"}) {
    std::string from_10;
    serializeJson(fixture(0)[part], from_10);
    for (size_t i = 1; i < kFixtureCount; i++) {
      std::string from_newer;
      serializeJson(fixture(i)[part], from_newer);
      EXPECT_EQ(from_10, from_newer)
          << part << ": " << kFixtures[i].file << " no longer agrees with " << kFixtures[0].file;
    }
  }
  // Each minor adds cases and removes none.
  for (size_t i = 1; i < kFixtureCount; i++) {
    EXPECT_LT(casesOf(i - 1).size(), casesOf(i).size())
        << kFixtures[i].file << " is " << kFixtures[i - 1].file << "'s cases and more";
  }
}

TEST(Crc16Vectors, EveryVectorOfTheFixture) {
  int n = 0;
  JsonArray vectors = fixture(0)["crc16"].as<JsonArray>();
  for (JsonObject v : vectors) {
    const std::vector<uint8_t> data = fromHex(v["data"].as<std::string>());
    const uint16_t want = uint16_t(std::stoul(v["crc"].as<std::string>(), nullptr, 16));
    EXPECT_EQ(want, crc16(data.data(), data.size())) << v["data"].as<std::string>();
    // The other direction: a packet that carries this checksum passes, and one bit less does not.
    std::vector<uint8_t> body = data;
    body.push_back(uint8_t(want & 0xFF));
    body.push_back(uint8_t(want >> 8));
    if (body.size() >= kMinPacket && body.size() <= kMaxPacket) {
      ParsedPacket pkt;
      EXPECT_EQ(PacketError::kNone, parsePacket(body.data(), body.size(), pkt));
      body[body.size() - 1] ^= 0x01;
      EXPECT_EQ(PacketError::kCrc, parsePacket(body.data(), body.size(), pkt));
    }
    n++;
  }
  EXPECT_GT(n, 0) << "the fixture has no crc16 vectors";
  // The check value the specification names (section 2).
  const char* nine = "123456789";
  EXPECT_EQ(0x29B1, crc16(reinterpret_cast<const uint8_t*>(nine), 9));
}

TEST(CobsVectors, EveryVectorOfTheFixtureBothDirections) {
  int n = 0;
  JsonArray vectors = fixture(0)["cobs"].as<JsonArray>();
  for (JsonObject v : vectors) {
    const std::vector<uint8_t> raw = fromHex(v["raw"].as<std::string>());
    const std::vector<uint8_t> want = fromHex(v["encoded"].as<std::string>());

    uint8_t out[kReceiveBuffer];
    const size_t len = cobsEncode(raw.data(), raw.size(), out, sizeof(out));
    ASSERT_GT(len, 0u) << v["raw"].as<std::string>();
    EXPECT_EQ(toHex(want), toHex(out, len));
    EXPECT_EQ(raw.size() + 1, len) << "the overhead of a frame this short is exactly one byte";
    for (size_t i = 0; i < len; i++) {
      EXPECT_NE(0, out[i]) << "an encoding holds no zero: that is what the terminator is for";
    }

    uint8_t back[kReceiveBuffer];
    size_t back_len = 0;
    EXPECT_EQ(CobsError::kNone,
              cobsDecode(want.data(), want.size(), back, sizeof(back), back_len));
    EXPECT_EQ(toHex(raw), toHex(back, back_len));
    n++;
  }
  EXPECT_GT(n, 0) << "the fixture has no cobs vectors";
}

TEST(CobsVectors, TheEncoderRefusesWhatDoesNotFitAndARunOf254) {
  uint8_t out[8];
  const uint8_t raw[4] = {1, 2, 3, 4};
  EXPECT_EQ(0u, cobsEncode(raw, sizeof(raw), out, 4));  // needs five
  EXPECT_EQ(5u, cobsEncode(raw, sizeof(raw), out, 5));

  // A run of 254 non-zero bytes would need the 0xFF code of general COBS, which this link never
  // sends (LINK-PROTOCOL.md section 2): the encoder refuses rather than emit it.
  std::vector<uint8_t> ones(254, 0x01);
  std::vector<uint8_t> room(300, 0);
  EXPECT_EQ(0u, cobsEncode(ones.data(), ones.size(), room.data(), room.size()));
  ones.pop_back();
  EXPECT_EQ(254u, cobsEncode(ones.data(), ones.size(), room.data(), room.size()));
}

TEST(CobsVectors, TheDecoderRefusesAZeroTheExtendedCodeAndABlockPastTheEnd) {
  uint8_t out[kReceiveBuffer];
  size_t len = 0;
  const uint8_t zero_inside[] = {0x02, 0x11, 0x00, 0x11};
  EXPECT_EQ(CobsError::kZeroInside,
            cobsDecode(zero_inside, sizeof(zero_inside), out, sizeof(out), len));
  const uint8_t extended[] = {0xFF, 0x11};
  EXPECT_EQ(CobsError::kExtendedCode,
            cobsDecode(extended, sizeof(extended), out, sizeof(out), len));
  const uint8_t past_end[] = {0x09, 0x02, 0x03};
  EXPECT_EQ(CobsError::kPastEnd, cobsDecode(past_end, sizeof(past_end), out, sizeof(out), len));
  EXPECT_EQ(CobsError::kEmpty, cobsDecode(past_end, 0, out, sizeof(out), len));
  const uint8_t three[] = {0x04, 0x11, 0x22, 0x33};
  EXPECT_EQ(CobsError::kNoRoom, cobsDecode(three, sizeof(three), out, 2, len));
}

// ----------------------------------------------------------------------------- the cases

TEST(CasesOfTheFixture, EveryRequestDecodesToItsFields) {
  int n = 0;
  for (const Case& c : cases()) {
    if (c.kind != "request") continue;
    EXPECT_TRUE(exerciseRequest(c)) << c.where() << ": no decoder for type 0x" << std::hex
                                    << unsigned(c.type);
    n++;
  }
  EXPECT_GT(n, 0) << "the fixture has no request cases";
}

TEST(CasesOfTheFixture, EveryResponseAndEventEncodesToItsPayload) {
  int n = 0;
  for (const Case& c : cases()) {
    if (c.kind == "request") continue;
    std::vector<uint8_t> got;
    ASSERT_TRUE(exerciseOutgoing(c, got)) << c.where() << ": no encoder for type 0x" << std::hex
                                          << unsigned(c.type);
    EXPECT_EQ(toHex(c.payload), toHex(got)) << c.where();
    n++;
  }
  EXPECT_GT(n, 0) << "the fixture has no response or event cases";
}

TEST(CasesOfTheFixture, EveryCaseBuildsItsPacketAndItsFrame) {
  for (const Case& c : cases()) {
    EXPECT_LE(c.payload.size(), size_t(kMaxPayload)) << c.where();

    uint8_t packet[kMaxPacket];
    const size_t packet_len =
        buildPacket(c.wire_type, c.seq, c.payload.data(), c.payload.size(), packet, sizeof(packet));
    ASSERT_GT(packet_len, 0u) << c.where();
    EXPECT_EQ(toHex(c.packet), toHex(packet, packet_len)) << c.where();

    FrameWriter w;
    ASSERT_TRUE(w.build(c.wire_type, c.seq, c.payload.data(), c.payload.size(), false)) << c.where();
    EXPECT_EQ(toHex(c.frame), toHex(w.bytes(), w.size())) << c.where();

    // The same frame with the leading 0x00 a sender adds after 100 ms of idle line (section 2).
    FrameWriter idle;
    ASSERT_TRUE(idle.build(c.wire_type, c.seq, c.payload.data(), c.payload.size(), true)) << c.where();
    EXPECT_EQ("00" + toHex(c.frame), toHex(idle.bytes(), idle.size())) << c.where();

    // And the way back: the receiver delivers exactly this packet and drops nothing.
    FrameReceiver rx;
    const std::vector<std::string> got = receiveAll(rx, c.frame, false);
    ASSERT_EQ(1u, got.size()) << c.where();
    EXPECT_EQ(std::to_string(unsigned(c.wire_type)) + "/" + std::to_string(unsigned(c.seq)) + "/" +
                  toHex(c.payload),
              got[0])
        << c.where();
    EXPECT_EQ(0, rx.drops()) << c.where();
  }
}

TEST(CasesOfTheFixture, EveryCaseIsExercised) {
  // The count of cases per kind and type in the fixture, against the count the two dispatchers
  // above actually handled. A case of a shape nothing here knows fails this test rather than
  // passing unnoticed, which is what makes "every case of the fixture passes" checkable.
  std::map<std::string, int> in_fixture;
  std::map<std::string, int> exercised;
  for (const Case& c : cases()) {
    const std::string key = c.kind + " 0x" + toHex(&c.type, 1) + " " + c.type_name;
    in_fixture[key]++;
    bool ok = false;
    if (c.kind == "request") {
      ok = exerciseRequest(c);
    } else {
      std::vector<uint8_t> out;
      ok = exerciseOutgoing(c, out);
    }
    if (ok) exercised[key]++;
  }
  for (const auto& entry : in_fixture) {
    EXPECT_EQ(entry.second, exercised[entry.first])
        << entry.first << ": the fixture has " << entry.second << " case(s) of this shape and "
        << exercised[entry.first] << " were exercised";
  }
  EXPECT_EQ(in_fixture.size(), exercised.size());
  // And each file is the one this task was written against: fewer cases than that is a copy
  // that lost something, and the `link` each one says is what names it.
  for (size_t i = 0; i < kFixtureCount; i++) {
    EXPECT_GE(casesOf(i).size(), kFixtures[i].least_cases)
        << "spec/fixtures/link/" << kFixtures[i].file << " has shrunk";
    EXPECT_EQ(size_t(kMaxPayload), fixture(i)["max_payload"].as<size_t>()) << kFixtures[i].file;
    EXPECT_EQ(kFixtures[i].link, fixture(i)["link"].as<std::string>()) << kFixtures[i].file;
  }
  // Each file is the one before it plus what its minor adds: every case of the older file, by
  // name and by frame, is in the newer one too. That is what makes running every file a check of
  // the older minors and not three copies of one list.
  for (size_t i = 1; i < kFixtureCount; i++) {
    for (const Case& old_case : casesOf(i - 1)) {
      bool found = false;
      for (const Case& newer : casesOf(i)) {
        if (newer.name == old_case.name && newer.frame == old_case.frame) found = true;
      }
      EXPECT_TRUE(found) << old_case.where() << " is not in " << kFixtures[i].file
                         << " with the same frame";
    }
  }
}

// ----------------------------------------------------------------------------- 1.1's band

TEST(TheBand, IsWrittenOnlyWhenTheBodySaysOnePointOneOrLater) {
  // Section 5.1, the `band` row: "present when this body's proto_minor is 1 or more, absent in a
  // 1.0 body". The two fixture cases are the same radio answering at the two minors; here the
  // same HelloBody is encoded twice, with nothing changed but its minor.
  const Case* at_11 = nullptr;
  for (const Case& c : casesOf(1)) {
    if (c.name == "hello response, 1.1: the band at the end") at_11 = &c;
  }
  ASSERT_NE(nullptr, at_11) << "link-v1.1.json has no 1.1 hello response";

  Store store;
  HelloBody b = helloBodyFromFields(at_11->fields, store);
  ASSERT_EQ(1, b.proto_minor);
  ASSERT_EQ("868", asText(b.band));

  uint8_t out[kMaxPayload];
  size_t n = encodeHelloResponse(b, out, sizeof(out));
  ASSERT_GT(n, 0u);
  EXPECT_EQ(toHex(at_11->payload), toHex(out, n)) << "the band at the end of a 1.1 body";

  // EV_BOOT is that same body after a reset reason instead of the status byte, so it carries the
  // band too (section 5.1: "a 1.1 RAK puts its band at the end whether or not the Walter knows
  // 1.1").
  uint8_t ev[kMaxPayload];
  const size_t m = encodeEvBoot(kResetPowerOn, b, ev, sizeof(ev));
  ASSERT_EQ(n, m) << "one byte, then the body";
  EXPECT_EQ(unsigned(kResetPowerOn), unsigned(ev[0]));
  EXPECT_EQ(toHex(out + 1, n - 1), toHex(ev + 1, m - 1)) << "EV_BOOT carries the band too";

  // The same body with nothing changed but its minor: four bytes shorter, and the band nowhere
  // in it.
  b.proto_minor = 0;
  uint8_t older[kMaxPayload];
  const size_t n0 = encodeHelloResponse(b, older, sizeof(older));
  ASSERT_GT(n0, 0u);
  EXPECT_EQ(n - 4, n0) << "a str8 of three characters is four bytes";
  EXPECT_EQ(std::string::npos, toHex(older, n0).find("03383638"))
      << "a 1.0 body carries no band";
  // Byte 2 is the body's own minor; from byte 3 on the two are the same bytes, which is what
  // "a minor adds fields at the end of a payload and never changes what exists" means here.
  EXPECT_EQ(0, older[2]);
  EXPECT_EQ(1, out[2]);
  EXPECT_EQ(toHex(older + 3, n0 - 3), toHex(out + 3, n0 - 3));
}

TEST(TheBand, AnEmptyOneIsAZeroLengthStringAndNotAnOmission) {
  // A role that has no band to report still writes the field in a 1.1 body: a receiver reads a
  // str8 there, and a body that simply stopped would be a 1.0 body claiming to be 1.1.
  HelloBody b;
  b.proto_minor = 1;
  uint8_t out[kMaxPayload];
  const size_t n = encodeHelloResponse(b, out, sizeof(out));
  ASSERT_GT(n, 0u);
  EXPECT_EQ(0x00, out[n - 1]) << "a zero-length str8 is one zero byte";
}

// ----------------------------------------------------------------------------- 1.2's request

TEST(TheLocalRequest, ItsTableIsSectionFivePointNinesAndNothingElse) {
  // "Any other req_type is ERR_RANGE. That includes 0x03 telemetry, whose local form is LPP_GET,
  // and 0x02 keep-alive, which belongs to a login session this link does not have" (section 5.9).
  // The four are MeshCore's own numbers, written out in Numbers.h against upstream's
  // examples/simple_repeater/MyMesh.cpp:46-51 at the pin 81dbae59.
  EXPECT_TRUE(isLocalRequestType(kLocalReqStatus));
  EXPECT_TRUE(isLocalRequestType(kLocalReqAccessList));
  EXPECT_TRUE(isLocalRequestType(kLocalReqNeighbours));
  EXPECT_TRUE(isLocalRequestType(kLocalReqOwnerInfo));
  EXPECT_EQ(0x01, kLocalReqStatus);
  EXPECT_EQ(0x05, kLocalReqAccessList);
  EXPECT_EQ(0x06, kLocalReqNeighbours);
  EXPECT_EQ(0x07, kLocalReqOwnerInfo);
  for (unsigned t = 0; t < 256; t++) {
    const bool in_table = t == 0x01 || t == 0x05 || t == 0x06 || t == 0x07;
    EXPECT_EQ(in_table, isLocalRequestType(uint8_t(t))) << "req_type 0x" << std::hex << t;
  }
  EXPECT_EQ(32, kLocalRequestMaxData);
  EXPECT_EQ(4, kLocalRequestTagBytes);
  EXPECT_EQ(0x80u, uint32_t(kCapLocalRequest));
  EXPECT_EQ(0x0D, kMsgLocalRequest);
}

TEST(TheLocalRequest, AnEmptyPayloadAndMoreThanThirtyTwoBytesOfDataAreMalformed) {
  // The two shapes section 5.9 calls ERR_MALFORMED, and the only two the codec judges.
  LocalRequest m;
  EXPECT_FALSE(decodeLocalRequest(bytesOf(nullptr, 0), m)) << "not even a req_type";

  std::vector<uint8_t> at_the_limit;
  at_the_limit.push_back(kLocalReqNeighbours);
  for (uint8_t i = 0; i < kLocalRequestMaxData; i++) at_the_limit.push_back(i);
  ASSERT_EQ(size_t(kLocalRequestMaxData) + 1, at_the_limit.size());
  ASSERT_TRUE(decodeLocalRequest(viewOf(at_the_limit), m)) << "32 bytes of data is legal";
  EXPECT_EQ(kLocalReqNeighbours, m.req_type);
  EXPECT_EQ(size_t(kLocalRequestMaxData), m.data.len);

  std::vector<uint8_t> one_too_many = at_the_limit;
  one_too_many.push_back(0xFF);
  EXPECT_FALSE(decodeLocalRequest(viewOf(one_too_many), m)) << "33 bytes of data is malformed";

  // A req_type the table does not hold is **decoded**, not refused: it is kErrRange, and that is
  // the session's answer to give (as REBOOT's mode is).
  const std::vector<uint8_t> telemetry = {0x03};
  ASSERT_TRUE(decodeLocalRequest(viewOf(telemetry), m));
  EXPECT_EQ(0x03, m.req_type);
  EXPECT_EQ(0u, m.data.len);
  EXPECT_FALSE(isLocalRequestType(m.req_type));
}

TEST(TheLocalRequest, AnEmptyAnswerIsTheStatusByteAndNothingElse) {
  // "An empty access list" in the fixture: status kOk and no data at all, which is a different
  // thing from a refusal and has to encode to one byte.
  LocalResponse m;
  uint8_t out[kMaxPayload];
  const size_t n = encodeLocalResponse(m, out, sizeof(out));
  ASSERT_EQ(1u, n);
  EXPECT_EQ(unsigned(kOk), unsigned(out[0]));
}

// ----------------------------------------------------------------------------- invalid and stream

TEST(InvalidFrames, EveryOneIsRefusedAndCounted) {
  int n = 0;
  JsonArray frames = fixture(0)["invalid"].as<JsonArray>();
  for (JsonObject v : frames) {
    const std::vector<uint8_t> frame = fromHex(v["frame"].as<std::string>());
    FrameReceiver rx;
    const std::vector<std::string> got = receiveAll(rx, frame, false);
    EXPECT_TRUE(got.empty()) << v["name"].as<std::string>() << ": delivered a packet";
    EXPECT_EQ(1, rx.drops()) << v["name"].as<std::string>() << " (the reference says: "
                             << v["why"].as<std::string>() << ")";
    // One byte at a time, the same answer.
    FrameReceiver slow;
    EXPECT_TRUE(receiveAll(slow, frame, true).empty()) << v["name"].as<std::string>();
    EXPECT_EQ(1, slow.drops()) << v["name"].as<std::string>();
    n++;
  }
  EXPECT_GT(n, 0) << "the fixture has no invalid frames";
}

TEST(TheStream, FedWholeAndFedOneByteAtATime) {
  JsonObject s = fixture(0)["stream"].as<JsonObject>();
  const std::vector<uint8_t> data = fromHex(s["bytes"].as<std::string>());
  std::vector<std::string> want;
  JsonArray expected = s["packets"].as<JsonArray>();
  for (JsonObject p : expected) {
    want.push_back(std::to_string(p["wire_type"].as<unsigned>()) + "/" +
                   std::to_string(p["seq"].as<unsigned>()) + "/" + p["payload"].as<std::string>());
  }
  const int bad = s["bad_frames"].as<int>();

  FrameReceiver whole;
  EXPECT_EQ(want, receiveAll(whole, data, false));
  EXPECT_EQ(bad, whole.drops());

  FrameReceiver slow;
  EXPECT_EQ(want, receiveAll(slow, data, true));
  EXPECT_EQ(bad, slow.drops());

  // And in every chunking of two, three and seven bytes: a UART hands over what it has.
  for (size_t chunk : {size_t(2), size_t(3), size_t(7)}) {
    FrameReceiver rx;
    std::vector<std::string> got;
    size_t at = 0;
    while (at < data.size()) {
      const size_t take = data.size() - at < chunk ? data.size() - at : chunk;
      std::vector<uint8_t> piece(data.begin() + std::ptrdiff_t(at),
                                 data.begin() + std::ptrdiff_t(at + take));
      const std::vector<std::string> from_piece = receiveAll(rx, piece, false);
      got.insert(got.end(), from_piece.begin(), from_piece.end());
      at += take;
    }
    EXPECT_EQ(want, got) << "chunks of " << chunk;
    EXPECT_EQ(bad, rx.drops()) << "chunks of " << chunk;
  }
}

// ----------------------------------------------------------------------------- the receiver

TEST(TheReceiver, EmptyFramesAreNotErrors) {
  FrameReceiver rx;
  const std::vector<uint8_t> zeros(16, 0x00);
  EXPECT_TRUE(receiveAll(rx, zeros, false).empty());
  EXPECT_EQ(0, rx.drops());
  EXPECT_EQ(0u, rx.pending());
}

TEST(TheReceiver, DropsAnOverflowWhenItsTerminatorArrives) {
  FrameReceiver rx;
  // One byte more than the longest frame this protocol has: dropped, counted once, and the frame
  // that follows is still delivered.
  for (size_t i = 0; i < kReceiveBuffer + 64; i++) EXPECT_FALSE(rx.feedByte(0x11));
  EXPECT_EQ(0, rx.drops()) << "nothing is counted before the terminator arrives";
  EXPECT_FALSE(rx.feedByte(0x00));
  EXPECT_EQ(1, rx.drops());
  EXPECT_EQ(1, rx.drops(FrameReceiver::Drop::kOverflow));
  EXPECT_EQ(FrameReceiver::Drop::kOverflow, rx.lastDrop());

  FrameWriter w;
  const uint8_t body[4] = {1, 2, 3, 4};
  ASSERT_TRUE(w.build(kMsgPing, 9, body, sizeof(body), false));
  std::vector<uint8_t> frame(w.bytes(), w.bytes() + w.size());
  EXPECT_EQ(1u, receiveAll(rx, frame, false).size());
  EXPECT_EQ(1, rx.drops());
}

TEST(TheReceiver, CountsEachReasonAndSaturatesAt65535) {
  FrameReceiver rx;
  // A frame whose COBS cannot be decoded, a packet too short, and a packet whose CRC is wrong.
  const std::vector<uint8_t> cobs_bad = {0x09, 0x02, 0x03, 0x00};
  const std::vector<uint8_t> too_short = {0x04, 0x02, 0x03, 0x04, 0x00};
  std::vector<uint8_t> crc_bad;
  {
    FrameWriter w;
    const uint8_t body[2] = {0x11, 0x22};
    ASSERT_TRUE(w.build(kMsgPing, 1, body, sizeof(body), false));
    crc_bad.assign(w.bytes(), w.bytes() + w.size());
    crc_bad[3] ^= 0x01;  // a bit of the body: the COBS still decodes, the CRC no longer matches
  }
  EXPECT_TRUE(receiveAll(rx, cobs_bad, false).empty());
  EXPECT_TRUE(receiveAll(rx, too_short, false).empty());
  EXPECT_TRUE(receiveAll(rx, crc_bad, false).empty());
  EXPECT_EQ(3, rx.drops());
  EXPECT_EQ(1, rx.drops(FrameReceiver::Drop::kCobs));
  EXPECT_EQ(1, rx.drops(FrameReceiver::Drop::kShort));
  EXPECT_EQ(1, rx.drops(FrameReceiver::Drop::kCrc));

  // A packet of 245 bytes - one byte of body too many - is oversize, not an overflow: its frame
  // is 246 bytes and fits the buffer exactly.
  std::vector<uint8_t> over;
  {
    std::vector<uint8_t> packet;
    packet.push_back(kMsgPing);
    packet.push_back(1);
    packet.insert(packet.end(), size_t(kMaxPayload) + 1, 0x11);
    const uint16_t crc = crc16(packet.data(), packet.size());
    packet.push_back(uint8_t(crc & 0xFF));
    packet.push_back(uint8_t(crc >> 8));
    EXPECT_EQ(size_t(kMaxPacket) + 1, packet.size());
    std::vector<uint8_t> encoded(kReceiveBuffer + 8, 0);
    const size_t len = cobsEncode(packet.data(), packet.size(), encoded.data(), encoded.size());
    ASSERT_GT(len, 0u);
    EXPECT_EQ(size_t(kReceiveBuffer), len);
    over.assign(encoded.begin(), encoded.begin() + std::ptrdiff_t(len));
    over.push_back(0x00);
  }
  EXPECT_TRUE(receiveAll(rx, over, false).empty());
  EXPECT_EQ(1, rx.drops(FrameReceiver::Drop::kOversize));
  EXPECT_EQ(4, rx.drops());

  // The counter saturates (STATUS's link_errors, section 5.3).
  for (int i = 0; i < 65600; i++) {
    for (uint8_t b : cobs_bad) rx.feedByte(b);
  }
  EXPECT_EQ(65535, rx.drops());
  EXPECT_EQ(65535, rx.drops(FrameReceiver::Drop::kCobs));
}

TEST(TheReceiver, ForgetsAHalfCollectedFrameOnReset) {
  FrameReceiver rx;
  EXPECT_FALSE(rx.feedByte(0x11));
  EXPECT_EQ(1u, rx.pending());
  rx.reset();
  EXPECT_EQ(0u, rx.pending());
  EXPECT_FALSE(rx.hasPacket());
  EXPECT_EQ(0, rx.drops()) << "a reset is not a drop";
}

TEST(TheWriter, RefusesAPayloadOverTwoHundredAndForty) {
  FrameWriter w;
  std::vector<uint8_t> body(size_t(kMaxPayload) + 1, 0x5A);
  EXPECT_FALSE(w.build(kMsgLppGet | kResponseBit, 0, body.data(), body.size(), false));
  EXPECT_EQ(0u, w.size());
  body.pop_back();
  EXPECT_TRUE(w.build(kMsgLppGet | kResponseBit, 0, body.data(), body.size(), false));
  EXPECT_EQ(size_t(kMaxFrame), w.size()) << "the longest frame there is";
  EXPECT_TRUE(w.build(kMsgLppGet | kResponseBit, 0, body.data(), body.size(), true));
  EXPECT_EQ(size_t(kMaxWrittenFrame), w.size());
}

// ----------------------------------------------------------------------------- malformed input

namespace {

// The bytes a request's layout needs before its last, variable-length field: below this many a
// decoder must say *malformed*, at or above it must decode (spec/LINK-PROTOCOL.md sections 5 and
// 6). kWholePayload is for the layouts whose length is settled by a field inside them - a str8,
// a path - where any truncation at all is malformed.
constexpr size_t kWholePayload = 0xFFFF;

size_t minimumPayload(uint8_t type) {
  switch (type) {
    case kMsgHello: return kWholePayload;      // four bytes and a str8 that runs to the end
    case kMsgJobBegin: return kWholePayload;   // 32 bytes of key and a path
    case kMsgPathSet: return kWholePayload;    // a path
    case kMsgPing: return 4;
    case kMsgTimeSet: return 6;
    case kMsgReboot: return 3;
    case kMsgSleepNotice: return 4;
    case kMsgCli: return 1;                    // one command line, at least one byte
    case kMsgLppGet: return 1;
    case kMsgSendLogin: return 4;              // and a password of 0 to 15 bytes
    case kMsgSendRequest: return 5;
    case kMsgSendCli: return 6;                // and a line of at least one byte
    case kMsgSendDiscovery: return 4;
    case kMsgSendTrace: return 5;
    case kMsgObsPull: return 1;
    default: return 0;                         // the requests with no fields
  }
}

// Decodes one request payload and says only whether it was accepted. The buffer is a copy of
// exactly the length given, so that a read past the end is a read past a heap block and
// AddressSanitizer says so.
bool decodeAccepts(uint8_t type, const uint8_t* data, size_t len) {
  std::vector<uint8_t> exact(data, data + len);
  const Bytes in = bytesOf(exact.data(), exact.size());
  switch (type) {
    case kMsgHello: { Hello m; return decodeHello(in, m); }
    case kMsgPing: { Ping m; return decodePing(in, m); }
    case kMsgTimeSet: { TimeSet m; return decodeTimeSet(in, m); }
    case kMsgReboot: { Reboot m; return decodeReboot(in, m); }
    case kMsgSleepNotice: { SleepNotice m; return decodeSleepNotice(in, m); }
    case kMsgCli: { Cli m; return decodeCli(in, m); }
    case kMsgLppGet: { LppGet m; return decodeLppGet(in, m); }
    case kMsgJobBegin: { JobBegin m; return decodeJobBegin(in, m); }
    case kMsgPathSet: { Path p; return decodePathSet(in, p); }
    case kMsgSendLogin: { SendLogin m; return decodeSendLogin(in, m); }
    case kMsgSendRequest: { SendRequest m; return decodeSendRequest(in, m); }
    case kMsgSendCli: { SendCli m; return decodeSendCli(in, m); }
    case kMsgSendDiscovery: { SendDiscovery m; return decodeSendDiscovery(in, m); }
    case kMsgSendTrace: { SendTrace m; return decodeSendTrace(in, m); }
    case kMsgObsPull: { ObsPull m; return decodeObsPull(in, m); }
    default: return decodeNoFields(in);
  }
}

}  // namespace

TEST(MalformedRequests, EveryTruncationOfEveryFixtureRequestIsRefusedOrLegal) {
  int truncations = 0;
  for (const Case& c : cases()) {
    if (c.kind != "request") continue;
    const size_t min = minimumPayload(c.type);
    for (size_t len = 0; len < c.payload.size(); len++) {
      const bool accepted = decodeAccepts(c.type, c.payload.data(), len);
      if (min == kWholePayload || len < min) {
        EXPECT_FALSE(accepted) << c.where() << " truncated to " << len
                               << " bytes: a payload one byte short of its layout is malformed";
      } else {
        EXPECT_TRUE(accepted) << c.where() << " truncated to " << len
                              << " bytes: the last field of this layout is *rest*";
      }
      truncations++;
    }
    // The payload itself, of course, decodes.
    EXPECT_TRUE(decodeAccepts(c.type, c.payload.data(), c.payload.size())) << c.where();
  }
  EXPECT_GT(truncations, 50) << "the fixture's requests are all empty?";
}

TEST(MalformedRequests, IllegalValuesAreRefused) {
  // A path: an unknown mode, a hash size and a hop count outside their ranges, and hops that are
  // not there (section 6.1).
  const uint8_t unknown_mode[] = {3, 0, 0};
  EXPECT_FALSE(decodeAccepts(kMsgPathSet, unknown_mode, sizeof(unknown_mode)));
  const uint8_t size_zero[] = {kPathHops, 0, 1, 0x11};
  EXPECT_FALSE(decodeAccepts(kMsgPathSet, size_zero, sizeof(size_zero)));
  const uint8_t size_four[] = {kPathHops, 4, 1, 0x11, 0x22, 0x33, 0x44};
  EXPECT_FALSE(decodeAccepts(kMsgPathSet, size_four, sizeof(size_four)));
  const uint8_t no_hops[] = {kPathHops, 2, 0};
  EXPECT_FALSE(decodeAccepts(kMsgPathSet, no_hops, sizeof(no_hops)));
  const uint8_t hops_short[] = {kPathHops, 2, 2, 0x11, 0x22, 0x33};
  EXPECT_FALSE(decodeAccepts(kMsgPathSet, hops_short, sizeof(hops_short)));
  std::vector<uint8_t> too_many_hops = {kPathHops, 3, 32};
  too_many_hops.resize(3 + 3 * 32, 0x11);
  EXPECT_FALSE(decodeAccepts(kMsgPathSet, too_many_hops.data(), too_many_hops.size()))
      << "96 bytes of hops: more than the 64 a path may carry";
  std::vector<uint8_t> legal_hops = {kPathHops, 2, 32};
  legal_hops.resize(3 + 2 * 32, 0x11);
  EXPECT_TRUE(decodeAccepts(kMsgPathSet, legal_hops.data(), legal_hops.size()));

  // A path with its two size bytes set under a mode that has no hops: ignored, as the reference
  // implementation ignores them, and the hops are not read.
  const uint8_t noisy_direct[] = {kPathDirect, 2, 2};
  Path p;
  ASSERT_TRUE(decodePathSet(bytesOf(noisy_direct, sizeof(noisy_direct)), p));
  EXPECT_EQ(kPathDirect, p.mode);
  EXPECT_EQ(0, p.hash_size);
  EXPECT_EQ(0, p.hop_count);

  // A CLI line: 1 to 160 bytes (section 5.7).
  std::vector<uint8_t> line(kCliLineMax, 'x');
  EXPECT_TRUE(decodeAccepts(kMsgCli, line.data(), line.size()));
  line.push_back('x');
  EXPECT_FALSE(decodeAccepts(kMsgCli, line.data(), line.size()));
  EXPECT_FALSE(decodeAccepts(kMsgCli, line.data(), 0));

  // SEND_CLI's line: 1 to 148 bytes. SEND_LOGIN's password: at most 15.
  std::vector<uint8_t> send_cli(5 + size_t(kSendCliTextMax), 'x');
  EXPECT_TRUE(decodeAccepts(kMsgSendCli, send_cli.data(), send_cli.size()));
  send_cli.push_back('x');
  EXPECT_FALSE(decodeAccepts(kMsgSendCli, send_cli.data(), send_cli.size()));
  std::vector<uint8_t> login(4 + size_t(kLoginPasswordMax), 'p');
  EXPECT_TRUE(decodeAccepts(kMsgSendLogin, login.data(), login.size()));
  login.push_back('p');
  EXPECT_FALSE(decodeAccepts(kMsgSendLogin, login.data(), login.size()));

  // A str8 whose length byte promises more than the payload holds (HELLO's fw_version).
  const uint8_t lying_str8[] = {1, 0, 1, 240, 9, 'a', 'b'};
  EXPECT_FALSE(decodeAccepts(kMsgHello, lying_str8, sizeof(lying_str8)));
}

TEST(MalformedRequests, TrailingBytesAreIgnored) {
  // A minor version adds fields at the end of a payload and an older receiver ignores them
  // (section 5.1, "Versions").
  std::vector<uint8_t> ping = {0xEF, 0xBE, 0xAD, 0xDE, 0x01, 0x02, 0x03};
  Ping m;
  ASSERT_TRUE(decodePing(bytesOf(ping.data(), ping.size()), m));
  EXPECT_EQ(0xDEADBEEFu, m.nonce);

  std::vector<uint8_t> status_request = {0x01, 0x02};
  EXPECT_TRUE(decodeNoFields(bytesOf(status_request.data(), status_request.size())));
}

// ----------------------------------------------------------------------------- encoders refuse

TEST(Encoders, RefuseABufferTooSmall) {
  uint8_t out[kMaxPayload];
  PingResponse ping;
  ping.nonce = 1;
  ping.uptime_s = 2;
  EXPECT_EQ(0u, encodePingResponse(ping, out, 8));
  EXPECT_EQ(9u, encodePingResponse(ping, out, 9));
  EXPECT_EQ(0u, encodeStatus(kOk, out, 0));

  // A CLI reply is at most 160 bytes (section 5.7); a longer one is a caller's bug, not something
  // to put on the wire truncated.
  std::vector<uint8_t> text(size_t(kCliReplyMax) + 1, 'x');
  CliResponse cli;
  cli.text = bytesOf(text.data(), text.size());
  EXPECT_EQ(0u, encodeCliResponse(cli, out, sizeof(out)));
  text.pop_back();
  cli.text = bytesOf(text.data(), text.size());
  EXPECT_EQ(size_t(kCliReplyMax) + 1, encodeCliResponse(cli, out, sizeof(out)));
}

TEST(Encoders, RefuseAFieldThatIsNotLegal) {
  uint8_t out[kMaxPayload];

  Path p;
  p.mode = 3;
  EXPECT_EQ(0u, encodePathGetResponse(p, out, sizeof(out)));
  p.mode = kPathHops;
  p.hash_size = 2;
  p.hop_count = 2;
  const uint8_t three[3] = {1, 2, 3};
  p.hashes = bytesOf(three, sizeof(three));
  EXPECT_EQ(0u, encodePathGetResponse(p, out, sizeof(out))) << "three bytes for two hops of two";
  const uint8_t four[4] = {1, 2, 3, 4};
  p.hashes = bytesOf(four, sizeof(four));
  EXPECT_EQ(8u, encodePathGetResponse(p, out, sizeof(out)))
      << "status, mode, hash_size, hop_count and four bytes of hops";

  IdentityResponse id;
  id.name = bytesOfText("TwinOak");
  EXPECT_EQ(0u, encodeIdentityResponse(id, out, sizeof(out))) << "no public key";

  ReplyRecord rec;
  rec.state = 6;
  EXPECT_EQ(0u, encodeReplyGetResponse(rec, out, sizeof(out)));
  EXPECT_EQ(0u, encodeEvMeshReply(rec, out, sizeof(out)));

  // An observation whose path does not match its encoded path_len, and one with too much
  // application data (section 7).
  uint8_t key[kPubKeyLen];
  memset(key, 0x5A, sizeof(key));
  const uint8_t path[4] = {1, 2, 3, 4};
  Observation o;
  o.pubkey = key;
  o.path_len = 0x42;  // two bytes a hop, two hops: four bytes of path
  o.path = bytesOf(path, 3);
  ObsPullResponse pull;
  pull.records = &o;
  pull.count = 1;
  EXPECT_EQ(0u, encodeObsPullResponse(pull, out, sizeof(out)));
  o.path = bytesOf(path, sizeof(path));
  EXPECT_GT(encodeObsPullResponse(pull, out, sizeof(out)), 0u);
  std::vector<uint8_t> app(size_t(kObsAppDataMax) + 1, 0x11);
  o.app_data = bytesOf(app.data(), app.size());
  EXPECT_EQ(0u, encodeObsPullResponse(pull, out, sizeof(out)));

  // A pull that says it has records but hands over none.
  pull.records = nullptr;
  EXPECT_EQ(0u, encodeObsPullResponse(pull, out, sizeof(out)));
}

TEST(Encoders, ARecordSetThatDoesNotFitOnePayloadIsRefusedWhole) {
  // A record of a direct neighbour with no application data is 45 bytes, and the pull's own
  // header is five: five fit one payload and six do not. The encoder says so rather than leaving
  // half a record on the wire; how many to hand out is the session's arithmetic (task 03).
  uint8_t key[kPubKeyLen];
  memset(key, 0x33, sizeof(key));
  std::vector<Observation> records(6);
  for (Observation& o : records) {
    o.pubkey = key;
    o.path_len = 0;
  }
  ObsPullResponse pull;
  pull.records = records.data();
  pull.count = 6;
  uint8_t out[kMaxPayload];
  EXPECT_EQ(0u, encodeObsPullResponse(pull, out, sizeof(out)));
  pull.count = 5;
  EXPECT_EQ(5u + 5 * 45, encodeObsPullResponse(pull, out, sizeof(out)));
}

// ----------------------------------------------------------------------------- the numbers

TEST(TheNumbers, AreTheTablesOfTheSpecification) {
  // The few that a reader of section 3 and section 4 can check at a glance; the rest are held by
  // every case of the fixture above.
  EXPECT_EQ(0x81, responseType(kMsgHello));
  EXPECT_EQ(0xAB, responseType(kMsgMeshCounters));
  EXPECT_TRUE(isRequestType(kMsgHello));
  EXPECT_TRUE(isRequestType(0x5F));
  EXPECT_FALSE(isRequestType(0x60));
  EXPECT_FALSE(isRequestType(0x00));
  EXPECT_TRUE(isEventType(kMsgEvBoot));
  EXPECT_TRUE(isEventType(0x7F));
  EXPECT_FALSE(isEventType(0x80));
  EXPECT_EQ(240, kMaxPayload);
  EXPECT_EQ(244, kMaxPacket);
  EXPECT_EQ(246, kReceiveBuffer);

  // The version this firmware speaks (section 5.1). 1.2 since 7 November 2026; the image
  // descriptor's link_minor and the release manifest's `link` are both this constant
  // (test/test_image, tools/release/build_release.py).
  EXPECT_EQ(1, kProtoMajor);
  EXPECT_EQ(2, kProtoMinor);
  EXPECT_EQ(kFixtures[kFixtureCount - 1].link,
            std::to_string(kProtoMajor) + "." + std::to_string(kProtoMinor))
      << "the newest fixture file is not the version this firmware speaks";

  // An observation's path_len, MeshCore's encoding (section 7).
  EXPECT_EQ(1, obsHashSize(0x00));
  EXPECT_EQ(0, obsHopCount(0x00));
  EXPECT_EQ(0, obsPathBytes(0x00));
  EXPECT_EQ(2, obsHashSize(0x42));
  EXPECT_EQ(2, obsHopCount(0x42));
  EXPECT_EQ(4, obsPathBytes(0x42));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
