// The numbers of the box link, protocol 1.0: message types, status codes, roles, capability
// bits, modes and the few limits the protocol fixes. One header that reads like the tables of
// spec/LINK-PROTOCOL.md sections 2 to 8, so that no other file of the codec carries a magic
// number and a reviewer can hold this file against that document line by line.
//
// Host-portable: no Arduino, no MeshCore (docs/ARCHITECTURE.md section 4, layer 1). The wire
// values are the specification's and are never renumbered here; a new minor version of the
// protocol adds names at the end of a table and changes none of these (LINK-PROTOCOL.md 5.1,
// "Versions").

#pragma once

#include <stdint.h>

namespace twinoak {
namespace link {

// ----------------------------------------------------------------------------- what fits where

// LINK-PROTOCOL.md section 2, "Frames".
constexpr uint8_t kProtoMajor = 1;
constexpr uint8_t kProtoMinor = 0;
constexpr uint16_t kMaxPayload = 240;              // bytes of payload in one packet
constexpr uint16_t kMinPacket = 4;                 // type + seq + crc16, with no payload
constexpr uint16_t kMaxPacket = kMaxPayload + 4;   // type + seq + payload + crc16: 244
// COBS adds exactly one byte to a packet this short (section 2), and the frame ends with 0x00.
constexpr uint16_t kMaxEncoded = kMaxPacket + 1;   // 245
constexpr uint16_t kMaxFrame = kMaxEncoded + 1;    // 246, the terminator included
// "A receive buffer needs 246 bytes": the longest encoding plus the room to see one byte too many.
constexpr uint16_t kReceiveBuffer = 246;
// A frame may be led by one 0x00 when the line has been idle (section 2), so a writer's buffer is
// one byte longer than the longest frame.
constexpr uint16_t kMaxWrittenFrame = kMaxFrame + 1;  // 247

constexpr uint8_t kPubKeyLen = 32;        // a MeshCore public key
constexpr int16_t kRssiUnknown = 0x7FFF;  // the RSSI a reply or an observation does not know

// ----------------------------------------------------------------------------- section 4, types

// Requests are 0x01..0x5F (Walter to RAK), responses the request's type with bit 7 set, events
// 0x60..0x7F (RAK to Walter). The reserved ranges (provisioning 0x10..0x1F, the bridge role
// 0x30..0x3F) have no names yet, on purpose.
enum MsgType : uint8_t {
  kMsgHello = 0x01,
  kMsgPing = 0x02,
  kMsgStatus = 0x03,
  kMsgTimeSet = 0x04,
  kMsgTimeGet = 0x05,
  kMsgReboot = 0x06,
  kMsgIdentity = 0x07,
  kMsgSleepNotice = 0x08,
  kMsgCli = 0x09,
  kMsgPowerGet = 0x0A,
  kMsgEnvGet = 0x0B,
  kMsgLppGet = 0x0C,
  kMsgJobBegin = 0x20,
  kMsgJobEnd = 0x21,
  kMsgPathSet = 0x22,
  kMsgPathGet = 0x23,
  kMsgSendLogin = 0x24,
  kMsgSendRequest = 0x25,
  kMsgSendCli = 0x26,
  kMsgSendDiscovery = 0x27,
  kMsgSendTrace = 0x28,
  kMsgReplyGet = 0x29,
  kMsgObsPull = 0x2A,
  kMsgMeshCounters = 0x2B,
  kMsgEvBoot = 0x60,
  kMsgEvMeshReply = 0x61,
  kMsgEvAttention = 0x62,
};

constexpr uint8_t kResponseBit = 0x80;
constexpr uint8_t kRequestFirst = 0x01;
constexpr uint8_t kRequestLast = 0x5F;
constexpr uint8_t kEventFirst = 0x60;
constexpr uint8_t kEventLast = 0x7F;

inline bool isRequestType(uint8_t type) { return type >= kRequestFirst && type <= kRequestLast; }
inline bool isEventType(uint8_t type) { return type >= kEventFirst && type <= kEventLast; }
inline uint8_t responseType(uint8_t request_type) { return uint8_t(request_type | kResponseBit); }

// ----------------------------------------------------------------------------- section 3, status

// The first byte of every response. When it is not kOk the payload is that one byte and nothing
// else.
enum Status : uint8_t {
  kOk = 0,
  kErrUnsupported = 1,  // this firmware does not know the type, or lacks the capability
  kErrMalformed = 2,    // the payload is too short or a field is not a legal value
  kErrBusy = 3,         // a mesh exchange is still waiting for its reply
  kErrState = 4,        // not possible now (no job begun, radio not up, ...)
  kErrRange = 5,        // a value is outside what is accepted
  kErrFailed = 6,       // tried and failed (no packet buffer, I2C error, ...)
  kErrDenied = 7,       // this role refuses it
};

// ----------------------------------------------------------------------------- section 5.1

// HELLO's peer_kind: who is asking.
enum PeerKind : uint8_t {
  kPeerWalter = 1,
  kPeerBenchTool = 2,
};

// HELLO's role.
enum Role : uint8_t {
  kRoleUnknown = 0,
  kRoleRepeater = 1,
  kRoleBridge = 2,
  kRoleScanner = 3,
};

// HELLO's caps. Unknown bits are ignored by both ends.
enum Capability : uint32_t {
  kCapCli = 0x01,
  kCapPower = 0x02,
  kCapEnv = 0x04,
  kCapMeshClient = 0x08,
  kCapObservations = 0x10,
  kCapAttention = 0x20,
  kCapTimePersist = 0x40,
};

// ----------------------------------------------------------------------------- section 5.3

// STATUS's reset_reason, also the first byte of EV_BOOT and the detail of attention reason 1.
enum ResetReason : uint8_t {
  kResetUnknown = 0,
  kResetPowerOn = 1,
  kResetPin = 2,
  kResetWatchdog = 3,
  kResetSoftware = 4,
  kResetLockup = 5,
  kResetSystemOff = 6,  // woke from system-off
  kResetBootloader = 7,  // left the bootloader
};

// STATUS's flags.
enum StatusFlag : uint16_t {
  kFlagJobBegun = 0x0001,
  kFlagReplyAwaited = 0x0002,
  kFlagWalterAsleep = 0x0004,
  kFlagAdvertsHeld = 0x0008,
  kFlagRadioUp = 0x0010,
};

// ----------------------------------------------------------------------------- section 5.4

// STATUS's and TIME_GET's time_state.
enum TimeState : uint8_t {
  kTimeUnset = 0,     // the firmware's built-in default; never right
  kTimeRestored = 1,  // carried over from before the reset
  kTimeSet = 2,       // set over this link since boot
  kTimeRtc = 3,       // read from a hardware RTC
};

// TIME_SET's source.
enum TimeSource : uint8_t {
  kTimeFromCellular = 1,
  kTimeFromServer = 2,
  kTimeFromPerson = 3,
};

// TIME_SET's flags.
enum TimeSetFlag : uint8_t {
  kTimeForce = 0x01,
};

// ----------------------------------------------------------------------------- section 5.5

// REBOOT's mode. A mode the board cannot do is answered kErrRange, so the codec passes every
// value up and the session decides (section 5.5).
enum RebootMode : uint8_t {
  kRebootApplication = 0,
  kRebootUf2 = 1,
  kRebootSerialDfu = 2,
  kRebootOtaDfu = 3,
};

// ----------------------------------------------------------------------------- sections 5.7, 5.8

constexpr uint8_t kCliLineMax = 160;   // CLI request: one command line, 1 to 160 bytes
constexpr uint8_t kCliReplyMax = 160;  // CLI response: the reply text, 0 to 160 bytes

// POWER_GET's valid bits, one per INA3221 channel, and what each channel is on the B-board.
enum PowerValid : uint8_t {
  kPowerChannel1 = 0x01,  // panel: positive = into the charger
  kPowerChannel2 = 0x02,  // battery: positive = charging, negative = discharging
  kPowerChannel3 = 0x04,  // total load: positive = drawn
};
constexpr uint8_t kPowerChannels = 3;

// ENV_GET's valid bits.
enum EnvValid : uint8_t {
  kEnvTemperature = 0x01,
  kEnvHumidity = 0x02,
  kEnvPressure = 0x04,
};

// LPP_GET's perm_mask: MeshCore's telemetry permission bits. Unknown bits are ignored.
enum LppPerm : uint8_t {
  kLppBase = 0x01,
  kLppLocation = 0x02,
  kLppEnvironment = 0x04,
};

// ----------------------------------------------------------------------------- section 6.1

// A path's path_mode.
enum PathMode : uint8_t {
  kPathNone = 0,    // the next send floods
  kPathDirect = 1,  // zero hops, sent direct with an empty path
  kPathHops = 2,    // hash_size x hop_count bytes of hops follow
};

constexpr uint8_t kPathHashSizeMin = 1;
constexpr uint8_t kPathHashSizeMax = 3;
constexpr uint8_t kPathHopCountMin = 1;
constexpr uint8_t kPathHopCountMax = 63;
constexpr uint8_t kPathHashesMax = 64;  // bytes of hops, at most

// ----------------------------------------------------------------------------- section 6.2

// The `send` field of every send response.
enum SendResult : uint8_t {
  kSendNothing = 0,
  kSendFlood = 1,
  kSendDirect = 2,
};

constexpr uint8_t kLoginPasswordMax = 15;  // SEND_LOGIN's password: 0 to 15 bytes
constexpr uint8_t kSendCliTextMax = 148;   // SEND_CLI's text: 1 to 148 bytes

// SEND_CLI's flags.
enum SendCliFlag : uint8_t {
  kSendCliSensitive = 0x01,  // the line holds a password: wipe it once the packet is built
};

// ----------------------------------------------------------------------------- section 6.3

// A reply record's state.
enum ReplyState : uint8_t {
  kReplyNone = 0,
  kReplyPending = 1,
  kReplyTimeout = 2,
  kReplyResponse = 3,
  kReplyText = 4,
  kReplyTrace = 5,
};

// ----------------------------------------------------------------------------- section 7

constexpr uint8_t kObsAppDataMax = 32;  // an observation's app_data: 0 to 32 bytes

// An observation's path_len is MeshCore's encoding: bits 7-6 the hash size minus one, bits 5-0
// the hop count.
inline uint8_t obsHashSize(uint8_t path_len) { return uint8_t((path_len >> 6) + 1); }
inline uint8_t obsHopCount(uint8_t path_len) { return uint8_t(path_len & 0x3F); }
inline uint8_t obsPathBytes(uint8_t path_len) {
  return uint8_t(obsHashSize(path_len) * obsHopCount(path_len));
}

// ----------------------------------------------------------------------------- section 8

// EV_ATTENTION's reason.
enum AttentionReason : uint8_t {
  kAttentionNone = 0,
  kAttentionRestarted = 1,      // detail: the reset reason
  kAttentionBatteryLow = 2,     // detail: battery millivolts
  kAttentionBatteryCritical = 3,  // detail: battery millivolts
  kAttentionPersonAsked = 4,    // detail: 0
  kAttentionRingNearlyFull = 5,  // detail: records waiting
  kAttentionFault = 6,          // detail: a code of the firmware's own
};

}  // namespace link
}  // namespace twinoak
