# Flashing a unit

One page, for a person with a TwinOak repeater on a desk and a release open in
a browser. [`docs/release.md`](release.md) is what a release carries and how it
is made; this is what to do with it.

Three ways in, and the release carries a file for each: **a cable** on the
adapter's USB-C (`.uf2` or `.zip`), **the Walter** over SWD (`.bin`), and — once
per new radio, never as an update — **the base** (`.hex`).

## 1. Which file

Every asset of a release is named
`twinoak-<role>-<board>-<band>-vX.Y.Z.<ext>`, so the role is in the name:

| File | What it is |
|---|---|
| `twinoak-repeater-rak4630-single-868-vX.Y.Z.uf2` | **the repeater** — the firmware a unit runs. UF2, for the bootloader's drive |
| `twinoak-repeater-rak4630-single-868-vX.Y.Z.zip` | the same firmware as a serial-DFU package |
| `twinoak-repeater-rak4630-single-868-vX.Y.Z.bin` | the same firmware as the raw application bytes, for SWD |
| `twinoak-scanner-rak4630-single-868-vX.Y.Z.*` | **NoiseScope**, the spectrum and noise scanner ([`docs/noisescope.md`](noisescope.md)) — flashed *in place of* the repeater when a site has a noise problem, and the repeater flashed back afterwards. It never transmits |
| `twinoak-base-rak4630-0.4.4-s140-6.1.1.hex` | **the base under the application** — MBR, SoftDevice, bootloader. A factory radio gets it once, from the Walter; §4 |
| `manifest.json`, `manifest.json.sig` | what each image is, and one Ed25519 signature over it. `SHA256SUMS` is beside them |

**The repeater and the scanner are the same board and the same band**: the only
thing that tells them apart in the file name is the word after `twinoak-`. A
unit on the air wants `repeater`.

Before flashing anything, check what you downloaded. In the release's own
directory, with this repository checked out beside it:

```bash
sha256sum -c SHA256SUMS                       # every file of the release, the manifest included
python <repo>/spec/tools/manifestref.py verify <64 hex> manifest.json
```

The `<64 hex>` is the key `keys/release.pub` publishes — the hexadecimal half of
its `ed25519 <key id> <64 hex>` line, which is also what every Walter is built
to trust ([`keys/README.md`](../keys/README.md)). `manifest.json.sig` names the
key id it was signed with, so the two can be read against each other by eye
before the verifier is run at all.

A release whose signature does not verify is not one of ours, and the Walter
refuses it too (`spec/FIRMWARE-MANIFEST.md` §5).

## 2. By cable, on the adapter's USB-C

The way a board on a bench is flashed. Nothing is erased first and nothing has
to be: an image lives in the application region and the file system — the node's
identity, its settings, its ACL, its clock — is above it and untouched
([`docs/ARCHITECTURE.md`](ARCHITECTURE.md) §9).

**The drive (`.uf2`), which is the simplest.** Press the adapter's reset button
**twice, quickly**. A drive called `RAK4631` appears; copy the `.uf2` onto it.
The board reboots by itself when the copy finishes.

**Serial DFU (`.zip`)**, which is what a script uses. The board has to be in its
bootloader first — the same double-tap, or the **1200-baud touch**: open the
application's port at 1200 baud and close it again, which the Adafruit
bootloader takes as *go to serial DFU*. Then:

```bash
adafruit-nrfutil dfu serial -pkg twinoak-repeater-rak4630-single-868-vX.Y.Z.zip -p COM29 -b 115200
```

Two things worth knowing, both measured on the first board
([`docs/bench.md`](bench.md) §0a):

- **The port changes when the board does.** The application, the bootloader
  after a double-tap, and the bootloader after a 1200-baud touch are three USB
  product ids (`239A:8029`, `239A:0029`, `239A:002A`) and three COM numbers on
  Windows — with the *same* serial number, which is the nRF52840's device id.
  Look for the port that appeared, not for the one you had.
- **`adafruit-nrfutil` exits 0 even when its write failed.** The line to read is
  its own `Device programmed.`; an exit code of zero without it is a flash that
  did not happen.

`python tools/bench/bench.py flash <zip>` is the same DFU with the guards and a
log, for a board listed in `bench.local.json` (`docs/bench.md` §0a).

## 3. Through the Walter

A unit in service is flashed by the Walter in the other box, over SWD, with the
`.bin` — the raw application bytes, written at the `load_address` the manifest
gives and nowhere else. Two ways to start it:

- **From the Walter's console:** `rak update --first-install` for a radio that
  has no golden image yet, `rak update` afterwards; `rak update status` tells
  the story of the last one and `rak role scanner|repeater` swaps the two images
  of the same release. The Walter's own documentation is its repository's.
- **From meshmaster.dk**, once the Walter has checked in: the exit node's page,
  *Offer a release* → the unit downloads the one image that is its own, stores it
  as the candidate and says what it made of it; then *Apply the candidate*. A
  rollback is the system working: the golden image comes back and the candidate
  goes on the bad list.

The Walter checks the manifest, the signature, the image's own descriptor and
the region before it writes anything, and holds the `HELLO` it gets afterwards
against the descriptor it wrote. **No chip erase, ever.**

## 3a. A radio left half-written

**6 November 2026.** A Walter at `v0.1.0` began a first install over SWD and
its own watchdog reset it part-way through, leaving the radio's application
region partly written and the radio with nothing it could start. The radio came
back over its own USB-C with `v0.1.0`'s `.uf2` — **and as a new node.** Its
next `HELLO` carried a new key, meshmaster.dk logged
`agent.companion_changed`, its repeater had to be registered again (41, where it
was 12), and its admin password was a random one nobody knows (`spec/SYSTEM.md`
S-13). The first version of this section promised the key would survive; it
did not, and `tasks/phase-3.9/reports/01.md` is the reading of why. What
follows is the recovery as it should be done, and what to expect at the end
of it.

**0. On the Walter first: `rak update status`, and keep the box powered.**

- An update that stopped in *flashing-candidate* is **resumed at the Walter's
  next boot** (the Walter's `docs/update.md`, *After a power cut*): a Walter
  that reboots — and "power the box down and up" reboots it — flashes the
  journalled image over whatever you have just put on the radio by hand.
  `rak update status` says which state the last run ended in. *stuck*, *done*
  and *rolled back* are terminal and resume nothing; *flashing-candidate*,
  *booting* and *proving* are not, and the Walter has to finish or abandon
  that run before the radio is touched by hand. (The first unit's run ended
  *stuck*, so nothing resumed there.)
- **On the production adapter the radio's USB-C does not power the radio**
  (the hardware repository's `BENCH-BREADBOARD.md` §6, item 7): the box must
  be on. A dead-looking radio on a cable alone is a radio with no power.

**1. Plug a cable into the adapter's USB-C and look for the `RAK4631` drive.**
An interrupted install leaves the radio in its bootloader by itself, so the
drive is very likely there with nothing pressed. The Walter's flasher writes
*bank 0 holds nothing* into the bootloader's settings page **before** its first
erase and *bank 0 holds a valid application* only **after** the whole image has
been read back and its SHA-256 checked, and the vector-table page is the first
it erases — so between those two points the bootloader refuses the application
on both of its counts and comes up in its own DFU with no timeout.

*What was checked, and in which bootloader.* The radio runs RAK's **0.4.4**, of
which RAK publishes no source; the 0.4.3 source it publishes beside it says
`bootloader_app_is_valid()` refuses an application whose first two words are
blank or whose settings page does not say `BANK_VALID_APP`, and
`check_dfu_mode()` then starts DFU with no timeout. The same function was found
in the **0.4.4 binary** vendored at `boards/base/rak4630/`
(`out/p3.4-01/rak-0.4.4_s140_6.1.1.dis.txt`, at `0xF5468`): the same two-word
blank test, the same `bank_0 == 1` test and the same optional CRC, instruction
for instruction, and the call that starts DFU keeps both of its shapes (a
3-second timeout only for a single tap or a UF2/serial magic, none otherwise).
0.4.4's one stated change, *BLE OTA recovery*, is a path that runs when the
application is invalid and brings the BLE stack up; what it does to the USB
drive could not be read out of the binary with certainty, which is why steps 2
and 3 are there.

**2. If no drive appears, reset the radio once** — the adapter's reset button,
or the Walter's `rak reset` (not a power cycle of the box: step 0). A flash that
was interrupted can have left the core halted over SWD, and only a reset puts
it back on its own feet.

**3. If there is still no drive, double-tap the reset button.** That works
whatever is in the application region: the double tap is the bootloader's own —
two resets on the pin within half a second.

What each path can write, and no more:

- **the Walter's install** writes the application region (`0x26000`–`0xED000`:
  the pages the image covers, and only those) and exactly one page outside it,
  the bootloader's settings page at `0xFF000`. Its flash layer refuses every
  other address (the Walter's `flasher.cpp` at its `v0.1.0`, `FlashRegion`).
- **this release's `.uf2`** (family `0xADA52840`) is written by the bootloader's
  UF2 handler, which takes a block of that family only below `0xEA000` and
  refuses one above; every block of the file is inside the application region.

Neither path can reach the bootloader at `0xF4000`, and neither writes the
file system at `0xED000`–`0xF4000` where the node's key, its settings, its ACL
and its clock live. `rak provision` can — it erases the chip whole and writes
the base, bootloader included — and so can the bootloader's own
bootloader-family UF2 branch; neither is part of a recovery.

**4. Copy the release's `.uf2` onto the drive.** §2, the ordinary way in. The
board reboots by itself when the copy finishes.

### The key

**A recovery may give the radio a new identity, as the first one did.** Not
because either path above writes the file system — neither does — but because
of what the firmware did with that file system on its next boot. Up to
`v0.1.1`, the first boot after the `.uf2` called the Adafruit core's
`InternalFS.begin()`, which **erases the whole region and formats it when the
volume will not mount**, and then found no key file and drew a new key: a new
node, with no word said. A volume stops mounting when one of its seven 4 KB
pages is torn: the core keeps LittleFS in 128-byte blocks on 4096-byte pages
and rewrites a page by erasing it and programming it again, so a reset, a
halt over SWD or a power dip inside that window takes 32 blocks at once — the
first page holds the superblock and the root directory. This firmware writes
that page twice in the first seconds of every boot (the clock's lower bound,
and again when the Walter sets the time), which is when a second reset is most
likely. Which of the evening's resets did it to the first radio cannot be told
now: the region was erased by the format.

**From `v0.1.2` the radio refuses instead.** It mounts without the format, and
makes a key only where no node was: a region that is blank (the base just
written) or a volume with neither a key file nor settings. A torn volume, or
settings without a key, is a **lost identity**: no key is made, nothing is
written, the mesh is not started, and the radio says so on its USB console
every ten seconds and on the link —

```
IDENTITY LOST: the file system at 0xED000 would not mount and is not blank. No key was made
and the mesh is not started. 'erase' then 'reboot' makes this radio a new node; the old key
cannot be recovered (docs/flashing.md 3a)
```

— `HELLO` answers as always, `IDENTITY` answers `ERR_STATE`, and an
`EV_ATTENTION` of reason 6 with detail 1 wakes a sleeping Walter. The way out
is the person's: `erase` (the core's erase-and-format, now on your word) and
`reboot`, at the USB console or as `rak cli erase` and `rak cli reboot` on the
Walter. The old key is gone either way — nothing keeps a copy of a private key
— but the radio does not become somebody else by itself, and the mesh and
MeshMaster are not told a new key until you have decided to make one.

**How to tell**, after any recovery:

- on the Walter, `rak hello` says which firmware answered (`firmware 0.1.2+…`),
  and `agent id` prints the repeater's key as `IDENTITY` returned it — compare
  it with the key MeshMaster knows for the unit. On a radio in the lost mode
  `agent id` has no key to print and `rak cli ver` answers the `IDENTITY LOST`
  line;
- on meshmaster.dk, the unit's exit-node page shows **the companion key
  changed** and the audit log has `agent.companion_changed` with the old and
  the new key (its `docs/ops/exit-nodes.md`); on a radio in the lost mode the
  heartbeat carries no radio key at all;
- at the USB console, `get public.key` is the node's key, and the boot prints
  `IDENTITY LOST` or `Repeater ID:`.

**What then**, when the key is new:

1. **Confirm the key on meshmaster.dk** — *This is the right companion* on the
   exit node's page, which lifts the suspension of admin routing through it and
   re-grants the pairs it can.
2. **Register the unit's repeater again** from that key: the old repeater
   record is another node's now.
3. **Re-apply what a person had set**, through the Walter's `rak cli <line>`
   or at the USB port, each line as `docs/provisioning.md` §2 gives it: the
   `name`, `lat` and `lon`, `owner.info`, the **admin password** (`password
   <new>` — the first boot drew a random one nobody knows), the **guest
   password**, the regions (`region default dk`, `region save`), and anything
   else that was not the profile's: the three delays, `flood.max`, a lower duty
   cycle, the ACL (`setperm`). Then tell MeshMaster the admin password
   (`docs/provisioning.md` §5).

   **What the profile restores by itself**, on the first boot, so there is no
   need to type it: `path.hash.mode 1`, `dutycycle 10`, `agc.reset.interval 4`,
   `loop.detect moderate`, `flood.max.unscoped 15` and `flood.advert.interval`
   (60–85 hours, derived from the new key, so it will differ from the old
   unit's). The radio channel is the build's and needs no line.

**The one line that shows it worked**, on the Walter's console:

```
> rak hello
hello: protocol 1.0, role 1 (repeater), slot 0, max payload 240, boot_id 0x5eed1234
  firmware 0.1.2+<sha7>, board rak4630-single, upstream repeater-v1.18.0+81dbae59
  capabilities 0x7f: CLI, POWER, ENV, MESH_CLIENT, OBSERVATIONS, ATTENTION, TIME_PERSIST
```

or the same question from a laptop on the link pins, which asks in 1.1 and is
answered in it:

```bash
python3 tools/linkcli.py --port COM7 hello    # protocol 1.1, and band 868 at the end
```

`firmware` is the reading that matters: the version is *inside* the image, so
`0.1.2+<sha7>` is this release running and `0.0.0-dev` would be a build
somebody made by hand. `capabilities 0x7f` is a node with its key; a node in
the lost mode advertises `0x21` (`CLI`, `ATTENTION`) and nothing else. A
**0.1.0** Walter says `protocol 1.0` and prints no band — correct, and all it
ever said ([`docs/link.md`](link.md) §1).

## 4. A radio fresh from the factory: the base, once

A RAK4630 arrives with RAK's RUI3, not the MBR, SoftDevice and bootloader this
firmware is linked against, so there is nothing for an update to update. The
Walter gives it the base **once**, from the release's `provision` file, on a
person's command:

```
rak provision check                         # reads only; prints the exact line to type next
rak provision <DEVICEID> confirm            # a chip erase, the base, the UICR words, one reset
```

It **erases the radio whole** — RUI3, any identity, UICR — which is why it wants
the radio named and the word `confirm`. An update never touches the base
(`spec/SYSTEM.md` S-5a). Afterwards the radio shows only its bootloader, and the
first application it gets is an ordinary first install: §2 by cable, or
`rak update --first-install`.

## 5. Afterwards: four things to look at

On the USB console at 115200 (`docs/provisioning.md` §3 is the long version), or
over the box link from a laptop with a 3.3 V USB-UART dongle on the link pins
(`docs/link.md` §1 has the wiring):

| | What to look for |
|---|---|
| **the firmware is the one you flashed** | `sensor get fw.version` → `twinoak-repeater X.Y.Z+<sha7> on MeshCore repeater-v1.18.0 (…)`. The version is *inside* the image, so this is the release and not a file name |
| **the first boot ran, once** | `sensor get firstboot.state` → `done` on the boot that found no settings, `never` on every boot after. `partial: <n> refused` is a line that did not take, and the console named the first one |
| **the channel is the Danish one** | `get radio` → `> 869.618,62.5,8,8`, and `get dutycycle` → `> 10.0%`. A `> 50.0%` is a unit flashed before the band was in the build, and it is one line to fix |
| **it is heard** | a zero-hop advert goes out once the clock is trustworthy, and a flood advert every 60–85 hours. A phone or another node on 869.618 MHz is what hears it; the unit's own `get public.key` is its name on the mesh |

And that the box link answers, which is the half the Walter needs:

```bash
python3 tools/linkcli.py --port COM7 hello        # the version, role, slot and capabilities
python3 tools/linkcli.py --port COM7 ping         # the main loop is turning
python3 tools/linkcli.py --port COM7 identity     # the public key and the advert name
```

For a scanner image it is the scanner's own lines instead — `info`, `stat`, a
`sweep` — which are NDJSON on the same UART (`docs/noisescope.md`).

## 6. The admin password

**A freshly flashed unit has an admin password nobody knows.** The first boot
draws fifteen random alphanumerics from the radio's own noise, never prints them
and never puts them on the air (`spec/SYSTEM.md` S-13). That is deliberate: no
phone can administer a unit that nobody has provisioned, and the USB port and the
box link are the only two ways in to a fresh one.

So **set your own before the unit goes out**, at the USB port:

```
password <the one you are going to keep>
  -> password now: <the one you are going to keep>
```

Three things about that line:

- **Upstream echoes it back**, on purpose, so it is on the screen of whatever is
  plugged into the field port. Provision at a bench, not over somebody else's
  terminal. `tools/linkcli.py provision` prints stars instead, and never prints
  the reply.
- **It is the only copy there is.** Nothing in this firmware can read it back:
  `get` has no branch for it. A password set and not written down is a unit that
  has to be erased and provisioned again — and an `erase` takes the identity with
  it.
- **The guest password starts empty**, and an empty one makes every blank login
  succeed at guest permission. Setting it is a decision either way
  (`docs/provisioning.md` §2).

[`docs/provisioning.md`](provisioning.md) is the rest: the six settings that make
a board *this* unit, the USB session line for line, the same lines over the link,
and what MeshMaster has to be told afterwards.
