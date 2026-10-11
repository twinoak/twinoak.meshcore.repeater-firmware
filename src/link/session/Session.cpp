#include "Session.h"

#include <string.h>

namespace twinoak {
namespace link {

// ----------------------------------------------------------------------------- the frame in

bool Session::feedByte(uint8_t b, uint32_t now_ms) {
  // A request already waiting must not be overwritten: the receiver's buffer still holds its
  // bytes and the transport is told to stop reading.
  if (request_waiting_) return true;
  (void)now_ms;  // the clock matters when a request is executed, not when its last byte arrives
  if (rx_.feedByte(b)) {
    request_waiting_ = true;
    return true;
  }
  return false;
}

// ----------------------------------------------------------------------------- the pass

void Session::poll(uint32_t now_ms) {
  if (restart_armed_ && int32_t(now_ms - restart_at_ms_) >= 0) {
    restart_armed_ = false;  // cleared first: if the board cannot restart, the link carries on
    s_.system->restartNow(restart_mode_);
  }
  if (has_outgoing_) return;  // the writer still holds something unsent
  if (!request_waiting_) return;

  const ParsedPacket& pkt = rx_.packet();
  request_waiting_ = false;

  // In 1.0 only the Walter sends requests (section 3). A frame that passed the CRC but carries a
  // response or an event type is not ours to answer - an answer to it would have a type nothing
  // on this link reads - so it is dropped and nothing is sent, as for any frame a receiver
  // refuses. It is not counted in link_errors, which is the receiver's own count of the frames it
  // could not parse.
  if (!isRequestType(pkt.type)) return;

  saw_request_ = true;
  last_request_ms_ = now_ms;

  // Section 3: the same (type, seq) is not executed again; the remembered response is sent again.
  if (have_memory_ && mem_type_ == pkt.type && mem_seq_ == pkt.seq) {
    sendResponse(pkt.type, pkt.seq, resp_len_, now_ms);
    return;
  }
  execute(pkt, now_ms);
}

void Session::outgoingSent(uint32_t now_ms) {
  has_outgoing_ = false;
  ever_sent_ = true;
  last_tx_ms_ = now_ms;
  // "The response is sent first; the restart follows after the delay, which leaves time for the
  // response to leave the UART" (section 5.5). The delay starts here, when the bytes have been
  // handed to the UART; the board's restartNow() flushes whatever is still in flight.
  if (restart_asked_) {
    restart_asked_ = false;
    restart_armed_ = true;
    restart_at_ms_ = now_ms + restart_delay_ms_;
  }
}

bool Session::lineIdle(uint32_t now_ms) const {
  if (!ever_sent_) return true;
  return (now_ms - last_tx_ms_) >= kIdleBeforeLeadingZeroMs;
}

// ----------------------------------------------------------------------------- what we are

HelloBody Session::helloBody(uint8_t proto_minor) const {
  HelloBody b;
  b.proto_major = kProtoMajor;
  b.proto_minor = proto_minor;
  b.role = s_.firmware->role;
  b.slot = s_.firmware->slot;
  b.max_payload = uint8_t(kMaxPayload);
  b.caps = capsOf(s_);
  b.boot_id = s_.system->bootId();
  b.fw_version = bytesOfText(s_.firmware->version);
  b.board = bytesOfText(s_.firmware->board);
  b.upstream = bytesOfText(s_.firmware->upstream);
  return b;
}

// ----------------------------------------------------------------------------- the handlers

void Session::execute(const ParsedPacket& pkt, uint32_t now_ms) {
  size_t n = 0;
  executed_++;

  switch (pkt.type) {
    case kMsgHello: {
      Hello m;
      if (!decodeHello(pkt.body, m)) {
        n = encodeStatus(kErrMalformed, resp_, sizeof(resp_));
        break;
      }
      // Section 5.1, "Versions": a RAK that does not speak the request's major answers
      // kErrUnsupported, and the Walter may try another major it knows.
      if (m.proto_major != kProtoMajor) {
        n = encodeStatus(kErrUnsupported, resp_, sizeof(resp_));
        break;
      }
      // Within a major, the highest minor both ends know (Numbers.h).
      n = encodeHelloResponse(helloBody(highestMinorBothKnow(m.proto_minor, kProtoMinor)), resp_,
                              sizeof(resp_));
      break;
    }

    case kMsgPing: {
      Ping m;
      if (!decodePing(pkt.body, m)) {
        n = encodeStatus(kErrMalformed, resp_, sizeof(resp_));
        break;
      }
      PingResponse out;
      out.nonce = m.nonce;  // echoed: section 5.2
      out.uptime_s = s_.system->uptimeSeconds();
      n = encodePingResponse(out, resp_, sizeof(resp_));
      break;
    }

    case kMsgStatus: {
      // Section 5.3. Every field a later task owns is read through that task's interface, and
      // reads zero - with its flag bit clear - for as long as the interface is absent.
      StatusResponse out;
      out.uptime_s = s_.system->uptimeSeconds();
      out.reset_reason = s_.system->resetReason();
      out.link_errors = rx_.drops();
      if (s_.system->radioUp()) out.flags |= kFlagRadioUp;
      if (s_.clock != nullptr) {
        out.time_state = s_.clock->timeState();
        out.epoch = s_.clock->epoch();
        if (s_.clock->advertsHeld()) out.flags |= kFlagAdvertsHeld;
      }
      if (s_.mesh != nullptr) {
        if (s_.mesh->jobBegun()) out.flags |= kFlagJobBegun;
        if (s_.mesh->replyAwaited()) out.flags |= kFlagReplyAwaited;
      }
      if (s_.observations != nullptr) {
        out.obs_pending = s_.observations->pending();
        out.obs_dropped = s_.observations->droppedSinceBoot();
      }
      if (s_.attention != nullptr) {
        out.attention = s_.attention->pendingReason();
        if (s_.attention->walterAsleep()) out.flags |= kFlagWalterAsleep;
      }
      n = encodeStatusResponse(out, resp_, sizeof(resp_));
      break;
    }

    case kMsgTimeSet: {
      // Section 5.4. Every rule about which values are acceptable is the clock keeper's
      // (src/clock/ClockKeeper.cpp); the session carries the answer back and nothing more.
      if (s_.clock == nullptr) {
        n = encodeStatus(kErrUnsupported, resp_, sizeof(resp_));
        break;
      }
      TimeSet m;
      if (!decodeTimeSet(pkt.body, m)) {
        n = encodeStatus(kErrMalformed, resp_, sizeof(resp_));
        break;
      }
      // `source` is which clock the Walter took the time from (1 the cellular network, 2 the
      // server, 3 a person at a console). Nothing here behaves differently for it, and a value
      // this firmware does not know is not a reason to refuse a time: a later minor version may
      // add one (section 5.1, "Versions").
      uint32_t prev = 0;
      if (!s_.clock->setTime(m.epoch, (m.flags & kTimeForce) != 0, prev)) {
        n = encodeStatus(kErrRange, resp_, sizeof(resp_));
        break;
      }
      TimeSetResponse out;
      out.prev_epoch = prev;
      out.epoch = s_.clock->epoch();
      out.time_state = s_.clock->timeState();
      n = encodeTimeSetResponse(out, resp_, sizeof(resp_));
      break;
    }

    case kMsgTimeGet: {
      // Section 5.4: the clock and its state, and nothing else.
      if (s_.clock == nullptr) {
        n = encodeStatus(kErrUnsupported, resp_, sizeof(resp_));
        break;
      }
      TimeGetResponse out;
      out.epoch = s_.clock->epoch();
      out.time_state = s_.clock->timeState();
      n = encodeTimeGetResponse(out, resp_, sizeof(resp_));
      break;
    }

    case kMsgIdentity: {
      // Section 5.6: the node's MeshCore public key and its advert name.
      IdentityResponse out;
      out.pubkey = s_.identity->publicKey();
      out.name = bytesOfText(s_.identity->nodeName());
      n = encodeIdentityResponse(out, resp_, sizeof(resp_));
      break;
    }

    case kMsgReboot: {
      Reboot m;
      if (!decodeReboot(pkt.body, m)) {
        n = encodeStatus(kErrMalformed, resp_, sizeof(resp_));
        break;
      }
      // Section 5.5: a mode the board cannot do is kErrRange, and nothing is scheduled.
      if (!s_.system->canRestart(m.mode)) {
        n = encodeStatus(kErrRange, resp_, sizeof(resp_));
        break;
      }
      restart_asked_ = true;
      restart_mode_ = m.mode;
      restart_delay_ms_ = m.delay_ms;
      n = encodeStatus(kOk, resp_, sizeof(resp_));
      break;
    }

    default:
      // "A request of an unknown type is answered ERR_UNSUPPORTED with that type's response code.
      // That is how a newer Walter discovers what an older RAK cannot do" (section 3). The types
      // of section 4 that later tasks own arrive here too, for the same reason: this firmware
      // does not know them yet, and does not advertise their capability either.
      n = encodeStatus(kErrUnsupported, resp_, sizeof(resp_));
      break;
  }

  if (n == 0) {
    // An encoder refused: a field no handler above can produce today. Say so rather than send
    // nothing, because silence after a request is a dead link as far as the Walter can tell.
    n = encodeStatus(kErrFailed, resp_, sizeof(resp_));
  }
  sendResponse(pkt.type, pkt.seq, n, now_ms);
}

// ----------------------------------------------------------------------------- the frame out

void Session::sendResponse(uint8_t request_type, uint8_t seq, size_t len, uint32_t now_ms) {
  resp_len_ = len;
  mem_type_ = request_type;
  mem_seq_ = seq;
  have_memory_ = true;
  has_outgoing_ =
      writer_.build(responseType(request_type), seq, resp_, resp_len_, lineIdle(now_ms));
}

void Session::sendEvent(uint8_t type, const uint8_t* body, size_t len, uint32_t now_ms) {
  has_outgoing_ = writer_.build(type, event_seq_, body, len, lineIdle(now_ms));
  if (has_outgoing_) event_seq_++;  // counted from 0 and wrapping (section 3)
}

void Session::begin(uint32_t now_ms) {
  // EV_BOOT: the reset reason and then the HELLO response body without its status byte
  // (section 5.1). Sent once, when the link comes up after a reset. It saves the Walter a round
  // trip; the Walter does not rely on it and sends HELLO when it wants to know.
  uint8_t body[kMaxPayload];
  const size_t n =
      encodeEvBoot(s_.system->resetReason(), helloBody(kProtoMinor), body, sizeof(body));
  if (n > 0) sendEvent(kMsgEvBoot, body, n, now_ms);
}

}  // namespace link
}  // namespace twinoak
