// The seams between the box link's session and the rest of the firmware: one small interface per
// thing the session needs to ask the board, and the struct of pointers it is given
// (docs/ARCHITECTURE.md section 4, layer 2).
//
// Two rules hold this together:
//
//   - **A capability is an interface that is there.** `capsOf()` below builds HELLO's `caps` word
//     from which pointers are non-null, so a firmware that has not implemented a thing simply
//     does not advertise it (spec/LINK-PROTOCOL.md 5.1) and a request for it is answered
//     kErrUnsupported. Nothing anywhere keeps a second list of what this build can do.
//   - **The session is portable.** It never includes Arduino or MeshCore; everything it knows
//     about the board arrives through these interfaces, which is what lets test/test_link_session
//     run the whole session against fakes.
//
// `firmware`, `system` and `identity` arrived with phase 1's task 03, and `clock` with its task
// 04 (src/roles/repeater/clock/LinkClock.h is what implements it). `firmware` and `system` are
// always present; `identity` is absent on the one boot that found a node's settings without its
// key and refused to make a new one (src/roles/repeater/app/IdentityGuard.h, pile 3.9's task
// 01), and IDENTITY then answers kErrState. The rest are declared here with the methods
// STATUS and `caps` need of them today; the task that implements one adds the methods its own
// messages need (05 the console and attention, 06 power and environment, phase 2 the mesh client
// and the observation ring).

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "common/link/codec/Messages.h"
#include "common/link/codec/Numbers.h"

namespace twinoak {
namespace link {

// What this image is: the strings and the role HELLO reports, which are the same strings the
// image descriptor carries (spec/FIRMWARE-MANIFEST.md section 4). Given to the session once; the
// strings must live as long as it does.
struct Firmware {
  uint8_t role = kRoleUnknown;
  uint8_t slot = 0;             // 0 on a single-radio unit
  const char* version = "";     // HELLO's fw_version, e.g. "0.3.0+abc1234"
  const char* board = "";       // e.g. "rak4630-single"
  const char* upstream = "";    // what it is built on, e.g. "repeater-v1.18.0+81dbae59"
  // HELLO's `band`, link 1.1 (spec/LINK-PROTOCOL.md 5.1): the band the running image was built
  // for, as its descriptor says it - "868". With `role` and `board` it is what a release's image
  // is chosen by, and HELLO is where the Walter reads all three until its first install commits.
  const char* band = "";
};

// The node itself: uptime, why it last restarted, the per-boot identifier, and the restart.
struct SystemAccess {
  virtual ~SystemAccess() {}

  virtual uint32_t uptimeSeconds() const = 0;
  // One of Numbers.h's ResetReason values (LINK-PROTOCOL.md section 5.3).
  virtual uint8_t resetReason() const = 0;
  // Random, drawn once per boot (section 5.1). A changed boot_id means the node restarted.
  virtual uint32_t bootId() const = 0;
  // STATUS flag bit 4.
  virtual bool radioUp() const = 0;

  // Whether this board can restart in that mode at all; a mode it cannot do is kErrRange
  // (section 5.5).
  virtual bool canRestart(uint8_t mode) const = 0;
  // Restarts now, in the mode a previous canRestart() accepted. Never returns. The session calls
  // it only after the response has been handed to the UART and the request's delay has passed.
  virtual void restartNow(uint8_t mode) = 0;
};

// The MeshCore identity this unit is one node of.
struct IdentityAccess {
  virtual ~IdentityAccess() {}

  virtual const uint8_t* publicKey() const = 0;  // kPubKeyLen bytes
  virtual const char* nodeName() const = 0;      // the advert name
};

// The clock keeper (phase 1's task 04, src/roles/repeater/clock/). Present => kCapTimePersist.
struct ClockAccess {
  virtual ~ClockAccess() {}

  virtual uint32_t epoch() const = 0;       // Unix seconds
  virtual uint8_t timeState() const = 0;    // one of Numbers.h's TimeState values
  virtual bool advertsHeld() const = 0;     // STATUS flag bit 3

  // TIME_SET (section 5.4). true: the clock moved, `prev_epoch` is what it said before and
  // timeState() now says what it became. false: refused, which the session answers kErrRange.
  // Whether a value is acceptable is the keeper's rule and not the session's: forward always,
  // backwards only with `force` or while nothing with a timestamp has left this node.
  virtual bool setTime(uint32_t epoch, bool force, uint32_t& prev_epoch) = 0;
};

// The firmware's own command interpreter (phase 1's task 05). Present => kCapCli.
struct CliAccess {
  virtual ~CliAccess() {}

  // Runs one command line as if it had been typed on the serial console (section 5.7) and writes
  // the reply into `reply` (NUL-terminated); returns the reply's length.
  //
  // `line` is NUL-terminated and is the session's own buffer, not the request's bytes, because
  // the interpreter is allowed to write into it: upstream's MyMesh::handleCommand terminates a
  // parameter in place (examples/simple_repeater/MyMesh.cpp:1234 at the pin 81dbae59,
  // `*sp++ = 0`). `reply_cap` is what upstream's own console loop gives its interpreter -
  // 160 bytes, src/roles/repeater/main.cpp - and the session keeps room behind it for the few of upstream's
  // commands that can write past it (src/common/link/session/Session.h).
  virtual size_t runCommand(char* line, char* reply, size_t reply_cap) = 0;
};

// Attention and the sleeping Walter: section 8, the machine in src/common/link/attention/
// (phase 1's task 05). Present => kCapAttention.
//
// The session drives it once a pass and puts on the wire whatever dueNow() asks for; everything
// about *when* - the 300 ms gap, the 1000 ms repeat, the ten sends, the hourly limit per reason,
// the queue - is the machine's, so that test/test_attention can hold it against a fake clock
// without a session and test/test_link_session can drive a session against a fake machine.
enum AttentionDue : uint8_t {
  kAttentionDueNothing = 0,
  kAttentionDueWake = 1,   // the two 0x00 bytes that wake a sleeping Walter
  kAttentionDueEvent = 2,  // EV_ATTENTION, as pendingEvent() spells it
};

struct AttentionAccess {
  virtual ~AttentionAccess() {}

  virtual uint8_t pendingReason() const = 0;  // STATUS's `attention`, or kAttentionNone
  virtual bool walterAsleep() const = 0;      // STATUS flag bit 2

  // What the session must put on the wire now, if anything.
  virtual uint8_t dueNow(uint32_t now_ms) const = 0;
  // The reason, detail and repeat of the event dueNow() just asked for.
  virtual Attention pendingEvent() const = 0;
  // The session has handed those bytes to the UART.
  virtual void wakeSent(uint32_t now_ms) = 0;
  virtual void eventSent(uint32_t now_ms) = 0;

  // A valid request arrived (section 8: "until any valid request arrives"). The repeats stop
  // here. The type is passed because SLEEP_NOTICE is the one request that does not mean the
  // Walter means to stay awake.
  virtual void noteRequest(uint8_t request_type, uint32_t now_ms) = 0;
  // That request's response has gone. The pending reason is cleared here and not a moment
  // earlier, so that the answer to the request which ends an attention still carries the reason
  // in STATUS - "a Walter that woke for its own reasons sees it too" (section 8).
  virtual void requestAnswered(uint32_t now_ms) = 0;
  // SLEEP_NOTICE itself: the Walter's last request before it sleeps.
  virtual void sleepNotice(uint32_t expect_back_s, uint32_t now_ms) = 0;
};

// The INA3221 (phase 1's task 06). Present => kCapPower.
struct PowerAccess {
  virtual ~PowerAccess() {}

  virtual bool readPower(PowerResponse& out) = 0;
};

// The BME280 (phase 1's task 06). Present => kCapEnv.
struct EnvAccess {
  virtual ~EnvAccess() {}

  virtual bool readEnv(EnvResponse& out) = 0;
};

// The Cayenne-LPP a mesh telemetry request would be answered with (phase 1's task 06). It carries
// no capability bit of its own: LINK-PROTOCOL.md section 4 gates LPP_GET on kCapCli.
struct LppAccess {
  virtual ~LppAccess() {}

  virtual size_t readLpp(uint8_t perm_mask, uint8_t* out, size_t cap) = 0;
};

// LOCAL_REQUEST (link 1.2, spec/LINK-PROTOCOL.md 5.9; pile 3.9's task 02). Present =>
// kCapLocalRequest. It is the capability of a role with a request handler of its own - the
// repeater's, through upstream's MyMesh::handleRequest
// (src/roles/repeater/link/LocalRequest.h). The scanner has none and does not advertise the bit.
struct LocalRequestAccess {
  virtual ~LocalRequestAccess() {}

  // Runs `req_type` with `data` through the node's own request handler, exactly as that handler
  // answers an admin over the mesh, and writes the answer **after its four tag bytes** into
  // `out` (section 5.9). Nothing goes on the air.
  //
  // Returns the number of bytes written, which may be 0 - an access list with nobody on it is a
  // legal, empty answer - or kLocalRequestNoAnswer when the handler answered nothing at all,
  // which the session answers kErrRange. Which `req_type` values reach here is the session's
  // business (section 5.9's table): an implementation is never asked for one outside it, and
  // 0x03 telemetry in particular is refused before this call, because LPP_GET is its local form.
  virtual int localRequest(uint8_t req_type, Bytes data, uint8_t* out, size_t cap) = 0;
};

// The mesh client (phase 2; src/roles/repeater/meshclient/LinkMeshClient.h implements it over the job of
// src/roles/repeater/meshclient/Job.h and the reply record of src/roles/repeater/meshclient/Reply.h). Present => kCapMeshClient.
//
// The four job requests of section 6.1 (task 01), the five sends of 6.2, the reply of 6.3 and the
// counters of 6.4 (task 02). Every method that answers a request returns one of Numbers.h's
// Status values, because which refusal a request gets - ERR_STATE with no job, ERR_BUSY while a
// reply is awaited, ERR_RANGE for what cannot be sent - is the client's rule and not the
// session's.
struct MeshClientAccess {
  virtual ~MeshClientAccess() {}

  virtual bool jobBegun() const = 0;      // STATUS flag bit 0
  virtual bool replyAwaited() const = 0;  // STATUS flag bit 1

  // JOB_BEGIN: ends any job in progress, then makes this key the peer and this path the job's.
  virtual uint8_t beginJob(const JobBegin& m, uint32_t now_ms) = 0;
  // JOB_END: forgets the peer, the path and any awaited reply, and wipes the shared secret.
  virtual void endJob() = 0;
  // PATH_SET: replaces the job's path (mode kPathNone forgets it). kErrState with no job.
  virtual uint8_t setPath(const Path& p) = 0;
  // PATH_GET: the path the job holds now, as a view into the client's own storage that stays
  // valid until the next request. kErrState with no job.
  virtual uint8_t getPath(Path& out) = 0;
  // A valid request arrived on the link, of any type, a retry included. "The RAK also ends a job
  // by itself when 120 seconds pass with no request on the link while a job is begun" (6.1):
  // this is what restarts that count.
  virtual void noteLinkRequest(uint32_t now_ms) = 0;

  // The five sends (6.2). Each puts exactly one packet on the air or none, arms the wait in the
  // same step when the request's timeout_ms is not 0, and fills `out` on kOk. The views in `m`
  // point into the link's receive buffer, which the session wipes after the call: a password is
  // in RAM for the length of the send and nowhere else.
  virtual uint8_t sendLogin(const SendLogin& m, SendResponse& out, uint32_t now_ms) = 0;
  virtual uint8_t sendRequest(const SendRequest& m, SendResponse& out, uint32_t now_ms) = 0;
  virtual uint8_t sendCli(const SendCli& m, SendResponse& out, uint32_t now_ms) = 0;
  virtual uint8_t sendDiscovery(const SendDiscovery& m, SendResponse& out, uint32_t now_ms) = 0;
  virtual uint8_t sendTrace(const SendTrace& m, SendResponse& out, uint32_t now_ms) = 0;

  // REPLY_GET (6.3): the record as it stands - pending, an outcome, or nothing held. The views in
  // `out` point into the client's own storage and stay valid until the next send or job request.
  virtual void getReply(ReplyRecord& out, uint32_t now_ms) = 0;
  // EV_MESH_REPLY (6.3): true, with `out` filled, exactly once per outcome - when an awaited reply
  // arrived or its wait timed out. The session asks once a pass when the wire is free.
  virtual bool takeReplyEvent(ReplyRecord& out, uint32_t now_ms) = 0;

  // MESH_COUNTERS (6.4): rx, tx, client_tx and the radio's floor, RSSI and SNR. The observation
  // fields (adverts, adverts_dropped, obs_dropped) are the ring's and the session fills them.
  virtual void getCounters(MeshCountersResponse& out) = 0;
};

// The observation ring (phase 2's task 03; src/roles/repeater/meshclient/LinkObservations.h implements it over
// the ring of src/roles/repeater/meshclient/ObservationRing.h). Present => kCapObservations.
struct ObservationAccess {
  virtual ~ObservationAccess() {}

  virtual uint8_t pending() const = 0;            // STATUS's obs_pending
  virtual uint16_t droppedSinceBoot() const = 0;  // STATUS's obs_dropped, saturating

  // MESH_COUNTERS' three observation counts (section 6.4), since boot. They are the ring's and
  // not the mesh client's, which is why the session reads them here and not through
  // MeshClientAccess::getCounters.
  virtual uint32_t adverts() const = 0;               // adverts that verified
  virtual uint32_t advertsDropped() const = 0;
  virtual uint32_t observationsDropped() const = 0;   // lost to a full ring

  // OBS_PULL (section 7): up to `max` records, oldest first, as many as fit in one payload.
  // Fills `remaining`, `dropped` and the records, whose views point into the implementation's own
  // storage and stay valid until the next pull - which is long enough, because the session
  // encodes the response inside this request's own handler.
  //
  // **The records handed out leave the ring.** A link-level retry of the same request (section 3)
  // never reaches here: the session answers it from its one-entry memory, which is what makes
  // "the same records, not the next ones" true without this function knowing anything about
  // retries.
  virtual void pull(uint8_t max, ObsPullResponse& out, uint32_t now_ms) = 0;
};

// Everything the session is given. The three that are not optional are checked by the session's
// constructor; every other null pointer is a capability this build does not have.
struct Services {
  const Firmware* firmware = nullptr;
  SystemAccess* system = nullptr;
  IdentityAccess* identity = nullptr;

  ClockAccess* clock = nullptr;
  CliAccess* cli = nullptr;
  AttentionAccess* attention = nullptr;
  PowerAccess* power = nullptr;
  EnvAccess* env = nullptr;
  LppAccess* lpp = nullptr;
  LocalRequestAccess* local_request = nullptr;
  MeshClientAccess* mesh = nullptr;
  ObservationAccess* observations = nullptr;
};

// HELLO's `caps`, built from the interfaces present and from nothing else (section 5.1).
inline uint32_t capsOf(const Services& s) {
  uint32_t caps = 0;
  if (s.cli != nullptr) caps |= kCapCli;
  if (s.power != nullptr) caps |= kCapPower;
  if (s.env != nullptr) caps |= kCapEnv;
  if (s.mesh != nullptr) caps |= kCapMeshClient;
  if (s.observations != nullptr) caps |= kCapObservations;
  if (s.attention != nullptr) caps |= kCapAttention;
  if (s.clock != nullptr) caps |= kCapTimePersist;
  if (s.local_request != nullptr) caps |= kCapLocalRequest;  // 1.2
  return caps;
}

}  // namespace link
}  // namespace twinoak
