#include "roles/repeater/link/BoardLink.h"

#include "common/board/target.h"
#include "common/fwdesc/ImageDescriptor.h"
#include "common/link/SerialTransport.h"
#include "common/link/attention/Attention.h"
#include "common/link/session/Interfaces.h"
#include "common/link/session/ResetReason.h"
#include "common/link/session/Session.h"
#include "common/power/BoardPower.h"
#include "common/version/Version.h"
#include "roles/repeater/app/IdentityGuard.h"  // kFaultCodeIdentityLost
#include "roles/repeater/clock/BoardClock.h"
#include "roles/repeater/clock/LinkClock.h"
#include "roles/repeater/link/LocalRequest.h"
#include "roles/repeater/meshclient/LinkObservations.h"

namespace twinoak {
namespace link {
namespace {

// --- the node ---------------------------------------------------------------------------------

class BoardSystem : public SystemAccess {
 public:
  void drawBootId() {
    // "random, drawn once per boot" (spec/LINK-PROTOCOL.md section 5.1). The radio's noise
    // listener is the same source the node's own key is drawn from (src/common/board/target.h); four
    // bytes once at boot is what it costs.
    uint8_t bytes[4] = {0, 0, 0, 0};
    radio_random(bytes, sizeof(bytes));
    boot_id_ = uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) |
               (uint32_t(bytes[3]) << 24);
  }

  uint32_t uptimeSeconds() const override { return millis() / 1000u; }

  uint8_t resetReason() const override {
    // MeshCore captures POWER->RESETREAS in a constructor of priority 101, before anything
    // clears it, and RAK4631Board::begin() -> checkBootVoltage() -> initPowerMgr() copies it into
    // the board object (src/helpers/NRF52Board.cpp:45 and :111, variants/rak4631/
    // RAK4631Board.cpp:60-63 at the pin 81dbae59). NRF52_POWER_MANAGEMENT is defined for this
    // environment, which is what makes getResetReason() the register and not zero - and it is
    // defined on purpose: upstream dropped it from its own rak4631 block at repeater-v1.18.0 (the
    // undervoltage boot lock behind it breaks other cell chemistries), but on this board the
    // reading that lock checks is 0 at boot (tasks/phase-1/reports/06.md), so the lock never
    // fires here and the define's one effect is this capture. platformio.ini says the same
    // beside the define.
    return resetReasonFromNrf52(board.getResetReason());
  }

  uint32_t bootId() const override { return boot_id_; }

  // src/roles/repeater/main.cpp halts when radio_init() fails, so the link only ever runs with the radio up.
  bool radioUp() const override { return true; }

  bool canRestart(uint8_t mode) const override {
    // All four modes of section 5.5 are reachable on this board. Modes 1 to 3 are the Adafruit
    // nRF52 bootloader's, entered by the magic word the core writes into GPREGRET before the
    // reset (cores/nRF5/wiring.c at the pinned core commit d541301: DFU_MAGIC_UF2_RESET 0x57,
    // DFU_MAGIC_SERIAL_ONLY_RESET 0x4e, DFU_MAGIC_OTA_RESET 0xA8); the RAK4630 ships with that
    // bootloader, which is also what the field USB port flashes through.
    return mode <= kRebootOtaDfu;
  }

  void restartNow(uint8_t mode) override {
    // The clock's lower bound, before anything else: this is a restart the firmware asked for,
    // so the file may keep the second it happens in (src/roles/repeater/clock/ClockKeeper.h). board.reboot()
    // below would do it too - TwinOakBoard overrides it - but modes 1 to 3 do not go through the
    // board at all, so it is done here for all four.
    twinoak::clock::saveBeforeRestart();
    // The response was handed to the UART a delay ago, but the DMA may still be running: flush
    // both consoles before the core pulls the interrupts down (section 5.5, "which leaves time
    // for the response to leave the UART").
    Serial1.flush();
    Serial.flush();
    switch (mode) {
      case kRebootUf2:
        enterUf2Dfu();
        break;
      case kRebootSerialDfu:
        enterSerialDfu();
        break;
      case kRebootOtaDfu:
        enterOTADfu();
        break;
      case kRebootApplication:
      default:
        board.reboot();  // NVIC_SystemReset (NRF52Board::reboot)
        break;
    }
  }

 private:
  uint32_t boot_id_ = 0;
};

// --- the identity -----------------------------------------------------------------------------

class BoardIdentity : public IdentityAccess {
 public:
  void attach(MyMesh* mesh) { mesh_ = mesh; }

  const uint8_t* publicKey() const override {
    return mesh_ == nullptr ? nullptr : mesh_->self_id.pub_key;
  }
  const char* nodeName() const override {
    return mesh_ == nullptr ? "" : mesh_->getNodeName();
  }

 private:
  MyMesh* mesh_ = nullptr;
};

// --- the console ------------------------------------------------------------------------------

// CLI (section 5.7): the line run by **upstream's own** interpreter, with a sender timestamp of
// 0, which is what makes it a command typed on the serial console rather than one that arrived
// over the mesh. Upstream's MyMesh::handleCommand(0, command, reply) is public and is the same
// call src/roles/repeater/main.cpp's USB console makes (examples/simple_repeater/MyMesh.h:229 and
// src/helpers/CommonCLI.cpp:183 at the pin 81dbae59); nothing of upstream's is touched to put it
// on this link.
//
// Three things about that call, each read out of the source at the pin:
//
//   - `command` is a char* the interpreter writes into (`*sp++ = 0` in the `setperm` branch), so
//     the session hands it a copy of the line and not the frame's bytes;
//   - the reply buffer is the 160 bytes upstream's own console gives it, and several of its
//     branches assume exactly that number (`snprintf(reply, 160, ...)`,
//     `_region_map->exportTo(reply, 160)`, `StrHelper::strncpy(&reply[14], ..., 160-15)`), which
//     is why the session's window is 160 and not 161;
//   - four lines never come back at all - `poweroff`, `shutdown`, `reboot` and `clkreboot` call
//     the board's powerOff()/reboot() (CommonCLI.cpp:194-201) - so the response to those is not
//     owed and this handler does not try to out-run the restart. Our board writes the clock's
//     file on the way down (src/common/board/target.cpp), as it does for the link's own REBOOT.
class BoardCli : public CliAccess {
 public:
  void attach(MyMesh* mesh) { mesh_ = mesh; }

  size_t runCommand(char* line, char* reply, size_t reply_cap) override {
    if (mesh_ == nullptr || reply_cap == 0) return 0;
    reply[0] = 0;
    mesh_->handleCommand(0, line, reply);  // 0: "there is no sender_timestamp via serial"
    size_t n = 0;
    while (n < reply_cap && reply[n] != 0) n++;
    return n;
  }

 private:
  MyMesh* mesh_ = nullptr;
};

// --- the one of each --------------------------------------------------------------------------

BoardSystem g_system;
BoardIdentity g_identity;
BoardCli g_cli;
AttentionMachine g_attention;

// LOCAL_REQUEST (link 1.2, spec/LINK-PROTOCOL.md 5.9): the node's status, access list,
// neighbours or owner info, answered by upstream's own request handler with nothing on the air
// (src/roles/repeater/link/LocalRequest.h). It carries its own buffers, so it is a global for the
// same reason the ring below is.
BoardLocalRequest g_local_request;

// The clock keeper as ClockAccess: twenty lines in src/roles/repeater/clock/LinkClock.h, and what puts
// TIME_PERSIST in HELLO's caps. It binds a reference to the keeper of another translation unit,
// whose constructor may not have run yet - which is safe because that is all it does with it;
// nothing here calls the keeper before main().
clock::LinkClock g_clock(clock::keeper());

// The observation ring and its link face (src/roles/repeater/meshclient/): every advert this node hears, kept
// for the Walter to collect. The ring is the largest single thing this firmware keeps in RAM -
// 64 records of section 7 - and it is a global, in .bss, for exactly that reason.
meshclient::ObservationRing g_ring;
meshclient::LinkObservations g_observations(g_ring);

// What this image is, filled in by linkBegin(). Field by field and not by braces: a C++11 struct
// with default member initialisers is not an aggregate, and the board builds as gnu++11
// (tasks/phase-1/reports/02.md).
Firmware g_firmware;

Services g_services;
Session g_session(g_services);
SerialTransport g_transport(g_session);

// The mesh client's link half lives with its job inside the TwinOakMesh (src/roles/repeater/app/TwinOakMesh.h);
// this is the one pointer linkLoop() needs for the job's silence rule.
meshclient::LinkMeshClient* g_mesh_client = nullptr;

}  // namespace

namespace {

// What this image is, read out of the image's own descriptor (spec/FIRMWARE-MANIFEST.md
// section 4) and not from the defines a second time: the bytes the Walter will check against
// the manifest are the bytes HELLO answers from, so the two cannot drift. Reading them here is
// also the reference that keeps the linker from discarding the descriptor.
//
// `slot` is the only field that is not the descriptor's: the descriptor says which radio
// positions this image may go in (0xFF, any, on a single-RAK board) and HELLO says which
// position this unit *is* (src/common/board/Boards.h, spec/LINK-PROTOCOL.md 5.1).
void fillFirmware() {
  g_firmware.role = fwdesc::imageRole();
  g_firmware.slot = fwdesc::imageSlot();
  g_firmware.version = fwdesc::imageVersion();  // "0.0.0-dev" unless the release says otherwise
  g_firmware.board = fwdesc::imageBoard();      // "rak4630-single"
  g_firmware.band = fwdesc::imageBand();        // "868", HELLO's `band` in link 1.1
  g_firmware.upstream = upstreamString();       // "<tag>+<pin8>"
}

}  // namespace

void linkBegin(TwinOakMesh& mesh) {
  g_system.drawBootId();
  g_identity.attach(&mesh);
  g_cli.attach(&mesh);
  g_mesh_client = &mesh.meshClient();

  fillFirmware();

  g_services.firmware = &g_firmware;
  g_services.system = &g_system;
  g_services.identity = &g_identity;
  g_services.clock = &g_clock;          // => kCapTimePersist in HELLO's caps
  g_services.cli = &g_cli;              // => kCapCli
  g_services.attention = &g_attention;  // => kCapAttention
  // One object is all three of these (src/common/power/LinkPower.h): POWER_GET, ENV_GET and LPP_GET all
  // answer out of the readers' cache, so none of them touches the I2C bus. LPP_GET has no
  // capability bit of its own - section 4 gates it on CLI - which is why the third line adds
  // nothing to `caps`.
  g_services.power = &power::linkPower();  // => kCapPower
  g_services.env = &power::linkPower();    // => kCapEnv
  g_services.lpp = &power::linkPower();
  // LOCAL_REQUEST (section 5.9, link 1.2): the repeater has a request handler of its own -
  // upstream's - so it is the one role that advertises this bit. The scanner speaks no link at
  // all, and the lost mode below has no mesh to ask.
  g_local_request.attach(&mesh);
  g_services.local_request = &g_local_request;  // => kCapLocalRequest
  // The job of 6.1, the five sends of 6.2, the reply of 6.3, the counters of 6.4 and STATUS's two
  // job bits (spec/LINK-PROTOCOL.md section 6; phase 2's task 04 is the backstop over them).
  g_services.mesh = g_mesh_client;  // => kCapMeshClient
  // OBS_PULL, STATUS's obs_pending and obs_dropped, and MESH_COUNTERS' three advert counts
  // (spec/LINK-PROTOCOL.md section 7). The ring is filled from TwinOakMesh::onAdvertRecv through
  // noteAdvert() below, not from here.
  g_services.observations = &g_observations;  // => kCapObservations; caps is now 0xff
  // Every other interface stays null until the task that implements it fills it in, and its
  // capability bit is then absent from HELLO by itself (session/Interfaces.h, capsOf).

  g_transport.begin();  // Serial1 up, and the session's EV_BOOT queued

  // Section 8's reason 1 - "the node restarted" - is owed only to a Walter that was asleep when
  // it happened: an awake one learns everything this event carries from EV_BOOT, which has just
  // been queued. Whether it was asleep is a fact that would have to survive a reset, and on this
  // board nothing does (tasks/phase-1/reports/04.md, "Where the surviving RAM would live"), so
  // the answer here is false and the call is the seam a board that can answer it would fill in.
  g_attention.noteBoot(g_system.resetReason(), /*walter_was_asleep=*/false, millis());
}

void linkBeginIdentityLost(CliAccess& cli) {
  g_system.drawBootId();
  fillFirmware();

  // The four that are always there, less the identity, which this boot has not got; the lost
  // mode's handler behind CLI; the attention machine, so that the fault reaches a sleeping
  // Walter. Nothing else: no clock (nothing may write the file system this boot), no telemetry,
  // no mesh client, no observations - and their bits are absent from HELLO's `caps` because the
  // pointers are null (session/Interfaces.h, capsOf), so a Walter asking for them is answered
  // ERR_UNSUPPORTED and not with something made up.
  g_services.firmware = &g_firmware;
  g_services.system = &g_system;
  g_services.identity = nullptr;  // IDENTITY => ERR_STATE (Session.cpp)
  g_services.cli = &cli;          // => kCapCli
  g_services.attention = &g_attention;  // => kCapAttention

  g_transport.begin();  // Serial1 up, and the session's EV_BOOT queued

  g_attention.noteBoot(g_system.resetReason(), /*walter_was_asleep=*/false, millis());
  // Section 8's reason 6, "a fault the node wants reported", with this firmware's code for the
  // condition (docs/link.md's attention table). Once: the machine's hourly limit per reason makes
  // the second raise a no-op anyway, and a boot that stays in this mode has nothing new to say.
  g_attention.raise(kAttentionFault, identity::kFaultCodeIdentityLost, millis());
}

void linkLoop() {
  g_transport.loop();
  // "The RAK also ends a job by itself when 120 seconds pass with no request on the link while a
  // job is begun" (spec/LINK-PROTOCOL.md 6.1): the rule is the job's (src/roles/repeater/meshclient/Job.cpp),
  // the session restarts its count on every valid request, and this is the once-a-pass look at
  // the clock that ends it.
  if (g_mesh_client != nullptr) g_mesh_client->loop(millis());
}

bool raiseAttention(uint8_t reason, uint16_t detail) {
  return g_attention.raise(reason, detail, millis());
}

bool raiseWalterWake() { return raiseAttention(kAttentionPersonAsked, 0); }

void noteAdvert(const meshclient::HeardAdvert& heard) {
  // The threshold, the `obs.wake` setting and "once per filling of the ring" are the ring face's
  // (src/roles/repeater/meshclient/LinkObservations.cpp); the hourly limit and the whole of section 8's sequence
  // are the machine's. This is the one line that joins them, and it is here because this file is
  // where both objects live.
  if (!g_observations.noteAdvert(heard, g_attention.walterAsleep())) return;
  raiseAttention(kAttentionRingNearlyFull, g_observations.wakeWaiting());
}

bool obsWakeEnabled() { return g_observations.wakeEnabled(); }
void setObsWakeEnabled(bool on) { g_observations.setWakeEnabled(on); }

bool attentionPending() { return g_attention.pendingReason() != kAttentionNone; }

bool walterAsleep() { return g_attention.walterAsleep(); }

uint32_t clientRefused() {
  return g_mesh_client == nullptr ? 0 : g_mesh_client->refused();
}

uint16_t linkErrors() { return g_session.linkErrors(); }
bool walterEverSeen() { return g_session.everSawRequest(); }
uint32_t walterSeenSecondsAgo() { return g_session.secondsSinceRequest(millis()); }
uint32_t linkWorstPassMicros() { return g_transport.worstPassMicros(); }

}  // namespace link
}  // namespace twinoak
