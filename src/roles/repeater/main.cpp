// Ours. setup() and loop() are MeshCore's examples/simple_repeater/main.cpp at the pin d9296435
// (repeater-v1.17.1), with:
//   - `the_mesh` a TwinOakMesh (src/roles/repeater/app/TwinOakMesh.h) where upstream constructs a MyMesh;
//   - the display (DISPLAY_CLASS, UITask), Ethernet (ETHERNET_ENABLED) and the SenseCAP button
//     left out - this adapter has none of them;
//   - the ESP32 / RP2040 / STM32 file-system branches left out - this is an nRF52 and nothing
//     else (the external-watchdog hook stays, as upstream has it, for a board that defines it);
//   - the debug-only delay and the debug print lines left out - nothing the mesh says is logged
//     as text (spec/SYSTEM.md S-12);
//   - the first boot after the_mesh.begin(): the random admin password and the first-boot
//     profile's lines, in one hold of the preference saves (src/roles/repeater/app/FirstBoot.h,
//     src/roles/repeater/firstboot/Profile.h, S-13 and S-12);
//   - the console's reply buffer 64 bytes longer than the 160 upstream gives its interpreter, and
//     a file-scope static like the command buffer beside it - the two comments where they are
//     declared say why;
//   - `len` in the console loop a size_t, so that -Werror accepts the comparison with
//     sizeof(command) that upstream writes with an int;
//   - the box link on Serial1, one call in each (src/roles/repeater/link/BoardLink.h, docs/ARCHITECTURE.md 4);
//   - the clock keeper, one call in each, before the mesh's own, and one rtc_clock.tick() in
//     setup() ahead of it (src/roles/repeater/clock/BoardClock.h, and the comment where it is called);
//   - the boot advert moved out of setup() and into loop(), where it waits for a clock
//     (docs/ARCHITECTURE.md 5; upstream sends it in setup() with a 16-second delay).
// The USB serial console - the read loop, the 160-byte command line, handleCommand(0, ...), the
// "  -> " echo - is upstream's, with the reply's guard above the only change. A pin bump re-reads
// upstream's main.cpp against this file (docs/upstream.md).

#include <string.h>

// Upstream's headers come in through these two and through nothing else here
// (src/common/board/UpstreamHeaders.h says why).
#include "roles/repeater/app/FirstBoot.h"
#include "roles/repeater/app/TwinOakMesh.h"
#include "roles/repeater/clock/BoardClock.h"
#include "roles/repeater/link/BoardLink.h"

StdRNG fast_rng;
SimpleMeshTables tables;

TwinOakMesh the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);

void halt() {
  while (1) ;
}

// Upstream's console loop keeps these two as `static char command[160]` and a `char reply[160]`
// local to loop(). Two changes, both deliberate:
//
//   - the reply is **160 bytes and 64 behind them**. Upstream's own `sensor list` can write past
//     the 160 it is given - it adds a line while it has written fewer than 134 bytes and then
//     appends `... next:<i>`, so the worst case is 133 plus the longest line plus 11 - and the
//     box link's session has kept exactly this window and this guard since phase 1's task 05
//     (src/common/link/session/Session.h, tasks/phase-1/reports/05.md has the arithmetic). The
//     console had nothing behind its window at all; with ten settings a listing reaches 160
//     characters and the NUL would have gone one byte past the buffer. What is printed is the
//     window's own 159 characters, which is what the link sends too.
//   - both are file-scope statics, so that the first boot can borrow them while setup() runs
//     (src/roles/repeater/app/FirstBoot.h) and there is one place that says how much room
//     upstream's interpreter has.
static const size_t kReplyWindow = 160;  // what upstream's console gives its interpreter
static const size_t kReplyGuard = 64;    // and the room behind it, as the link keeps it
static char command[160];
static char reply[kReplyWindow + kReplyGuard];

void setup() {
  Serial.begin(115200);
  delay(1000);

  board.begin();

#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.begin();
#endif

  if (!radio_init()) {
    halt();
  }

  fast_rng.begin(radio_driver.getRngSeed());

  FILESYSTEM* fs;
  InternalFS.begin();
  fs = &InternalFS;
  IdentityStore store(InternalFS, "");
  if (!store.load("_main", the_mesh.self_id)) {
    the_mesh.self_id = radio_new_identity();   // create new random identity
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  Serial.print("Repeater ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  command[0] = 0;

  sensors.begin();

  // One tick before the clock keeper looks: VolatileRTCClock accumulates millis() between ticks
  // and its prev_millis was set when the global was constructed, so without this the first tick
  // of loop() would hand the keeper the whole of setup() at once - a second or three - and its
  // poll would read that as somebody setting the clock (src/helpers/ArduinoHelpers.h at the pin,
  // VolatileRTCClock::tick; upstream's main.cpp ticks only in loop()).
  rtc_clock.tick();

  // The clock, before begin() and before loop(), so that nothing of upstream's can stamp a
  // packet or a preference with the 2024 date its volatile clock starts at: a hardware RTC if one
  // answered, otherwise the newest time this node knows (src/roles/repeater/clock/, ARCHITECTURE section 5).
  // After radio_init(), which is where rtc_clock.begin(Wire) probes the bus for that RTC.
  twinoak::clock::begin(InternalFS);

  // Decided before begin() runs loadPrefs, acted on after it: a node with no saved preferences
  // gets a random admin password and then the first-boot profile, through upstream's own console
  // interpreter, in one hold of the preference saves - so the settings file appears once, with
  // both in it (src/roles/repeater/app/FirstBoot.h). Before the radio is used: nothing has been
  // sent or received at this point, loop() has not run, and the console's two buffers are idle
  // and are what the profile is given. The password itself is never printed.
  const bool saved_prefs = twinoak::hasSavedPrefs(InternalFS);
  the_mesh.begin(fs);
  const twinoak::FirstBootOutcome first_boot = twinoak::firstBoot(
      the_mesh, saved_prefs, command, sizeof(command), reply, sizeof(reply));
  if (first_boot.password_set) {
    Serial.println("First boot: a random admin password was set (not shown; set yours with 'password <new>')");
  }
  if (first_boot.was_first_boot) {
    Serial.print("First boot: the profile ran ");
    Serial.print(first_boot.profile.ran);
    Serial.print(" of ");
    Serial.print(first_boot.profile.ran + first_boot.profile.refused);
    Serial.print(" lines (sensor get firstboot.state says '");
    Serial.print(twinoak::firstboot::stateText());
    Serial.println("'; docs/provisioning.md is the rest)");
    // The command of the first refusal, if there was one: ours, never a password (no line of the
    // profile may carry one - test/test_firstboot holds that), and the reply is not printed.
    if (first_boot.profile.first_refused != nullptr) {
      Serial.print("First boot: refused: ");
      Serial.println(first_boot.profile.first_refused);
    }
  }
  command[0] = 0;
  reply[0] = 0;

  // The box link on Serial1, after the radio (its RNG draws the boot identifier) and after
  // begin() (the advert name IDENTITY reports comes from the loaded preferences).
  twinoak::link::linkBegin(the_mesh);

  // The boot advert is not sent here: it waits for a clock, in loop() below.

  board.onBootComplete();
}

void loop() {
  // Handle Serial CLI
  size_t len = strlen(command);
  while (Serial.available() && len < sizeof(command)-1) {
    char c = Serial.read();
    if (c != '\n') {
      command[len++] = c;
      command[len] = 0;
      Serial.print(c);
    }
    if (c == '\r') break;
  }
  if (len == sizeof(command)-1) {  // command buffer full
    command[sizeof(command)-1] = '\r';
  }

  if (len > 0 && command[len - 1] == '\r') {  // received complete line
    Serial.print('\n');
    command[len - 1] = 0;  // replace newline with C string null terminator
    // The window, and the guard behind it zeroed with it: a line that writes past 160 bytes
    // writes into the guard and not into this function's frame, and what is printed is the
    // window's own bytes (the two buffers above say why).
    memset(reply, 0, sizeof(reply));
    the_mesh.handleCommand(0, command, reply);  // NOTE: there is no sender_timestamp via serial!
    reply[kReplyWindow - 1] = 0;
    if (reply[0]) {
      Serial.print("  -> "); Serial.println(reply);
    }

    command[0] = 0;  // reset command buffer
  }

  // Before the mesh's own pass: a clock the console just set is noticed in the same pass that
  // set it, and the advert below is decided with this pass's state.
  twinoak::clock::loop();

  // Upstream sends this in setup() with a 16-second delay; ours waits instead until the clock is
  // set or the hold times out (spec/LINK-PROTOCOL.md section 5.4, ARCHITECTURE section 5), and
  // then sends it zero-hop as upstream does, with the 1500 ms delay upstream's own `advert`
  // command uses. Upstream's periodic adverts stay upstream's: the first of those cannot fire
  // before two minutes (MyMesh::updateAdvertTimer, advert_interval 1 = 2 minutes for a new
  // install), which is after this hold.
#if ENABLE_ADVERT_ON_BOOT == 1
  if (twinoak::clock::bootAdvertDue()) {
    the_mesh.sendSelfAdvertisement(1500, false);
    twinoak::clock::noteBootAdvertSent();
  }
#endif

  the_mesh.loop();
  twinoak::link::linkLoop();
  sensors.loop();
  rtc_clock.tick();

#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.loop();
#endif
  if (the_mesh.getNodePrefs()->powersaving_enabled && !the_mesh.hasPendingWork()) {
    board.sleep(0); // nrf ignores seconds param, sleeps whenever possible
  }
}
