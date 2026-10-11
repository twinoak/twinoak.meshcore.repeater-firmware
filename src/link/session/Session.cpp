#include "Session.h"

#include <string.h>

namespace twinoak {
namespace link {
namespace {

// The two bytes that wake a sleeping Walter: "the RAK sends 0x00 0x00, waits 300 ms, and sends
// the event" (section 8). They are not a frame - the far end reads them as two empty frames and
// drops them - and they are the only thing this session puts on the wire that is not one. What
// they are for is the start bit on the Walter's GPIO8.
const uint8_t kWakeBytes[2] = {0x00, 0x00};

// The five sends (section 6.2): the requests whose payload can carry a password, so the receiver
// is wiped after one is answered - whether it ran or was answered from the retry memory.
bool isSendType(uint8_t type) {
  return type == kMsgSendLogin || type == kMsgSendRequest || type == kMsgSendCli ||
         type == kMsgSendDiscovery || type == kMsgSendTrace;
}

}  // namespace

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
  // A reply from the mesh that arrived, or a wait that ran out, is reported before anything else
  // (section 6.3, EV_MESH_REPLY, exactly once): a request that is waiting is answered on the next
  // pass, which is microseconds away, and the Walter reads the reply before the answer to
  // whatever it asked next - a REPLY_GET for the same record, as often as not.
  if (pumpMeshReply(now_ms)) return;
  if (!request_waiting_) {
    // Nothing to answer: section 8's sequence may have the wire.
    pumpAttention(now_ms);
    return;
  }

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
  // A valid request is the Walter speaking: it is awake, and it ends a pending attention's
  // repeats (section 8). A retry counts - it is the same Walter saying the same thing - which is
  // why this is here and not inside execute().
  if (s_.attention != nullptr) s_.attention->noteRequest(pkt.type, now_ms);
  // And it restarts the mesh client's silence count (section 6.1: a job is ended by the RAK
  // after 120 seconds with no request on the link). A retry counts here too, for the same
  // reason, which is why this is above the memory check and not inside execute().
  if (s_.mesh != nullptr) s_.mesh->noteLinkRequest(now_ms);

  // Section 3: the same (type, seq) is not executed again; the remembered response is sent again.
  if (have_memory_ && mem_type_ == pkt.type && mem_seq_ == pkt.seq) {
    // A retried send transmits nothing - the handler does not run - but the receiver still holds
    // the frame's bytes, and for a send that frame carried a password (section 6). The remembered
    // response is the client's answer and holds none; the receiver is wiped all the same, because
    // a lost response that is being retried must not leave the password in RAM a second time.
    const uint8_t type = pkt.type;
    const uint8_t seq = pkt.seq;
    sendResponse(type, seq, resp_len_, now_ms);
    if (isSendType(type)) rx_.wipe();
    return;
  }
  execute(pkt, now_ms);
}

void Session::outgoingSent(uint32_t now_ms) {
  has_outgoing_ = false;
  ever_sent_ = true;
  last_tx_ms_ = now_ms;
  if (s_.attention != nullptr) {
    if (out_is_response_) {
      // The response that ends an attention still carried its reason in STATUS; now it is told.
      s_.attention->requestAnswered(now_ms);
    } else if (out_len_ == sizeof(kWakeBytes) && out_ptr_ == kWakeBytes) {
      s_.attention->wakeSent(now_ms);
    } else if (wrote_attention_event_) {
      wrote_attention_event_ = false;
      s_.attention->eventSent(now_ms);
    }
  }
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
  // `pkt` is a view into the receiver, and a send handler wipes the receiver before it returns
  // (the five sends below): the response's type and seq are taken before anything can touch it.
  const uint8_t type = pkt.type;
  const uint8_t seq = pkt.seq;
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

    case kMsgCli: {
      // Section 5.7: one command line, run by the firmware's own interpreter as a command typed
      // on its serial console - everything the console can do, including the commands the
      // firmware refuses over the mesh. The Walter is trusted with that: it already holds the
      // SWD lines.
      if (s_.cli == nullptr) {
        n = encodeStatus(kErrUnsupported, resp_, sizeof(resp_));
        break;
      }
      Cli m;
      if (!decodeCli(pkt.body, m)) {
        // An empty line, a line over 160 bytes, or one that is not UTF-8 text without control
        // characters (Messages.cpp's decodeCli, Bytes.h's isPlainText).
        n = encodeStatus(kErrMalformed, resp_, sizeof(resp_));
        break;
      }
      n = cliHandler(m);
      break;
    }

    case kMsgPowerGet: {
      // Section 5.8: the three channels' bus voltage and current, the board's per-channel shunt
      // already applied. The numbers come out of the reader's cache and **no I2C happens here**
      // (src/power/PowerReader.h): a request must not wait on a bus.
      if (s_.power == nullptr) {
        n = encodeStatus(kErrUnsupported, resp_, sizeof(resp_));
        break;
      }
      PowerResponse out;
      if (!s_.power->readPower(out)) {
        n = encodeStatus(kErrFailed, resp_, sizeof(resp_));
        break;
      }
      n = encodePowerResponse(out, resp_, sizeof(resp_));
      break;
    }

    case kMsgEnvGet: {
      // Section 5.8, the same way: the last sample of the part in the enclosure, and `valid` 0
      // on a unit that has none.
      if (s_.env == nullptr) {
        n = encodeStatus(kErrUnsupported, resp_, sizeof(resp_));
        break;
      }
      EnvResponse out;
      if (!s_.env->readEnv(out)) {
        n = encodeStatus(kErrFailed, resp_, sizeof(resp_));
        break;
      }
      n = encodeEnvResponse(out, resp_, sizeof(resp_));
      break;
    }

    case kMsgLppGet: {
      // "The Cayenne-LPP bytes the node would answer a mesh telemetry request with at those
      // permissions" (section 5.8). Section 4 gates this one on the CLI capability and not on a
      // bit of its own, so a build with no LppAccess answers kErrUnsupported.
      if (s_.lpp == nullptr) {
        n = encodeStatus(kErrUnsupported, resp_, sizeof(resp_));
        break;
      }
      LppGet m;
      if (!decodeLppGet(pkt.body, m)) {
        n = encodeStatus(kErrMalformed, resp_, sizeof(resp_));
        break;
      }
      // An unknown permission bit is ignored rather than refused (section 5.8), which is what
      // handing `perm_mask` straight through does.
      //
      // The bytes are written **into the response payload, after its status byte**, which is
      // exactly the payload encodeLppResponse() builds (test/test_link_codec holds that
      // function to this shape against the fixture) - and this way without a second buffer of
      // 180 bytes on a 2 KB stack.
      n = encodeStatus(kOk, resp_, sizeof(resp_));  // one byte: kOk
      n += s_.lpp->readLpp(m.perm_mask, resp_ + n, sizeof(resp_) - n);
      break;
    }

    case kMsgSleepNotice: {
      // Section 8: the Walter's last request before it sleeps. From here until the next valid
      // request this node sends no events but EV_BOOT and EV_ATTENTION (eventAllowed), and
      // STATUS's flag bit 2 says so. The machine is where that belief lives, so a build without
      // it cannot hold the belief and answers kErrUnsupported - which is how every other
      // capability of this session behaves and what HELLO's caps already told the Walter.
      if (s_.attention == nullptr) {
        n = encodeStatus(kErrUnsupported, resp_, sizeof(resp_));
        break;
      }
      SleepNotice m;
      if (!decodeSleepNotice(pkt.body, m)) {
        n = encodeStatus(kErrMalformed, resp_, sizeof(resp_));
        break;
      }
      // poll() has already called noteRequest(), which leaves this one request's flag alone for
      // exactly this moment (src/link/attention/Attention.cpp).
      s_.attention->sleepNotice(m.expect_back_s, now_ms);
      n = encodeStatus(kOk, resp_, sizeof(resp_));
      break;
    }

    case kMsgJobBegin: {
      // Section 6.1: ends any job in progress first, then makes this key the peer. The path is
      // judged by the codec against 6.1's bounds (decodeJobBegin), so a path that is not legal
      // is kErrMalformed here and the client is never asked - nothing changes, the job in
      // progress included.
      if (s_.mesh == nullptr) {
        n = encodeStatus(kErrUnsupported, resp_, sizeof(resp_));
        break;
      }
      JobBegin m;
      if (!decodeJobBegin(pkt.body, m)) {
        n = encodeStatus(kErrMalformed, resp_, sizeof(resp_));
        break;
      }
      n = encodeStatus(s_.mesh->beginJob(m, now_ms), resp_, sizeof(resp_));
      break;
    }

    case kMsgJobEnd: {
      // Section 6.1: the peer, the path, any awaited or held reply, and the shared secret - gone.
      // With no job it is still kOk: there is nothing to refuse.
      if (s_.mesh == nullptr) {
        n = encodeStatus(kErrUnsupported, resp_, sizeof(resp_));
        break;
      }
      s_.mesh->endJob();
      n = encodeStatus(kOk, resp_, sizeof(resp_));
      break;
    }

    case kMsgPathSet: {
      // Section 6.1: the job's path replaced (mode 0 forgets it); kErrState with no job; a path
      // that is not legal is kErrMalformed and changes nothing - the codec refuses it before the
      // client is asked, as for JOB_BEGIN.
      if (s_.mesh == nullptr) {
        n = encodeStatus(kErrUnsupported, resp_, sizeof(resp_));
        break;
      }
      Path p;
      if (!decodePathSet(pkt.body, p)) {
        n = encodeStatus(kErrMalformed, resp_, sizeof(resp_));
        break;
      }
      n = encodeStatus(s_.mesh->setPath(p), resp_, sizeof(resp_));
      break;
    }

    case kMsgPathGet: {
      // Section 6.1: the path the job holds now - which is also the route a flooded send's reply
      // came back by (6.3, "a returned path is learned").
      if (s_.mesh == nullptr) {
        n = encodeStatus(kErrUnsupported, resp_, sizeof(resp_));
        break;
      }
      Path p;
      const uint8_t status = s_.mesh->getPath(p);
      if (status != kOk) {
        n = encodeStatus(status, resp_, sizeof(resp_));
        break;
      }
      n = encodePathGetResponse(p, resp_, sizeof(resp_));
      break;
    }

    case kMsgSendLogin:
    case kMsgSendRequest:
    case kMsgSendCli:
    case kMsgSendDiscovery:
    case kMsgSendTrace: {
      // Section 6.2: one packet on the air, or none, and the wait armed in the same step. Every
      // rule - no job, a reply still awaited, what cannot be sent - is the client's.
      if (s_.mesh == nullptr) {
        n = encodeStatus(kErrUnsupported, resp_, sizeof(resp_));
        break;
      }
      n = sendHandler(pkt.type, pkt.body, now_ms);
      // "A password is in RAM for the length of the send" (section 6): the request's bytes are in
      // the receiver's buffer, and they are gone before the answer is framed - after every send,
      // not only a login or a line flagged sensitive, because a Walter that forgot the flag has
      // still sent a password and the cost is a loop over 492 bytes. The response remembered for a
      // retry is the client's answer (send, tag, sender_ts) and holds nothing of the request.
      rx_.wipe();
      break;
    }

    case kMsgReplyGet: {
      // Section 6.3: the record as it stands - pending, an outcome, or nothing held. The views in
      // it are the client's own storage and are encoded here and now.
      if (s_.mesh == nullptr) {
        n = encodeStatus(kErrUnsupported, resp_, sizeof(resp_));
        break;
      }
      ReplyRecord rec;
      s_.mesh->getReply(rec, now_ms);
      n = encodeReplyGetResponse(rec, resp_, sizeof(resp_));
      break;
    }

    case kMsgMeshCounters: {
      // Section 6.4: the radio's rx and tx, the client's own client_tx, the radio's floor, RSSI
      // and SNR - and the observation ring's three, which read zero until the ring exists and are
      // the ring's to answer when it does (obs_dropped is the same count STATUS carries).
      if (s_.mesh == nullptr) {
        n = encodeStatus(kErrUnsupported, resp_, sizeof(resp_));
        break;
      }
      MeshCountersResponse out;
      s_.mesh->getCounters(out);
      if (s_.observations != nullptr) out.obs_dropped = s_.observations->droppedSinceBoot();
      n = encodeMeshCountersResponse(out, resp_, sizeof(resp_));
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
  sendResponse(type, seq, n, now_ms);
}

// ----------------------------------------------------------------------------- the console

size_t Session::cliHandler(const Cli& m) {
  // A copy of the line, NUL-terminated: the interpreter takes a char* and writes into it
  // (CliAccess, Interfaces.h). decodeCli has already held the length to 1..kCliLineMax.
  memcpy(line_, m.text.ptr, m.text.len);
  line_[m.text.len] = 0;

  // The window upstream's own console loop gives its interpreter, and the guard behind it, both
  // cleared first: the guard so that a byte written past the window is seen afterwards, the
  // window so that a command which answers nothing answers nothing (upstream's console reads
  // reply[0] the same way).
  memset(reply_, 0, sizeof(reply_));
  s_.cli->runCommand(line_, reply_, kCliReplyMax);

  // The bytes decide, not the length the interpreter reports: one that returns a count it did not
  // write - or writes a reply it does not count - cannot make this read past the window.
  size_t text = 0;
  while (text < kCliReplyMax && reply_[text] != 0) text++;
  if (text >= kCliReplyMax - 1) cli_truncations_++;
  for (size_t i = kCliReplyMax; i < sizeof(reply_); i++) {
    if (reply_[i] != 0) {
      // A command wrote past the 160 bytes a console would have given it. The guard absorbed it;
      // what goes out is the window, which is what the console itself would have printed.
      cli_overruns_++;
      break;
    }
  }

  CliResponse out;
  out.text = bytesOf(reply_, text);
  return encodeCliResponse(out, resp_, sizeof(resp_));
}

// ----------------------------------------------------------------------------- the mesh client

size_t Session::sendHandler(uint8_t type, Bytes body, uint32_t now_ms) {
  // The decoders hold the lengths section 6.2 fixes - a password of at most 15 bytes, a line of
  // 1 to 148 - and read nothing past the payload; a payload that is not a legal value is
  // kErrMalformed and the client is never asked. What the client answers is one of Numbers.h's
  // statuses and, on kOk, the three fields of the send response.
  SendResponse out;
  uint8_t status = kErrMalformed;
  switch (type) {
    case kMsgSendLogin: {
      SendLogin m;
      if (decodeSendLogin(body, m)) status = s_.mesh->sendLogin(m, out, now_ms);
      break;
    }
    case kMsgSendRequest: {
      SendRequest m;
      if (decodeSendRequest(body, m)) status = s_.mesh->sendRequest(m, out, now_ms);
      break;
    }
    case kMsgSendCli: {
      SendCli m;
      if (decodeSendCli(body, m)) status = s_.mesh->sendCli(m, out, now_ms);
      break;
    }
    case kMsgSendDiscovery: {
      SendDiscovery m;
      if (decodeSendDiscovery(body, m)) status = s_.mesh->sendDiscovery(m, out, now_ms);
      break;
    }
    case kMsgSendTrace: {
      SendTrace m;
      if (decodeSendTrace(body, m)) status = s_.mesh->sendTrace(m, out, now_ms);
      break;
    }
    default:
      break;
  }
  if (status != kOk) return encodeStatus(status, resp_, sizeof(resp_));
  return encodeSendResponse(out, resp_, sizeof(resp_));
}

bool Session::pumpMeshReply(uint32_t now_ms) {
  if (s_.mesh == nullptr) return false;
  ReplyRecord rec;
  if (!s_.mesh->takeReplyEvent(rec, now_ms)) return false;
  // The client has let the outcome go as reported, whatever happens next: sendEvent() drops the
  // frame while the Walter is believed asleep (section 8), and REPLY_GET is then how it learns
  // the outcome - "an event is never the only way to learn something" (section 3).
  uint8_t body[kMaxPayload];
  const size_t n = encodeEvMeshReply(rec, body, sizeof(body));
  if (n == 0) return false;  // a record the encoder refuses: unreachable, and silence is safe
  sendEvent(kMsgEvMeshReply, body, n, now_ms);
  return has_outgoing_;
}

// ----------------------------------------------------------------------------- attention

void Session::pumpAttention(uint32_t now_ms) {
  if (s_.attention == nullptr) return;
  const uint8_t due = s_.attention->dueNow(now_ms);
  if (due == kAttentionDueWake) {
    out_ptr_ = kWakeBytes;
    out_len_ = sizeof(kWakeBytes);
    out_is_response_ = false;
    wrote_attention_event_ = false;
    has_outgoing_ = true;
    return;
  }
  if (due != kAttentionDueEvent) return;

  uint8_t body[8];
  const size_t n = encodeEvAttention(s_.attention->pendingEvent(), body, sizeof(body));
  if (n == 0) return;  // four bytes into eight: unreachable, and silence is the safe answer
  sendEvent(kMsgEvAttention, body, n, now_ms);
  // The machine counts the send when the bytes have gone and not when they were built
  // (outgoingSent), so a frame the UART could not take this pass is not counted twice.
  if (has_outgoing_) wrote_attention_event_ = true;
}

// ----------------------------------------------------------------------------- the frame out

void Session::sendResponse(uint8_t request_type, uint8_t seq, size_t len, uint32_t now_ms) {
  resp_len_ = len;
  mem_type_ = request_type;
  mem_seq_ = seq;
  have_memory_ = true;
  has_outgoing_ =
      writer_.build(responseType(request_type), seq, resp_, resp_len_, lineIdle(now_ms));
  out_ptr_ = writer_.bytes();
  out_len_ = writer_.size();
  out_is_response_ = has_outgoing_;
  wrote_attention_event_ = false;
}

void Session::sendEvent(uint8_t type, const uint8_t* body, size_t len, uint32_t now_ms) {
  // Section 8: while the Walter is believed asleep nothing leaves but EV_BOOT and EV_ATTENTION.
  // An event that is held back is not lost - "an event is never the only way to learn
  // something" (section 3), so the Walter asks with a request when it wakes.
  if (!eventAllowed(type)) return;
  has_outgoing_ = writer_.build(type, event_seq_, body, len, lineIdle(now_ms));
  out_ptr_ = writer_.bytes();
  out_len_ = writer_.size();
  out_is_response_ = false;
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
