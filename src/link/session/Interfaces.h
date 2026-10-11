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
// 04 (src/clock/LinkClock.h is what implements it). The rest are declared here with the methods
// STATUS and `caps` need of them today; the task that implements one adds the methods its own
// messages need (05 the console and attention, 06 power and environment, phase 2 the mesh client
// and the observation ring).

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../codec/Messages.h"
#include "../codec/Numbers.h"

namespace twinoak {
namespace link {

// What this image is: the three strings and the role HELLO reports, which are the same strings the
// image descriptor carries (spec/FIRMWARE-MANIFEST.md section 4). Given to the session once; the
// strings must live as long as it does.
struct Firmware {
  uint8_t role = kRoleUnknown;
  uint8_t slot = 0;             // 0 on a single-radio unit
  const char* version = "";     // HELLO's fw_version, e.g. "0.3.0+abc1234"
  const char* board = "";       // e.g. "rak4630-single"
  const char* upstream = "";    // what it is built on, e.g. "repeater-v1.17.1+d9296435"
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

// The clock keeper (phase 1's task 04, src/clock/). Present => kCapTimePersist.
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
  virtual size_t runCommand(const char* line, char* reply, size_t reply_cap) = 0;
};

// Attention and the sleeping Walter (phase 1's task 05). Present => kCapAttention.
struct AttentionAccess {
  virtual ~AttentionAccess() {}

  virtual uint8_t pendingReason() const = 0;  // STATUS's `attention`, or kAttentionNone
  virtual bool walterAsleep() const = 0;      // STATUS flag bit 2
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

// The mesh client (phase 2). Present => kCapMeshClient.
struct MeshClientAccess {
  virtual ~MeshClientAccess() {}

  virtual bool jobBegun() const = 0;      // STATUS flag bit 0
  virtual bool replyAwaited() const = 0;  // STATUS flag bit 1
};

// The observation ring (phase 2). Present => kCapObservations.
struct ObservationAccess {
  virtual ~ObservationAccess() {}

  virtual uint8_t pending() const = 0;           // STATUS's obs_pending
  virtual uint16_t droppedSinceBoot() const = 0;  // STATUS's obs_dropped, saturating
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
  return caps;
}

}  // namespace link
}  // namespace twinoak
