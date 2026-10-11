// Ours. setup() and loop() are MeshCore's examples/simple_repeater/main.cpp at the pin d9296435
// (repeater-v1.17.1), with:
//   - `the_mesh` a TwinOakMesh (src/app/TwinOakMesh.h) where upstream constructs a MyMesh;
//   - the display (DISPLAY_CLASS, UITask), Ethernet (ETHERNET_ENABLED) and the SenseCAP button
//     left out - this adapter has none of them;
//   - the ESP32 / RP2040 / STM32 file-system branches left out - this is an nRF52 and nothing
//     else (the external-watchdog hook stays, as upstream has it, for a board that defines it);
//   - the debug-only delay and the debug print lines left out - nothing the mesh says is logged
//     as text (spec/SYSTEM.md S-12);
//   - the first-boot admin password after the_mesh.begin() (src/app/FirstBoot.h, S-13);
//   - `len` in the console loop a size_t, so that -Werror accepts the comparison with
//     sizeof(command) that upstream writes with an int.
// The USB serial console - the read loop, the 160-byte command and reply, handleCommand(0, ...),
// the "  -> " echo - is upstream's, unchanged. A pin bump re-reads upstream's main.cpp against
// this file (docs/upstream.md).

#include <string.h>

// Upstream's headers come in through these two and through nothing else here
// (src/board/UpstreamHeaders.h says why).
#include "app/FirstBoot.h"
#include "app/TwinOakMesh.h"

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

  // Decided before begin() runs loadPrefs, acted on after it: a node with no saved preferences
  // gets a random admin password, which begin()'s savePrefs() path never sees and the next boot
  // loads. The password itself is never printed.
  const bool saved_prefs = twinoak::hasSavedPrefs(InternalFS);
  the_mesh.begin(fs);
  if (twinoak::ensureAdminPassword(the_mesh, saved_prefs)) {
    Serial.println("First boot: a random admin password was set (not shown; set yours with 'password <new>')");
  }

  // send out initial zero hop Advertisement to the mesh
#if ENABLE_ADVERT_ON_BOOT == 1
  the_mesh.sendSelfAdvertisement(16000, false);
#endif

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

  the_mesh.loop();
  sensors.loop();
  rtc_clock.tick();

#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.loop();
#endif
  if (the_mesh.getNodePrefs()->powersaving_enabled && !the_mesh.hasPendingWork()) {
    board.sleep(0); // nrf ignores seconds param, sleeps whenever possible
  }
}
