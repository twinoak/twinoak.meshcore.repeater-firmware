// src/link/attention/: section 8 of the box link - asking for the Walter's attention, and
// keeping quiet while it sleeps - against a fake clock, and then through a **real** Session so
// that what goes on the wire is the bytes and not an intention.
//
// What is checked, in the order tasks/phase-1/05-local-management.md asks for it:
//   - the 300 ms gap between the two wake bytes and the first event;
//   - ten sends a second apart, with `repeat` 0 to 9, and an eleventh that never comes;
//   - ended early by any valid request;
//   - the hourly limit, per reason, counted from when it was raised;
//   - two reasons queued behind the pending one, and neither added twice;
//   - STATUS's `attention` before and after - including that the answer to the request which
//     ends an attention still carries its reason;
//   - SLEEP_NOTICE: the belief, the events held, EV_ATTENTION not held, and the next request;
//   - the boot attention, which this board never raises and the machine still knows how to;
//   - a millisecond clock that wraps under it.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "link/attention/Attention.h"
#include "link/codec/FrameReceiver.h"
#include "link/codec/FrameWriter.h"
#include "link/codec/Messages.h"
#include "link/codec/Numbers.h"
#include "link/session/Interfaces.h"
#include "link/session/Session.h"

namespace {

using namespace twinoak::link;  // NOLINT: a test of one namespace, and the names are the wire's

std::string toHex(const uint8_t* data, size_t len) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < len; i++) {
    out += digits[data[i] >> 4];
    out += digits[data[i] & 0x0F];
  }
  return out;
}

// ----------------------------------------------------------------------------- the machine alone

// A fake clock: the machine never reads a clock of its own, so "winding it" is passing a bigger
// number. Every call goes through this so that a test reads like a timeline.
struct Clock {
  uint32_t ms = 1000;
  void advance(uint32_t by) { ms += by; }
};

// The machine driven as the session drives it: ask what is due, do it, say it has gone.
// Returns what was sent, as a word, so that a test can assert a whole sequence.
std::string step(AttentionMachine& m, Clock& c) {
  const uint8_t due = m.dueNow(c.ms);
  if (due == kAttentionDueWake) {
    m.wakeSent(c.ms);
    return "wake";
  }
  if (due == kAttentionDueEvent) {
    const Attention e = m.pendingEvent();
    m.eventSent(c.ms);
    return "event:" + std::to_string(e.reason) + ":" + std::to_string(e.detail) + ":" +
           std::to_string(e.repeat);
  }
  return "";
}

}  // namespace

TEST(TheSequence, TheWakeBytesThenThreeHundredMillisecondsThenTenEventsASecondApart) {
  AttentionMachine m;
  Clock c;
  EXPECT_EQ(kAttentionNone, m.pendingReason());
  EXPECT_EQ("", step(m, c)) << "nothing is due when nothing was raised";

  ASSERT_TRUE(m.raise(kAttentionPersonAsked, 0, c.ms));
  EXPECT_EQ(kAttentionPersonAsked, m.pendingReason());

  // The wake bytes go at once.
  EXPECT_EQ("wake", step(m, c));
  EXPECT_EQ(0, m.sendsSoFar());

  // And then nothing for 300 ms, to the millisecond.
  c.advance(kAttentionWakeGapMs - 1);
  EXPECT_EQ("", step(m, c)) << "299 ms is not 300";
  c.advance(1);
  EXPECT_EQ("event:4:0:0", step(m, c));
  EXPECT_EQ(1, m.sendsSoFar());

  // Then every 1000 ms, with repeat counting up, nine more times.
  for (int repeat = 1; repeat < kAttentionRepeats; repeat++) {
    c.advance(kAttentionRepeatMs - 1);
    EXPECT_EQ("", step(m, c)) << "999 ms after send " << repeat;
    c.advance(1);
    EXPECT_EQ("event:4:0:" + std::to_string(repeat), step(m, c));
    EXPECT_EQ(repeat + 1, m.sendsSoFar());
  }
  EXPECT_EQ(kAttentionRepeats, m.sendsSoFar()) << "ten sends: repeat 0 to 9";

  // An eleventh never comes, however long the Walter stays quiet - and the reason is still
  // pending, because no request has arrived.
  c.advance(kAttentionRepeatMs * 100);
  EXPECT_EQ("", step(m, c));
  EXPECT_EQ(kAttentionPersonAsked, m.pendingReason())
      << "visible in STATUS until the Walter asks for something, used up or not";
}

TEST(TheSequence, AnyValidRequestEndsItAndTheReasonGoesWhenThatRequestIsAnswered) {
  AttentionMachine m;
  Clock c;
  ASSERT_TRUE(m.raise(kAttentionBatteryLow, 3450, c.ms));
  EXPECT_EQ("wake", step(m, c));
  c.advance(kAttentionWakeGapMs);
  EXPECT_EQ("event:2:3450:0", step(m, c));
  c.advance(kAttentionRepeatMs);
  EXPECT_EQ("event:2:3450:1", step(m, c));

  // The Walter woke up and asked for something: no more sends, from this moment.
  c.advance(10);
  m.noteRequest(kMsgStatus, c.ms);
  EXPECT_EQ(kAttentionBatteryLow, m.pendingReason())
      << "the answer to that very request still carries the reason";
  c.advance(kAttentionRepeatMs * 5);
  EXPECT_EQ("", step(m, c)) << "and nothing more is ever sent for it";
  EXPECT_EQ(2, m.sendsSoFar());

  // The answer has gone: the reason is told, and STATUS says 0 from here on.
  m.requestAnswered(c.ms);
  EXPECT_EQ(kAttentionNone, m.pendingReason());
  EXPECT_EQ("", step(m, c));
}

TEST(TheSequence, ARequestThatArrivesBeforeTheFirstEventStopsItAltogether) {
  AttentionMachine m;
  Clock c;
  ASSERT_TRUE(m.raise(kAttentionPersonAsked, 0, c.ms));
  EXPECT_EQ("wake", step(m, c));
  c.advance(10);
  m.noteRequest(kMsgHello, c.ms);
  c.advance(kAttentionWakeGapMs * 10);
  EXPECT_EQ("", step(m, c)) << "the Walter is awake and has been told; there is nothing to send";
  EXPECT_EQ(0, m.sendsSoFar());
  m.requestAnswered(c.ms);
  EXPECT_EQ(kAttentionNone, m.pendingReason());
}

// ----------------------------------------------------------------------------- the rate limit

TEST(TheRateLimit, OneNewAttentionPerReasonPerHourCountedFromWhenItWasRaised) {
  AttentionMachine m;
  Clock c;
  ASSERT_TRUE(m.raise(kAttentionPersonAsked, 0, c.ms));
  EXPECT_EQ(1u, m.raised());

  // It is told, and the attention is over.
  m.noteRequest(kMsgStatus, c.ms);
  m.requestAnswered(c.ms);
  ASSERT_EQ(kAttentionNone, m.pendingReason());

  // A second one inside the hour raises nothing at all.
  c.advance(kAttentionLimitMs - 1);
  EXPECT_FALSE(m.raise(kAttentionPersonAsked, 0, c.ms));
  EXPECT_EQ(kAttentionNone, m.pendingReason());
  EXPECT_EQ("", step(m, c));
  EXPECT_EQ(1u, m.rateLimited());

  // Another reason is not limited by it: the limit is per reason.
  EXPECT_TRUE(m.raise(kAttentionBatteryCritical, 3100, c.ms));
  EXPECT_EQ(kAttentionBatteryCritical, m.pendingReason());
  m.noteRequest(kMsgStatus, c.ms);
  m.requestAnswered(c.ms);

  // And an hour after the first one was raised, reason 4 may be raised again.
  c.advance(1);
  EXPECT_TRUE(m.raise(kAttentionPersonAsked, 0, c.ms));
  EXPECT_EQ(kAttentionPersonAsked, m.pendingReason());
  EXPECT_EQ(3u, m.raised());
  EXPECT_EQ(1u, m.rateLimited());
}

TEST(TheRateLimit, AReasonAlreadyPendingOrQueuedIsNotAddedTwice) {
  AttentionMachine m;
  Clock c;
  ASSERT_TRUE(m.raise(kAttentionRingNearlyFull, 200, c.ms));
  EXPECT_FALSE(m.raise(kAttentionRingNearlyFull, 250, c.ms)) << "already pending";
  EXPECT_EQ(0, m.queued());
  EXPECT_EQ(0u, m.rateLimited()) << "and it is not the rate limit that refused it";

  ASSERT_TRUE(m.raise(kAttentionFault, 7, c.ms));
  EXPECT_EQ(1, m.queued());
  EXPECT_FALSE(m.raise(kAttentionFault, 8, c.ms)) << "already queued";
  EXPECT_EQ(1, m.queued());

  // The detail of the first raise is the one that is kept: 250 and 8 were never news.
  EXPECT_EQ(200u, m.pendingEvent().detail);
}

TEST(TheRateLimit, AMillisecondClockThatWrapsDoesNotOpenTheGate) {
  AttentionMachine m;
  Clock c;
  c.ms = 0xFFFFFF00;  // 256 ms before the wrap
  ASSERT_TRUE(m.raise(kAttentionPersonAsked, 0, c.ms));
  m.noteRequest(kMsgStatus, c.ms);
  m.requestAnswered(c.ms);

  c.advance(1000);  // past the wrap, and only 744 ms of real time later
  EXPECT_FALSE(m.raise(kAttentionPersonAsked, 0, c.ms)) << "a wrap is not an hour";
  c.advance(kAttentionLimitMs - 1000);
  EXPECT_TRUE(m.raise(kAttentionPersonAsked, 0, c.ms)) << "and an hour after it still is";
}

// ----------------------------------------------------------------------------- the queue

TEST(TheQueue, OnePendingAtATimeAndTheRestBehindIt) {
  AttentionMachine m;
  Clock c;
  ASSERT_TRUE(m.raise(kAttentionBatteryLow, 3450, c.ms));
  ASSERT_TRUE(m.raise(kAttentionPersonAsked, 0, c.ms));
  ASSERT_TRUE(m.raise(kAttentionFault, 9, c.ms));
  EXPECT_EQ(kAttentionBatteryLow, m.pendingReason()) << "the first one raised is the pending one";
  EXPECT_EQ(2, m.queued());

  EXPECT_EQ("wake", step(m, c));
  c.advance(kAttentionWakeGapMs);
  EXPECT_EQ("event:2:3450:0", step(m, c)) << "and only it is sent";

  // A request ends it; the next takes its place, with its own wake bytes and its own repeat 0.
  m.noteRequest(kMsgStatus, c.ms);
  m.requestAnswered(c.ms);
  EXPECT_EQ(kAttentionPersonAsked, m.pendingReason());
  EXPECT_EQ(1, m.queued());
  EXPECT_EQ("wake", step(m, c));
  c.advance(kAttentionWakeGapMs);
  EXPECT_EQ("event:4:0:0", step(m, c));

  m.noteRequest(kMsgPing, c.ms);
  m.requestAnswered(c.ms);
  EXPECT_EQ(kAttentionFault, m.pendingReason());
  EXPECT_EQ(0, m.queued());
  m.noteRequest(kMsgPing, c.ms);
  m.requestAnswered(c.ms);
  EXPECT_EQ(kAttentionNone, m.pendingReason()) << "and then there are none";
}

TEST(TheQueue, EveryReasonAtOnceFitsAndNothingOutsideOneToSixIsTaken) {
  AttentionMachine m;
  Clock c;
  for (uint8_t reason = kAttentionRestarted; reason <= kAttentionFault; reason++) {
    EXPECT_TRUE(m.raise(reason, reason, c.ms)) << unsigned(reason);
  }
  EXPECT_EQ(kAttentionRestarted, m.pendingReason());
  EXPECT_EQ(kAttentionFault - 1, m.queued()) << "one pending, the other five behind it";

  EXPECT_FALSE(m.raise(kAttentionNone, 0, c.ms)) << "0 is not a reason";
  EXPECT_FALSE(m.raise(7, 0, c.ms)) << "and nor is 7, until a later minor version says so";
}

// ----------------------------------------------------------------------------- the boot

TEST(TheBoot, ReasonOneIsForAWalterThatWasAsleepAndForNobodyElse) {
  AttentionMachine awake;
  Clock c;
  EXPECT_FALSE(awake.noteBoot(kResetWatchdog, /*walter_was_asleep=*/false, c.ms))
      << "an awake Walter learns everything this event carries from EV_BOOT";
  EXPECT_EQ(kAttentionNone, awake.pendingReason());

  AttentionMachine asleep;
  EXPECT_TRUE(asleep.noteBoot(kResetWatchdog, /*walter_was_asleep=*/true, c.ms));
  EXPECT_EQ(kAttentionRestarted, asleep.pendingReason());
  EXPECT_EQ(kResetWatchdog, asleep.pendingEvent().detail) << "reason 1's detail is the reset reason";
}

// ----------------------------------------------------------------------------- sleep

TEST(TheBelief, SleepNoticeSetsItAndAnyOtherRequestClearsIt) {
  AttentionMachine m;
  Clock c;
  EXPECT_FALSE(m.walterAsleep());
  EXPECT_EQ(0u, m.expectBackSeconds());

  m.noteRequest(kMsgSleepNotice, c.ms);
  m.sleepNotice(900, c.ms);
  EXPECT_TRUE(m.walterAsleep());
  EXPECT_EQ(900u, m.expectBackSeconds());

  // A retried SLEEP_NOTICE is answered from the session's one-entry memory without the handler
  // running again (LINK-PROTOCOL.md section 3), so noteRequest alone must not undo the belief.
  m.noteRequest(kMsgSleepNotice, c.ms);
  EXPECT_TRUE(m.walterAsleep());

  c.advance(60000);
  m.noteRequest(kMsgHello, c.ms);
  EXPECT_FALSE(m.walterAsleep()) << "the Walter is back";
  EXPECT_EQ(900u, m.expectBackSeconds()) << "what it said last time is still what it said";
}

TEST(TheBelief, OnlyEvBootAndEvAttentionAreAllowedWhileItSleeps) {
  EXPECT_TRUE(eventAllowedAsleep(kMsgEvBoot));
  EXPECT_TRUE(eventAllowedAsleep(kMsgEvAttention));
  EXPECT_FALSE(eventAllowedAsleep(kMsgEvMeshReply));
  // Every event type of section 4, and the room above them a later minor version may use.
  for (uint8_t type = kEventFirst; type != 0 && type <= kEventLast; type++) {
    const bool allowed = eventAllowedAsleep(type);
    EXPECT_EQ(type == kMsgEvBoot || type == kMsgEvAttention, allowed) << unsigned(type);
  }
}

// ----------------------------------------------------------------------------- through a session

namespace {

// Everything a session needs that is not the attention machine. The box link's own suite
// (test/test_link_session) is where these are exercised.
class StubSystem : public SystemAccess {
 public:
  uint32_t uptimeSeconds() const override { return 42; }
  uint8_t resetReason() const override { return kResetPowerOn; }
  uint32_t bootId() const override { return 0; }
  bool radioUp() const override { return true; }
  bool canRestart(uint8_t) const override { return true; }
  void restartNow(uint8_t) override {}
};

class StubIdentity : public IdentityAccess {
 public:
  uint8_t key[kPubKeyLen] = {0};
  const uint8_t* publicKey() const override { return key; }
  const char* nodeName() const override { return ""; }
};

// A real Session with the real machine behind it, driven exactly as src/link/SerialTransport.cpp
// drives it. Nothing here fakes a frame.
struct Rig {
  Rig() {
    firmware.role = kRoleRepeater;
    services.firmware = &firmware;
    services.system = &system;
    services.identity = &identity;
    services.attention = &machine;
  }

  // One pass: read what is waiting, poll, and take whatever the session hands the UART.
  void pass() {
    while (!session.requestWaiting() && !in.empty()) {
      session.feedByte(in.front(), now);
      in.erase(in.begin());
    }
    session.poll(now);
    if (session.hasOutgoing()) {
      sent.push_back(toHex(session.outgoing(), session.outgoingSize()));
      session.outgoingSent(now);
    }
  }

  void feed(const std::vector<uint8_t>& frame) {
    for (uint8_t b : frame) in.push_back(b);
  }

  // The last thing sent, without the leading zero of an idle line.
  std::string last() const {
    if (sent.empty()) return "";
    const std::string& f = sent.back();
    return f.rfind("00", 0) == 0 && f.size() > 4 ? f.substr(2) : f;
  }

  Firmware firmware;
  StubSystem system;
  StubIdentity identity;
  AttentionMachine machine;
  Services services;
  Session session{services};
  std::vector<uint8_t> in;
  std::vector<std::string> sent;
  uint32_t now = 10000;
};

std::vector<uint8_t> frameOf(uint8_t type, uint8_t seq, const std::vector<uint8_t>& body) {
  FrameWriter w;
  EXPECT_TRUE(w.build(type, seq, body.empty() ? nullptr : body.data(), body.size(), false));
  return std::vector<uint8_t>(w.bytes(), w.bytes() + w.size());
}

// The (reason, detail, repeat) of an EV_ATTENTION frame, or (0,0,0) if it is not one.
Attention attentionIn(const std::string& frame_hex) {
  FrameReceiver rx;
  for (size_t i = 0; i + 1 < frame_hex.size(); i += 2) {
    rx.feedByte(uint8_t(std::stoul(frame_hex.substr(i, 2), nullptr, 16)));
  }
  Attention e;
  if (!rx.hasPacket() || rx.packet().type != kMsgEvAttention) return e;
  Reader r(rx.packet().body);
  e.reason = r.u8();
  e.detail = r.u16();
  e.repeat = r.u8();
  return e;
}

}  // namespace

TEST(ThroughTheLink, TheWholeSequenceOnTheWire) {
  Rig r;
  ASSERT_TRUE(r.machine.raise(kAttentionPersonAsked, 0, r.now));

  // The wake bytes: two zeros, and not a frame.
  r.pass();
  ASSERT_EQ(1u, r.sent.size());
  EXPECT_EQ("0000", r.sent[0]);

  // Nothing at all for 300 ms, and then the first event.
  r.now += kAttentionWakeGapMs - 1;
  r.pass();
  EXPECT_EQ(1u, r.sent.size());
  r.now += 1;
  r.pass();
  ASSERT_EQ(2u, r.sent.size());
  EXPECT_EQ(kAttentionPersonAsked, attentionIn(r.last()).reason);
  EXPECT_EQ(0, attentionIn(r.last()).repeat);

  // Then nine more, a second apart, with repeat counting up.
  for (int repeat = 1; repeat < kAttentionRepeats; repeat++) {
    r.now += kAttentionRepeatMs;
    r.pass();
    ASSERT_EQ(size_t(repeat + 2), r.sent.size()) << "send " << repeat;
    EXPECT_EQ(kAttentionPersonAsked, attentionIn(r.last()).reason);
    EXPECT_EQ(repeat, attentionIn(r.last()).repeat);
  }

  // And no more, ever.
  for (int i = 0; i < 5; i++) {
    r.now += kAttentionRepeatMs;
    r.pass();
  }
  EXPECT_EQ(size_t(kAttentionRepeats + 1), r.sent.size()) << "the wake bytes and ten events";
  EXPECT_EQ(kAttentionPersonAsked, r.machine.pendingReason());

  // STATUS still carries the reason - and clears it, because its own answer is what tells it.
  r.feed(frameOf(kMsgStatus, 1, {}));
  r.pass();
  ASSERT_EQ(size_t(kAttentionRepeats + 2), r.sent.size());
  FrameReceiver rx;
  for (size_t i = 0; i + 1 < r.last().size(); i += 2) {
    rx.feedByte(uint8_t(std::stoul(r.last().substr(i, 2), nullptr, 16)));
  }
  ASSERT_TRUE(rx.hasPacket());
  EXPECT_EQ(responseType(kMsgStatus), rx.packet().type);
  Reader body(rx.packet().body);
  body.u8();
  body.u32();
  body.u8();
  body.u8();
  body.u32();
  body.u16();
  body.u8();
  body.u16();
  body.u16();
  EXPECT_EQ(kAttentionPersonAsked, body.u8()) << "STATUS's attention, in the answer that ends it";
  EXPECT_EQ(kAttentionNone, r.machine.pendingReason()) << "and nothing afterwards";

  r.now += kAttentionRepeatMs;
  r.pass();
  EXPECT_EQ(size_t(kAttentionRepeats + 2), r.sent.size()) << "the wire goes quiet";
}

TEST(ThroughTheLink, SleepNoticeHoldsTheEventsThatSectionEightHolds) {
  Rig r;
  // SLEEP_NOTICE, through the real session: expect_back_s 900.
  r.feed(frameOf(kMsgSleepNotice, 10, {0x84, 0x03, 0x00, 0x00}));
  r.pass();
  ASSERT_EQ(1u, r.sent.size());
  EXPECT_TRUE(r.machine.walterAsleep());
  EXPECT_EQ(900u, r.machine.expectBackSeconds());
  EXPECT_FALSE(r.session.eventAllowed(kMsgEvMeshReply)) << "phase 2's event waits";
  EXPECT_TRUE(r.session.eventAllowed(kMsgEvAttention));

  // An attention raised while it sleeps is exactly what the sleeping Walter is woken for.
  ASSERT_TRUE(r.machine.raise(kAttentionBatteryCritical, 3100, r.now));
  r.pass();
  EXPECT_EQ("0000", r.sent.back());
  r.now += kAttentionWakeGapMs;
  r.pass();
  ASSERT_EQ(3u, r.sent.size());
  EXPECT_EQ(kAttentionBatteryCritical, attentionIn(r.last()).reason);
  EXPECT_EQ(3100u, attentionIn(r.last()).detail);
  EXPECT_TRUE(r.machine.walterAsleep()) << "sending to it does not mean it woke";

  // The Walter wakes and asks: the belief goes, and so does the attention once it is answered.
  r.feed(frameOf(kMsgPing, 3, {0, 0, 0, 0}));
  r.pass();
  EXPECT_FALSE(r.machine.walterAsleep());
  EXPECT_EQ(kAttentionNone, r.machine.pendingReason());
  EXPECT_TRUE(r.session.eventAllowed(kMsgEvMeshReply));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
