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
