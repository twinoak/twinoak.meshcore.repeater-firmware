// The decision a boot takes about the node's identity, and the mode the node runs in when it
// refuses to invent one (pile 3.9, task 01: the first radio's key was lost on 6 November 2026).
//
// What happened, read from the code: the Adafruit nRF52 core's InternalFileSystem::begin()
// (libraries/InternalFileSytem/src/InternalFileSystem.cpp at the pinned core d541301) answers a
// LittleFS volume it cannot mount by **erasing every page of the region and formatting it**, and
// then upstream's setup() - ours is a copy of it - finds no `/_main.id`, draws a new key and saves
// it. Nothing printed, nothing refused: a node that was known to a whole mesh and to MeshMaster
// is a stranger on its next advert. The volume can stop mounting because the core keeps LittleFS
// in 128-byte blocks on 4096-byte pages and rewrites a page by erasing it and programming it
// again (flash_cache.c, flash_cache_flush), so a reset, a halt or a power dip inside that window
// takes 32 blocks at once - the superblock pair and the root directory among them, which live in
// the first page. tasks/phase-3.9/reports/01.md is the whole reading.
//
// The rule this file holds, in one sentence: **a key is made only where no node was.** A region
// that is blank (every byte 0xFF - the base was just written, or a chip was erased) is a first
// boot; a mounted volume with neither a key file nor settings is a first boot (an `erase`, or a
// format by hand); everything else that has no readable key is a node whose key is gone, and this
// firmware says so and waits for a person rather than becoming another node by itself
// (spec/SYSTEM.md S-1: one node, one identity).
//
// Host-portable: no Arduino, no MeshCore, no file system. The board's main.cpp reads the region
// and the file and hands the facts in; test/test_identity holds the table and the lost mode's
// console against fakes.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "common/link/session/Interfaces.h"  // link::CliAccess

namespace twinoak {
namespace identity {

// Where the core keeps the internal file system on an nRF52840: LFS_FLASH_ADDR and
// LFS_FLASH_TOTAL_SIZE of InternalFileSystem.cpp at the pinned core d541301 - 0xED000, seven
// 4096-byte pages, 0xED000..0xF4000 (docs/ARCHITECTURE.md section 9 draws the same map). The
// core does not export the two defines; these are their values, cited.
constexpr uint32_t kRegionStart = 0xED000u;
constexpr uint32_t kRegionBytes = 7u * 4096u;
constexpr uint32_t kPageBytes = 4096u;

// What the board found when it tried to mount the region, before anything was written to it.
enum class Volume : uint8_t {
  kMounted,       // lfs_mount succeeded
  kBlank,         // it did not, and every byte of the region is 0xFF: never formatted
  kUnmountable,   // it did not, and the region is not blank: a volume that was and is torn
  kFormatFailed,  // the region was blank and could not be formatted and mounted: not a key matter
};

// What the board found of the key file once the volume was mounted. Upstream keeps it as
// "/_main.id" (IdentityStore(InternalFS, "") in examples/simple_repeater/main.cpp at the pin
// 81dbae59; IdentityStore::load builds "%s/%s.id"), 64 bytes: the public key and the private key.
enum class KeyFile : uint8_t {
  kLoaded,      // the file exists and read as a key
  kAbsent,      // no such file
  kUnreadable,  // the file exists and did not read as a key
};

enum class Decision : uint8_t {
  kUseSaved,  // the key loaded: this is the node it was
  kMakeNew,   // nothing of a node is there: a first boot - make a key and save it
  kRefuse,    // a node was here and its key is not: do not become another one
};

struct Verdict {
  Decision decision = Decision::kRefuse;
  const char* why = "";  // one clause for the console, never null
};

// Every byte 0xFF. `bytes` is the region as the chip maps it; on the board that is a pointer to
// flash and the read costs nothing.
bool regionIsBlank(const uint8_t* bytes, size_t len);

// The table. `key` and `prefs_present` matter only when the volume mounted.
Verdict decide(Volume volume, KeyFile key, bool prefs_present);

// --- the mode the node runs in when it refuses ------------------------------------------------
//
// No key, no mesh, no file system touched. The USB console and the box link stay up so that a
// person sees why - at the port, through the Walter's `rak cli`, or on meshmaster.dk through the
// Walter's heartbeat, which gets no key from IDENTITY (spec/LINK-PROTOCOL.md 5.6 answered
// ERR_STATE) and an EV_ATTENTION of reason 6 with the code below. Two lines are understood, and
// they are upstream's own words so that nothing new has to be learned: `erase` formats the region
// (the core's erase-and-format, now on a person's command and not the firmware's), and `reboot`
// restarts into a first boot. Everything else is answered with the explanation.

// EV_ATTENTION reason 6's `detail` ("a code of the firmware's own", spec/LINK-PROTOCOL.md
// section 8) for this condition. docs/link.md's attention table lists it.
constexpr uint16_t kFaultCodeIdentityLost = 1;

struct LostIdentityActions {
  virtual ~LostIdentityActions() {}
  // Erase every page of the region, format it and mount it - the core's InternalFS.begin() on an
  // unmountable volume. True when it mounted afterwards.
  virtual bool eraseFileSystem() = 0;
  // Restart the application. Never returns on the board; a fake returns.
  virtual void restart() = 0;
};

// The explanation, as the console prints it and as every unknown line is answered. Returns the
// length written (NUL-terminated, truncated to `cap`).
size_t explainLost(const Verdict& verdict, bool erased, char* out, size_t cap);

class LostIdentityCli : public link::CliAccess {
 public:
  // `verdict` is read at every call and not copied, so that a global handler can be built before
  // setup() has decided anything (src/roles/repeater/main.cpp); it must outlive this object.
  LostIdentityCli(LostIdentityActions& actions, const Verdict& verdict)
      : actions_(actions), verdict_(&verdict) {}

  // `erase`, `reboot`, or the explanation. The reply is NUL-terminated and never longer than
  // `reply_cap` - 1; the return is its length.
  size_t runCommand(char* line, char* reply, size_t reply_cap) override;

  // Whether `erase` has been run and succeeded in this boot: the explanation then says that a
  // `reboot` is all that is left.
  bool erased() const { return erased_; }

 private:
  LostIdentityActions& actions_;
  const Verdict* verdict_;
  bool erased_ = false;
};

}  // namespace identity
}  // namespace twinoak
