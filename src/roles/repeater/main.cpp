// Ours. setup() and loop() are MeshCore's examples/simple_repeater/main.cpp at the pin d9296435
// (repeater-v1.17.1), with:
//   - `the_mesh` a TwinOakMesh (src/roles/repeater/app/TwinOakMesh.h) where upstream constructs a MyMesh;
//   - the display (DISPLAY_CLASS, UITask), Ethernet (ETHERNET_ENABLED) and the SenseCAP button
//     left out - this adapter has none of them;
//   - the ESP32 / RP2040 / STM32 file-system branches left out - this is an nRF52 and nothing
//     else (the external-watchdog hook stays, as upstream has it, for a board that defines it);
//   - the debug-only delay and the debug print lines left out - nothing the mesh says is logged
//     as text (spec/SYSTEM.md S-12);
//   - the first-boot admin password after the_mesh.begin() (src/roles/repeater/app/FirstBoot.h, S-13);
//   - `len` in the console loop a size_t, so that -Werror accepts the comparison with
//     sizeof(command) that upstream writes with an int;
//   - the box link on Serial1, one call in each (src/roles/repeater/link/BoardLink.h, docs/ARCHITECTURE.md 4);
//   - the clock keeper, one call in each, before the mesh's own, and one rtc_clock.tick() in
//     setup() ahead of it (src/roles/repeater/clock/BoardClock.h, and the comment where it is called);
//   - the boot advert moved out of setup() and into loop(), where it waits for a clock
//     (docs/ARCHITECTURE.md 5; upstream sends it in setup() with a 16-second delay).
// The USB serial console - the read loop, the 160-byte command and reply, handleCommand(0, ...),
// the "  -> " echo - is upstream's, unchanged. A pin bump re-reads upstream's main.cpp against
// this file (docs/upstream.md).

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

static char command[160];

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
  // gets a random admin password, which begin()'s savePrefs() path never sees and the next boot
  // loads. The password itself is never printed.
  const bool saved_prefs = twinoak::hasSavedPrefs(InternalFS);
  the_mesh.begin(fs);
  if (twinoak::ensureAdminPassword(the_mesh, saved_prefs)) {
    Serial.println("First boot: a random admin password was set (not shown; set yours with 'password <new>')");
  }

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
    char reply[160];
    reply[0] = 0;
    the_mesh.handleCommand(0, command, reply);  // NOTE: there is no sender_timestamp via serial!
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
