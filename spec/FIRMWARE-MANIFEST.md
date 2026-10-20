# The firmware manifest, its signature, and the image descriptor

What a release of either firmware says about itself, and how the next reader in the chain knows it
is true. Three readers depend on it: the release pipelines that write it, the Walter that decides
what may be flashed, and MeshMaster's firmware catalogue. `tools/manifestref.py` is this document
as code; `fixtures/manifest/` is what it generates.

**Status:** schema 1, written 2026-10-11. **2026-10-20:** the `provision` file (§4a) — the base under
the repeater, which the Walter writes once to a new radio (`SYSTEM.md` S-5a). Added to schema 1
before the first release was signed: every reader refuses a `use` it does not know, so the three
readers take it in one round (the repeater's release tooling, the Walter, MeshMaster's catalogue).

---

## 1. The chain

```
release workflow ──► manifest.json + manifest.json.sig + the image files      (a GitHub Release)
        │
        ▼
MeshMaster's catalogue ──► serves the same bytes to a unit that asks          (it cannot change them)
        │
        ▼
the Walter ──► checks the signature, then the manifest, then each file's hash,
               then the descriptor inside the image ──► only then stores or flashes anything
```

The signature is made where the image is built and checked where it is used. Nothing in between —
the server, the mobile network, a person with a laptop — is trusted with more than delivering
bytes. A server that is broken into can withhold an update; it cannot make a unit run firmware the
release key did not sign.

## 2. `manifest.json`

One per release, covering every image of that release. Schema 1:

| Field | Type | |
|---|---|---|
| `schema` | integer | 1 |
| `product` | string | `twinoak-repeater-firmware` or `twinoak-walter-firmware` |
| `version` | string | `X.Y.Z+<sha7>` — what the firmware itself reports |
| `tag` | string | the git tag, `vX.Y.Z` |
| `commit` | string | 40 hex characters |
| `built_at` | string | UTC, `YYYY-MM-DDTHH:MM:SSZ` |
| `link` | object | `major`, `minor`: the box-link version the firmware speaks (`LINK-PROTOCOL.md`) |
| `upstream` | object, optional | `name`, `tag`, `commit` of what it is built on (MeshCore, for the repeater) |
| `images` | array | at least one, below |

An **image** is one role on one board in one band — the unit of "what can go on this radio":

| Field | Type | |
|---|---|---|
| `id` | string | unique within the manifest: `<role>.<board>.<band>` |
| `role` | string | `repeater`, `bridge`, `scanner`, `manager` |
| `board` | string | `rak4630-single`, later `rak4630-dual-a` …; `walter` |
| `band` | string | `868`, `433`; `lte-m` for the Walter |
| `mcu` | string | `nrf52840`, `esp32s3` |
| `app_region` | object | `start`, `end` as `0x%08X` strings: the flash a `swd` file may occupy on this board. Required when the image has a `swd` file |
| `requires` | object, optional | what must already be there: `walter` (minimum Walter version), `link_major` |
| `files` | array | at least one, below |

A **file** is one way of delivering that image:

| Field | Type | |
|---|---|---|
| `use` | string | `swd` raw binary for the Walter's SWD flasher · `usb-dfu` nrfutil package for a laptop · `usb-uf2` for the UF2 drive · `ota` the Walter's own application image · `factory` the Walter's merged image for a board out of the box · `debug` an ELF · `provision` the base under the application on a new radio: MBR, SoftDevice, bootloader and the UICR words they need (§4a) |
| `name` | string | the file name in the release, no path |
| `format` | string | `bin`, `zip`, `uf2`, `elf`; `hex` (Intel HEX) for `provision`, and `hex` for nothing else |
| `size` | integer | bytes |
| `sha256` | string | 64 lowercase hex characters |
| `load_address` | string | `0x%08X`; required for `swd`, `ota` and `factory` |

Rules a consumer applies **after** the signature has passed (`manifestref.validate`):

- Unknown fields are ignored; an unknown `schema` or `product` is refused.
- A `swd` file must lie inside its image's `app_region`: `start ≤ load_address` and
  `load_address + size ≤ end`. The region is the board's — everything outside it (the bootloader,
  the SoftDevice, the file system that holds the node's identity and settings, UICR) is not
  something an update may write.
- A `provision` file is `format` `hex`, has **no** `load_address` (its records say where every byte
  goes), belongs to an image whose `mcu` is `nrf52840` and which has an `app_region`, and there is
  at most one in an image. The same file may be listed under several images of one board (the hash
  makes it one file). Whether its bytes are a base is §4a, checked by whoever is about to use it.
- A file is identified by its hash, never by its name.

`app_region` in the fixtures is an example. The real numbers come from the firmware's own linker
script and are written by the repeater repository's release tooling, which has a test that holds
them against the script.

**How ours is written.** UTF-8 without a BOM, two-space indent, LF, one trailing newline, keys in
the order of the tables above (`manifestref.canonical`). That is a convention for diffs, not a
requirement: what is signed is the file's bytes exactly as they are.

## 3. The signature

`manifest.json.sig` is one line of ASCII:

```
ed25519 <key id> <signature>
```

- The algorithm is **Ed25519 as in RFC 8032** — no pre-hash, no context — over the bytes of
  `manifest.json`.
- `<signature>` is the 64 signature bytes in standard base64.
- `<key id>` is 16 lowercase hex characters: the first eight bytes of SHA-256 over the raw 32-byte
  public key. It lets a verifier that trusts more than one key pick the right one; it is not a
  secret and proves nothing by itself.

**Keys.**

- The private key exists in one place: a secret of the release workflow. It is created once, by a
  person (`manifestref.py keygen`), and never committed, logged or pasted anywhere else.
- The public key is a file in each firmware repository, compiled into the Walter firmware. A Walter
  build trusts a small fixed list of keys (room for three), so a key can be replaced by shipping a
  Walter release that trusts both the old and the new one, then one that trusts only the new.
- `fixtures/manifest/TEST-KEY.txt` is a key pair for tests, public on purpose. A build that trusts
  it is a test build. Release tooling refuses to sign a tagged release with it, and the Walter's
  build refuses to include it outside test builds.
- A Walter with no release key compiled in accepts no update at all. That is the state before the
  key exists, and it is safe.

Because the manifest carries the hash of every file, one signature covers the whole release.

## 4. The descriptor inside an image

A manifest describes files; a file can be renamed or mixed up. So every image also carries, inside
itself, 96 bytes that say what it is. The Walter reads them from the bytes it is about to flash and
compares them with the manifest entry and with the radio it is about to flash them to.

| Offset | Size | Field | |
|---:|---:|---|---|
| 0 | 8 | `magic` | ASCII `TWOKFWD1` |
| 8 | 2 | `length` | 96, little-endian |
| 10 | 1 | `role` | 0 unknown, 1 repeater, 2 bridge, 3 scanner, 16 manager (the Walter) |
| 11 | 1 | `slot` | `0xFF` = any; otherwise the only radio position this image is for |
| 12 | 1 | `link_major` | |
| 13 | 1 | `link_minor` | |
| 14 | 2 | `flags` | 0 |
| 16 | 32 | `version` | ASCII, zero-padded, at least one terminating zero |
| 48 | 24 | `board` | same |
| 72 | 8 | `band` | same |
| 80 | 12 | reserved | zero |
| 92 | 4 | `crc32` | CRC-32 (IEEE 802.3, as zlib) over bytes 0–91, little-endian |

The descriptor sits on a 4-byte boundary anywhere in the image; a reader finds it by searching for
the magic and checking the CRC. **Exactly one** valid descriptor must be present: none, or two,
and the image is refused. The firmware's build places it (a `const` structure the linker is told to
keep); the release tooling checks it is there and that it agrees with the manifest before anything
is published.

The same three strings — `version`, `board`, role — are what the running firmware reports in the
link's `HELLO`. After a flash, the Walter's health check holds the `HELLO` it gets against the
descriptor it flashed.

## 4a. The base: a `provision` file

A factory radio arrives with its maker's firmware (RAK's RUI3), not the MBR, SoftDevice and
bootloader a TwinOak application is linked against. The base is those, as one Intel HEX file per
board, written to a radio **once**, by a person's command (`SYSTEM.md` S-5a), never by an update.
It carries no descriptor (it is not ours, and it is not an image); what it may contain is held by
these rules instead, in this order, against the `app_region` of the image it is listed under
(`manifestref.validate_provision`):

1. Records `00`, `01`, `02`, `04` (`03` and `05`, start addresses, are read and ignored); every
   checksum good; exactly one end record, and nothing after it; no address given two different
   bytes.
2. Every byte is in flash (`0x00000000`–`0x000FFFFF`) or in UICR's registers (`0x10001000`–
   `0x10001307`: `REGOUT0`, at `0x10001304`, is the last).
3. Nothing in `UICR.APPROTECT` (`0x10001208`): that word depends on the chip's revision, and the
   Walter writes it from the chip's `INFO.VARIANT`, not from a file.
4. An MBR: data at `0x00000000`.
5. A SoftDevice information structure at `0x00003000` (MBR size `0x1000` + Nordic's
   `SD_INFO_STRUCT_OFFSET` `0x2000`, `nrf_sdm.h`): magic `0x51B1E5DB` at `+0x04`; the end address
   at `+0x08` **equal to `app_region.start`**; `SD_ID` 140 at `+0x10` and `SD_VERSION` 6001001 at
   `+0x14` — S140 6.1.1, what the application is linked against.
6. `UICR.NRFFW[0]` (`0x10001014`, the bootloader's address, read by the MBR): present, a 4 KB page,
   at or above `app_region.end`, below `0x00100000`, and the file has data there.
7. No flash byte between `app_region.start` and `NRFFW[0]`: the application and the file system
   (the node's identity) are never the base's.

`NRFFW[1]` (the MBR's parameter page) and other UICR words are allowed and written as the file says.
The rules are about what the file may hold. How the Walter erases, writes and checks a chip is the
Walter's (`walter-firmware` `docs/provisioning.md`).

## 5. What the Walter checks, in order

1. The signature line names a key it trusts and the signature verifies over the manifest's bytes.
2. The manifest validates (§2) and its `product` is one this unit takes.
3. There is an image whose `role`, `board` and `band` are what this radio position holds or may
   hold, and whose `requires` this unit meets.
4. The downloaded file's size and SHA-256 are the manifest's.
5. The file holds exactly one descriptor, and it agrees with the manifest entry.
6. For a `swd` file: it fits the `app_region`, and the region is one this Walter build knows for
   that board. A manifest cannot widen what may be written.

Only after all six is the file stored as a candidate. Flashing it, checking its health and keeping
or rolling it back is the update procedure (`walter-firmware`), not this document.

For a `provision` file, 5 and 6 are one check instead: **5p.** the image's `app_region` lies inside
the region this Walter build knows for the board, and the file passes §4a against **the build's**
region — the one the radio really has, which a manifest cannot move. It is then stored as the slot's
base — never as a candidate, never anything an update can reach.

## 6. The fixtures

`fixtures/manifest/`, generated by `tools/manifestref.py gen`; `index.json` lists them:

- `manifest-repeater.json`, `manifest-walter.json` and their `.sig` — valid.
- `manifest-repeater-tampered.json` — one character changed after signing; must fail with the
  repeater's signature. `manifest-repeater-unknown-key.json.sig` — a real signature by a key the
  verifier does not hold; must fail.
- `image-repeater.bin`, `image-walter-ota.bin` — stand-in images whose size and hash the manifests
  record, each with one descriptor at the offset `index.json` gives.
- `image-two-descriptors.bin`, `image-broken-descriptor.bin` — must be refused.
- `manifest-repeater-base.json` and its `.sig` — the repeater's manifest with a `provision` file in
  its image; `base-rak4630.hex` is that file (a small stand-in that meets every rule of §4a).
- `base-*.hex` other than `base-rak4630.hex` — each breaks one rule of §4a (every rule has at least
  one) and must be refused; `index.json` → `provision` gives the rule each one breaks, word for word.
  Every reader of a base (the reference, the Walter, MeshMaster's catalogue) refuses each with the
  same words — against the image's region (`0x00026000`–`0x000D4000` in the fixture) and against the
  `rak4630-single` board's (`0x00026000`–`0x000ED000`) alike.
- `rfc8032_test_1` in `index.json` — the RFC's first test vector, so an Ed25519 implementation is
  proven before it is trusted with ours.
