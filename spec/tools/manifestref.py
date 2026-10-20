#!/usr/bin/env python3
"""Reference implementation of the firmware manifest, its signature and the image descriptor.

FIRMWARE-MANIFEST.md is the specification; this is the same thing as code, and the source of
fixtures/manifest/.

    python3 manifestref.py gen   ../fixtures/manifest      write the fixtures (needs `cryptography`)
    python3 manifestref.py check ../fixtures/manifest      re-derive and verify every one of them
    python3 manifestref.py keygen                          print a new key pair (for the real key)
    python3 manifestref.py sign <private-hex> <manifest.json>      write <manifest.json>.sig
    python3 manifestref.py verify <public-hex> <manifest.json>     check it against its .sig
    python3 manifestref.py descriptor <image.bin>          find and print the descriptor in an image
    python3 manifestref.py base <base.hex> <start> <end>   check a `provision` file against an app_region

The descriptor and manifest halves need only the standard library. Signing and verifying use the
`cryptography` package (Ed25519 as in RFC 8032, no pre-hash, no context).
"""

import base64
import hashlib
import json
import os
import struct
import sys
import zlib

# ----------------------------------------------------------------------------- the descriptor

DESC_MAGIC = b"TWOKFWD1"
DESC_LEN = 96
ROLES = {"unknown": 0, "repeater": 1, "bridge": 2, "scanner": 3, "manager": 16}
ROLE_NAMES = {v: k for k, v in ROLES.items()}


def _fixed(text: str, size: int) -> bytes:
    b = text.encode("ascii")
    if len(b) >= size:
        raise ValueError("%r does not fit %d bytes with its terminator" % (text, size))
    return b + b"\x00" * (size - len(b))


def make_descriptor(role: str, version: str, board: str, band: str, link_major: int, link_minor: int) -> bytes:
    """The 96 bytes a firmware image carries about itself (FIRMWARE-MANIFEST.md section 4)."""
    body = (DESC_MAGIC + struct.pack("<HBBBBH", DESC_LEN, ROLES[role], 0xFF, link_major, link_minor, 0)
            + _fixed(version, 32) + _fixed(board, 24) + _fixed(band, 8) + b"\x00" * 12)
    assert len(body) == DESC_LEN - 4
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def parse_descriptor(blob: bytes) -> dict:
    if len(blob) != DESC_LEN or blob[:8] != DESC_MAGIC:
        raise ValueError("not a descriptor")
    length, role, slot, major, minor, flags = struct.unpack("<HBBBBH", blob[8:16])
    if length != DESC_LEN:
        raise ValueError("descriptor length %d" % length)
    if struct.unpack("<I", blob[92:96])[0] != (zlib.crc32(blob[:92]) & 0xFFFFFFFF):
        raise ValueError("descriptor crc")

    def text(b):
        return b.split(b"\x00", 1)[0].decode("ascii")

    return {"role": ROLE_NAMES.get(role, "role-%d" % role), "slot": slot, "link_major": major, "link_minor": minor,
            "flags": flags, "version": text(blob[16:48]), "board": text(blob[48:72]), "band": text(blob[72:80])}


def find_descriptor(image: bytes):
    """Returns (offset, fields). Exactly one valid descriptor must be in the image, on a 4-byte boundary."""
    found = []
    i = image.find(DESC_MAGIC)
    while i >= 0:
        if i % 4 == 0 and i + DESC_LEN <= len(image):
            try:
                found.append((i, parse_descriptor(image[i:i + DESC_LEN])))
            except ValueError:
                pass
        i = image.find(DESC_MAGIC, i + 1)
    if len(found) != 1:
        raise ValueError("%d valid descriptors in the image, expected exactly one" % len(found))
    return found[0]


# ----------------------------------------------------------------------------- the manifest

SCHEMA = 1
PRODUCTS = ("twinoak-repeater-firmware", "twinoak-walter-firmware")
USES = ("swd", "usb-dfu", "usb-uf2", "ota", "factory", "debug", "provision")


def canonical(manifest: dict) -> bytes:
    """How the release pipeline writes manifest.json: two-space indent, keys in the order given, LF,
    one trailing newline, UTF-8 without a BOM. What is signed is the file's bytes, whatever they are -
    this is only how ours are produced."""
    return (json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def validate(manifest: dict):
    """The checks a consumer makes after the signature has passed. Raises ValueError."""
    def need(cond, what):
        if not cond:
            raise ValueError(what)

    need(manifest.get("schema") == SCHEMA, "schema is not %d" % SCHEMA)
    need(manifest.get("product") in PRODUCTS, "unknown product")
    for key in ("version", "tag", "commit", "built_at"):
        need(isinstance(manifest.get(key), str) and manifest[key], key + " missing")
    need(len(manifest["commit"]) == 40, "commit is not 40 hex characters")
    link = manifest.get("link", {})
    need(isinstance(link.get("major"), int) and isinstance(link.get("minor"), int), "link version missing")
    need(isinstance(manifest.get("images"), list) and manifest["images"], "no images")
    ids = set()
    for image in manifest["images"]:
        for key in ("id", "role", "board", "band", "mcu"):
            need(isinstance(image.get(key), str) and image[key], "image." + key + " missing")
        need(image["id"] not in ids, "duplicate image id " + image["id"])
        ids.add(image["id"])
        need(isinstance(image.get("files"), list) and image["files"], "image has no files")
        for f in image["files"]:
            need(f.get("use") in USES, "unknown use %r" % f.get("use"))
            need(isinstance(f.get("name"), str) and "/" not in f["name"] and "\\" not in f["name"], "bad file name")
            need(isinstance(f.get("size"), int) and f["size"] > 0, "bad size")
            need(isinstance(f.get("sha256"), str) and len(f["sha256"]) == 64, "bad sha256")
            if f["use"] in ("swd", "ota", "factory"):
                need(isinstance(f.get("load_address"), str) and f["load_address"].startswith("0x"), "load_address missing")
            if f["use"] == "swd":
                region = image.get("app_region", {})
                need("start" in region and "end" in region, "an swd image needs app_region")
                start, end, load = int(region["start"], 16), int(region["end"], 16), int(f["load_address"], 16)
                need(start <= load and load + f["size"] <= end, "the image does not fit its app_region")
            need(f.get("format") != "hex" or f["use"] == "provision", "format hex is for a provision file only")
            if f["use"] == "provision":
                need(f.get("format") == "hex", "a provision file is Intel HEX (format hex)")
                need("load_address" not in f, "a provision file has no load_address: its records say where")
                need(image["mcu"] == "nrf52840", "a provision file is for an nRF52840 board")
                region = image.get("app_region", {})
                need("start" in region and "end" in region, "a provision file needs its image's app_region")
        need(sum(1 for f in image["files"] if f.get("use") == "provision") <= 1,
             "more than one provision file in image " + image["id"])


# ----------------------------------------------------------------------------- the base (`provision`)
#
# What goes under a TwinOak application on an nRF52840 board: the MBR and SoftDevice, the bootloader
# and the UICR words the MBR reads (FIRMWARE-MANIFEST.md §4a). Written only by the Walter's
# `rak provision` command (SYSTEM.md S-5a), never by an update. These rules are the whole contract.

FLASH_END = 0x00100000
UICR_START, UICR_END = 0x10001000, 0x10001308   # REGOUT0 at 0x10001304 is UICR's last register
UICR_NRFFW0, UICR_NRFFW1 = 0x10001014, 0x10001018
UICR_APPROTECT = 0x10001208
SD_INFO = 0x00003000              # MBR size 0x1000 + SD_INFO_STRUCT_OFFSET 0x2000 (Nordic nrf_sdm.h)
SD_MAGIC_VALUE = 0x51B1E5DB       # at SD_INFO + 0x04
SD_S140_ID, SD_S140_VERSION = 140, 6001001   # SD_ID at + 0x10, SD_VERSION at + 0x14: S140 6.1.1


def parse_ihex(data: bytes) -> dict:
    """Intel HEX to {address: byte}. Records 00, 01, 02, 04; 03 and 05 (start addresses) are read
    and ignored. A checksum error, a record after the end, no end record, or one address given two
    different bytes is refused. Raises ValueError."""
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError:
        raise ValueError("not ASCII")
    mem, base, ended = {}, 0, False
    for n, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        if ended:
            raise ValueError("line %d: a record after the end record" % n)
        if not line.startswith(":"):
            raise ValueError("line %d: not an Intel HEX record" % n)
        try:
            raw = bytes.fromhex(line[1:])
        except ValueError:
            raise ValueError("line %d: not hex digits" % n)
        if len(raw) < 5 or len(raw) != raw[0] + 5:
            raise ValueError("line %d: record length" % n)
        if sum(raw) & 0xFF:
            raise ValueError("line %d: checksum" % n)
        addr, kind, body = (raw[1] << 8) | raw[2], raw[3], raw[4:-1]
        if kind == 0:
            for i, b in enumerate(body):
                a = base + addr + i
                if mem.get(a, b) != b:
                    raise ValueError("line %d: 0x%08X given two different bytes" % (n, a))
                mem[a] = b
        elif kind == 1:
            ended = True
        elif kind == 2 and len(body) == 2:
            base = ((body[0] << 8) | body[1]) << 4
        elif kind == 4 and len(body) == 2:
            base = ((body[0] << 8) | body[1]) << 16
        elif kind in (3, 5):
            pass
        else:
            raise ValueError("line %d: record type %d" % (n, kind))
    if not ended:
        raise ValueError("no end record (a truncated file?)")
    if not mem:
        raise ValueError("no data")
    return mem


def _word(mem: dict, addr: int):
    if all(addr + i in mem for i in range(4)):
        return mem[addr] | mem[addr + 1] << 8 | mem[addr + 2] << 16 | mem[addr + 3] << 24
    return None


def validate_provision(data: bytes, app_start: int, app_end: int) -> dict:
    """The base rules of FIRMWARE-MANIFEST.md §4a, in their order. Returns what the base is;
    raises ValueError with the rule that failed."""
    def need(cond, what):
        if not cond:
            raise ValueError(what)

    mem = parse_ihex(data)
    for a in mem:
        need(a < FLASH_END or UICR_START <= a < UICR_END, "0x%08X is neither flash nor UICR" % a)
    need(all(not (UICR_APPROTECT <= a < UICR_APPROTECT + 4) for a in mem),
         "the file writes UICR.APPROTECT: the Walter decides that word by the chip's revision")
    need(0 in mem, "nothing at 0x00000000: no MBR (a bootloader-only file?)")
    need(_word(mem, SD_INFO + 0x04) == SD_MAGIC_VALUE, "no SoftDevice information structure at 0x00003000")
    sd_end = _word(mem, SD_INFO + 0x08)
    need(sd_end == app_start, "the SoftDevice ends at %s, the application starts at 0x%08X"
         % ("0x%08X" % sd_end if sd_end is not None else "?", app_start))
    sd_id, sd_version = _word(mem, SD_INFO + 0x10), _word(mem, SD_INFO + 0x14)
    need((sd_id, sd_version) == (SD_S140_ID, SD_S140_VERSION),
         "the SoftDevice is not S140 6.1.1 (id %s, version %s)" % (sd_id, sd_version))
    boot = _word(mem, UICR_NRFFW0)
    need(boot is not None, "no UICR.NRFFW[0]: the MBR would never start a bootloader")
    need(boot % 0x1000 == 0 and app_end <= boot < FLASH_END and boot in mem,
         "UICR.NRFFW[0] = 0x%08X: not a page at or above the application region's end that the file fills" % boot)
    for a in mem:
        if a < FLASH_END:
            need(a < app_start or a >= boot,
                 "0x%08X lies between the SoftDevice and the bootloader: the application and the file system are not the base's" % a)
    flash_bytes = sum(1 for a in mem if a < FLASH_END)
    uicr_words = sorted({a & ~3 for a in mem if a >= UICR_START})
    return {"bootloader": "0x%08X" % boot, "softdevice": "S140 6.1.1", "flash_bytes": flash_bytes,
            "uicr_words": ["0x%08X" % w for w in uicr_words],
            "mbr_params": "0x%08X" % _word(mem, UICR_NRFFW1) if _word(mem, UICR_NRFFW1) is not None else None}


def ihex(chunks) -> bytes:
    """[(address, bytes)] to Intel HEX: 16 data bytes a record, 04 records at every 64 KB, an end
    record. How a fixture is written; any valid Intel HEX is accepted."""
    lines, upper = [], None

    def record(addr, kind, body):
        raw = bytes([len(body), (addr >> 8) & 0xFF, addr & 0xFF, kind]) + body
        return ":" + (raw + bytes([(-sum(raw)) & 0xFF])).hex().upper()

    for start, blob in chunks:
        for off in range(0, len(blob), 16):
            a = start + off
            if a >> 16 != upper:
                upper = a >> 16
                lines.append(record(0, 4, bytes([upper >> 8, upper & 0xFF])))
            lines.append(record(a & 0xFFFF, 0, blob[off:off + 16]))
    lines.append(record(0, 1, b""))
    return ("\n".join(lines) + "\n").encode("ascii")


def file_entry(use: str, name: str, data: bytes, load_address=None) -> dict:
    entry = {"use": use, "name": name, "format": name.rsplit(".", 1)[-1], "size": len(data),
             "sha256": hashlib.sha256(data).hexdigest()}
    if load_address is not None:
        entry["load_address"] = "0x%08X" % load_address
    return entry


# ----------------------------------------------------------------------------- the signature


def key_id(public: bytes) -> str:
    """Sixteen hex characters: the first eight bytes of SHA-256 over the raw 32-byte public key."""
    return hashlib.sha256(public).hexdigest()[:16]


def _ed():
    from cryptography.hazmat.primitives.asymmetric import ed25519
    return ed25519


def sign(private: bytes, message: bytes) -> str:
    ed = _ed()
    key = ed.Ed25519PrivateKey.from_private_bytes(private)
    from cryptography.hazmat.primitives import serialization
    public = key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    return "ed25519 %s %s\n" % (key_id(public), base64.b64encode(key.sign(message)).decode("ascii"))


def verify(publics, message: bytes, sig_line: str) -> bool:
    """publics: an iterable of raw public keys the verifier trusts. The line names the key it used."""
    parts = sig_line.strip().split(" ")
    if len(parts) != 3 or parts[0] != "ed25519":
        return False
    try:
        signature = base64.b64decode(parts[2], validate=True)
    except Exception:
        return False
    if len(signature) != 64:
        return False
    ed = _ed()
    for public in publics:
        if key_id(public) == parts[1]:
            try:
                ed.Ed25519PublicKey.from_public_bytes(public).verify(signature, message)
                return True
            except Exception:
                return False
    return False


# ----------------------------------------------------------------------------- the fixtures

# A key pair for tests and for nothing else. It is public on purpose: every repository that tests
# signature checking needs the same one. A firmware build that trusts it is a test build.
TEST_PRIVATE = bytes.fromhex("7465737420746573742074657374207465737420746573742074657374207465")  # b"test test test ..."


def fake_image(descriptor: bytes, size: int, where: int, seed: int) -> bytes:
    """Deterministic filler with the descriptor at `where` - a stand-in for a firmware image."""
    out = bytearray()
    state = seed
    while len(out) < size:
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        out += struct.pack("<I", state)
    out = out[:size]
    out[where:where + DESC_LEN] = descriptor
    return bytes(out)


def build_fixtures(out_dir: str):
    from cryptography.hazmat.primitives import serialization
    ed = _ed()
    public = ed.Ed25519PrivateKey.from_private_bytes(TEST_PRIVATE).public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)

    def write(name, data: bytes):
        with open(os.path.join(out_dir, name), "wb") as fh:
            fh.write(data)

    write("TEST-KEY.txt", (
        "A key pair for tests and for nothing else. It is public on purpose.\n"
        "A firmware build that trusts this key is a test build and must never be released.\n\n"
        "private %s\npublic  %s\nkey id  %s\n" % (TEST_PRIVATE.hex(), public.hex(), key_id(public))).encode())

    # the repeater's release: one role, one board, three files
    rep_desc = make_descriptor("repeater", "0.3.0+abc1234", "rak4630-single", "868", 1, 0)
    rep_bin = fake_image(rep_desc, 4096, 0x400, 1)
    rep_zip = fake_image(rep_desc, 2048, 0x100, 2)
    rep_uf2 = fake_image(rep_desc, 8192, 0x800, 3)
    repeater = {
        "schema": SCHEMA,
        "product": "twinoak-repeater-firmware",
        "version": "0.3.0+abc1234",
        "tag": "v0.3.0",
        "commit": "abc1234" + "0" * 33,
        "built_at": "2026-10-12T08:00:00Z",
        "link": {"major": 1, "minor": 0},
        "upstream": {"name": "MeshCore", "tag": "repeater-v1.17.1", "commit": "d92964352441e53b93e8667b802e04f6e072b39e"},
        "images": [{
            "id": "repeater.rak4630-single.868",
            "role": "repeater", "board": "rak4630-single", "band": "868", "mcu": "nrf52840",
            "app_region": {"start": "0x00026000", "end": "0x000D4000"},
            "requires": {"walter": "0.3.0", "link_major": 1},
            "files": [
                file_entry("swd", "twinoak-repeater-rak4630-single-868-v0.3.0.bin", rep_bin, 0x26000),
                file_entry("usb-dfu", "twinoak-repeater-rak4630-single-868-v0.3.0.zip", rep_zip),
                file_entry("usb-uf2", "twinoak-repeater-rak4630-single-868-v0.3.0.uf2", rep_uf2),
            ],
        }],
    }
    wal_desc = make_descriptor("manager", "0.3.0+def5678", "walter", "lte-m", 1, 0)
    wal_ota = fake_image(wal_desc, 4096, 0x20, 4)
    wal_factory = fake_image(wal_desc, 8192, 0x1020, 5)
    walter = {
        "schema": SCHEMA,
        "product": "twinoak-walter-firmware",
        "version": "0.3.0+def5678",
        "tag": "v0.3.0",
        "commit": "def5678" + "0" * 33,
        "built_at": "2026-10-12T08:05:00Z",
        "link": {"major": 1, "minor": 0},
        "images": [{
            "id": "manager.walter.lte-m",
            "role": "manager", "board": "walter", "band": "lte-m", "mcu": "esp32s3",
            "requires": {"link_major": 1},
            "files": [
                file_entry("ota", "twinoak-walter-v0.3.0.bin", wal_ota, 0x20000),
                file_entry("factory", "twinoak-walter-v0.3.0-factory.bin", wal_factory, 0x0),
            ],
        }],
    }
    for name, manifest in (("repeater", repeater), ("walter", walter)):
        validate(manifest)
        data = canonical(manifest)
        write("manifest-%s.json" % name, data)
        write("manifest-%s.json.sig" % name, sign(TEST_PRIVATE, data).encode("ascii"))

    # the same manifest with one character changed: the old signature must not pass
    tampered = canonical(repeater).replace(b'"0x000D4000"', b'"0x000F4000"')
    write("manifest-repeater-tampered.json", tampered)
    # a signature by a key the verifier does not hold
    other = hashlib.sha256(b"some other key").digest()
    write("manifest-repeater-unknown-key.json.sig", sign(other, canonical(repeater)).encode("ascii"))

    # the base under the repeater (FIRMWARE-MANIFEST.md §4a): a stand-in, small, every rule met
    def words(*vals):
        return b"".join(struct.pack("<I", v) for v in vals)

    def base_chunks(sd_end=0x26000, nrffw0=0xF4000):
        filler = fake_image(b"", 256, 0, 7)
        return [
            (0x00000000, filler),                                             # the MBR
            (0x00001000, filler),                                             # the SoftDevice ...
            (0x00003000, words(0, SD_MAGIC_VALUE, sd_end, 0x00B6, SD_S140_ID, SD_S140_VERSION)),
            (0x000F4000, filler),                                             # the bootloader
            (0x10001014, words(nrffw0, 0x000FE000)),                          # NRFFW[0], NRFFW[1]
        ]

    base = ihex(base_chunks())
    write("base-rak4630.hex", base)
    refused = [
        ("base-bootloader-only.hex", ihex([c for c in base_chunks() if c[0] >= 0xF4000]),
         "nothing at 0x00000000: no MBR (a bootloader-only file?)"),
        ("base-sets-approtect.hex", ihex(base_chunks() + [(UICR_APPROTECT, words(0x5A))]),
         "the file writes UICR.APPROTECT: the Walter decides that word by the chip's revision"),
        ("base-writes-the-application.hex", ihex(base_chunks() + [(0x00026000, words(0))]),
         "0x00026000 lies between the SoftDevice and the bootloader: the application and the file system are not the base's"),
        ("base-other-softdevice.hex", ihex(base_chunks(sd_end=0x27000)),
         "the SoftDevice ends at 0x00027000, the application starts at 0x00026000"),
        ("base-no-bootloader-address.hex", ihex([c for c in base_chunks() if c[0] != 0x10001014]),
         "no UICR.NRFFW[0]: the MBR would never start a bootloader"),
    ]
    # rule 1, record by record: a stand-in's lines, each broken one way
    good_lines = base.decode("ascii").splitlines()
    bad_sum = list(good_lines)
    bad_sum[2] = bad_sum[2][:-2] + "%02X" % ((int(bad_sum[2][-2:], 16) + 1) & 0xFF)
    twice = list(good_lines)
    twice[-1:-1] = ihex([(0x00000000, b"\x00")]).decode("ascii").splitlines()[:2]  # back to 0, one byte
    refused += [
        ("base-bad-checksum.hex", ("\n".join(bad_sum) + "\n").encode("ascii"), "line 3: checksum"),
        ("base-no-end-record.hex", ("\n".join(good_lines[:-1]) + "\n").encode("ascii"),
         "no end record (a truncated file?)"),
        ("base-after-the-end.hex", ("\n".join(good_lines + [good_lines[1]]) + "\n").encode("ascii"),
         "line %d: a record after the end record" % (len(good_lines) + 1)),
        ("base-one-address-two-bytes.hex", ("\n".join(twice) + "\n").encode("ascii"),
         "line %d: 0x00000000 given two different bytes" % (len(good_lines) + 1)),
        ("base-outside-uicr.hex", ihex(base_chunks() + [(0x10001400, words(0))]),
         "0x10001400 is neither flash nor UICR"),
        ("base-other-softdevice-version.hex",
         ihex([c if c[0] != 0x3000 else (0x3000, words(0, SD_MAGIC_VALUE, 0x26000, 0x0123, SD_S140_ID, 7002000))
               for c in base_chunks()]),
         "the SoftDevice is not S140 6.1.1 (id 140, version 7002000)"),
        ("base-unaligned-bootloader.hex",
         ihex([c for c in base_chunks(nrffw0=0xF4100) if c[0] != 0xF4000] + [(0x000F4100, fake_image(b"", 256, 0, 8))]),
         "UICR.NRFFW[0] = 0x000F4100: not a page at or above the application region's end that the file fills"),
        ("base-bootloader-in-the-application.hex",
         ihex([c for c in base_chunks(nrffw0=0xD0000) if c[0] != 0xF4000] + [(0x000D0000, fake_image(b"", 256, 0, 9))]),
         "UICR.NRFFW[0] = 0x000D0000: not a page at or above the application region's end that the file fills"),
        ("base-writes-the-file-system.hex", ihex(base_chunks() + [(0x000F0000, words(0))]),
         "0x000F0000 lies between the SoftDevice and the bootloader: the application and the file system are not the base's"),
    ]
    for name, data, _ in refused:
        write(name, data)
    with_base = json.loads(canonical(repeater))
    with_base["images"][0]["files"].append(file_entry("provision", "twinoak-base-rak4630-s140-6.1.1.hex", base))
    validate(with_base)
    write("manifest-repeater-base.json", canonical(with_base))
    write("manifest-repeater-base.json.sig", sign(TEST_PRIVATE, canonical(with_base)).encode("ascii"))

    write("image-repeater.bin", rep_bin)
    write("image-walter-ota.bin", wal_ota)
    two = bytearray(rep_bin)
    two[0x800:0x800 + DESC_LEN] = rep_desc
    write("image-two-descriptors.bin", bytes(two))
    broken = bytearray(rep_bin)
    broken[0x400 + 20] ^= 0x01
    write("image-broken-descriptor.bin", bytes(broken))

    index = {
        "about": "Generated by software/spec/tools/manifestref.py from FIRMWARE-MANIFEST.md. Never edited by hand.",
        "test_public_key": public.hex(),
        "test_key_id": key_id(public),
        "rfc8032_test_1": {
            "about": "RFC 8032 section 7.1, TEST 1: the empty message. An Ed25519 verifier must accept it.",
            "public": "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
            "message": "",
            "signature": "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                         "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
        },
        "signatures": [
            {"manifest": "manifest-repeater.json", "sig": "manifest-repeater.json.sig", "valid": True},
            {"manifest": "manifest-walter.json", "sig": "manifest-walter.json.sig", "valid": True},
            {"manifest": "manifest-repeater-tampered.json", "sig": "manifest-repeater.json.sig", "valid": False,
             "why": "one character of the manifest changed after it was signed"},
            {"manifest": "manifest-repeater.json", "sig": "manifest-repeater-unknown-key.json.sig", "valid": False,
             "why": "signed by a key the verifier does not hold"},
            {"manifest": "manifest-walter.json", "sig": "manifest-repeater.json.sig", "valid": False,
             "why": "another manifest's signature"},
            {"manifest": "manifest-repeater-base.json", "sig": "manifest-repeater-base.json.sig", "valid": True},
        ],
        "descriptors": [
            {"image": "image-repeater.bin", "valid": True, "offset": 0x400, "hex": rep_desc.hex(),
             "fields": parse_descriptor(rep_desc)},
            {"image": "image-walter-ota.bin", "valid": True, "offset": 0x20, "hex": wal_desc.hex(),
             "fields": parse_descriptor(wal_desc)},
            {"image": "image-two-descriptors.bin", "valid": False, "why": "two valid descriptors"},
            {"image": "image-broken-descriptor.bin", "valid": False, "why": "the only descriptor fails its crc"},
        ],
        "files": [
            {"manifest": "manifest-repeater.json", "name": repeater["images"][0]["files"][0]["name"],
             "bytes_of": "image-repeater.bin"},
            {"manifest": "manifest-walter.json", "name": walter["images"][0]["files"][0]["name"],
             "bytes_of": "image-walter-ota.bin"},
            {"manifest": "manifest-repeater-base.json", "name": with_base["images"][0]["files"][3]["name"],
             "bytes_of": "base-rak4630.hex"},
        ],
        "provision": {
            "about": "FIRMWARE-MANIFEST.md 4a, checked against the image's app_region of manifest-repeater-base.json; every base gives the same result against the rak4630-single board region, 0x00026000-0x000ED000.",
            "app_region": with_base["images"][0]["app_region"],
            "bases": [{"file": "base-rak4630.hex", "valid": True,
                       "summary": validate_provision(base, 0x26000, 0xD4000)}]
                     + [{"file": name, "valid": False, "why": why} for name, _, why in refused],
        },
    }
    write("index.json", (json.dumps(index, indent=1) + "\n").encode("ascii"))


def check(out_dir: str) -> int:
    def read(name):
        with open(os.path.join(out_dir, name), "rb") as fh:
            return fh.read()

    index = json.loads(read("index.json"))
    public = bytes.fromhex(index["test_public_key"])
    assert key_id(public) == index["test_key_id"]
    n = 0
    rfc = index["rfc8032_test_1"]
    _ed().Ed25519PublicKey.from_public_bytes(bytes.fromhex(rfc["public"])).verify(
        bytes.fromhex(rfc["signature"]), bytes.fromhex(rfc["message"]))
    n += 1
    for s in index["signatures"]:
        got = verify([public], read(s["manifest"]), read(s["sig"]).decode("ascii"))
        assert got == s["valid"], s
        n += 1
    for name in ("manifest-repeater.json", "manifest-walter.json", "manifest-repeater-base.json"):
        manifest = json.loads(read(name))
        validate(manifest)
        assert canonical(manifest) == read(name), name + " is not in canonical form"
        n += 1
    for d in index["descriptors"]:
        try:
            offset, fields = find_descriptor(read(d["image"]))
            ok = True
        except ValueError:
            ok = False
        assert ok == d["valid"], d
        if ok:
            assert offset == d["offset"] and fields == d["fields"], d
            assert parse_descriptor(bytes.fromhex(d["hex"])) == d["fields"]
        n += 1
    for f in index["files"]:
        manifest = json.loads(read(f["manifest"]))
        entry = [e for image in manifest["images"] for e in image["files"] if e["name"] == f["name"]][0]
        data = read(f["bytes_of"])
        assert entry["size"] == len(data) and entry["sha256"] == hashlib.sha256(data).hexdigest(), f
        n += 1
    region = index["provision"]["app_region"]
    start, end = int(region["start"], 16), int(region["end"], 16)
    for b in index["provision"]["bases"]:
        try:
            got, why = validate_provision(read(b["file"]), start, end), None
        except ValueError as e:
            got, why = None, str(e)
        assert (got is not None) == b["valid"], (b, why)
        if b["valid"]:
            assert got == b["summary"], (b, got)
        else:
            assert why == b["why"], (b, why)
        n += 1
    return n


def main(argv):
    if len(argv) == 3 and argv[1] == "gen":
        os.makedirs(argv[2], exist_ok=True)
        build_fixtures(argv[2])
        print("wrote %s: %d checks pass" % (argv[2], check(argv[2])))
        return 0
    if len(argv) == 3 and argv[1] == "check":
        print("%s: %d checks pass" % (argv[2], check(argv[2])))
        return 0
    if len(argv) == 2 and argv[1] == "keygen":
        from cryptography.hazmat.primitives import serialization
        key = _ed().Ed25519PrivateKey.generate()
        private = key.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
                                    serialization.NoEncryption())
        public = key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        print("private %s   <- a secret: the release workflow's, and nobody else's" % private.hex())
        print("public  %s" % public.hex())
        print("key id  %s" % key_id(public))
        return 0
    if len(argv) == 4 and argv[1] == "sign":
        with open(argv[3], "rb") as fh:
            line = sign(bytes.fromhex(argv[2]), fh.read())
        with open(argv[3] + ".sig", "w", encoding="ascii", newline="\n") as fh:
            fh.write(line)
        print(line.strip())
        return 0
    if len(argv) == 4 and argv[1] == "verify":
        with open(argv[3], "rb") as fh:
            message = fh.read()
        with open(argv[3] + ".sig", "r", encoding="ascii") as fh:
            ok = verify([bytes.fromhex(argv[2])], message, fh.read())
        print("valid" if ok else "NOT VALID")
        return 0 if ok else 1
    if len(argv) == 5 and argv[1] == "base":
        with open(argv[2], "rb") as fh:
            summary = validate_provision(fh.read(), int(argv[3], 16), int(argv[4], 16))
        print(json.dumps(summary, indent=1))
        return 0
    if len(argv) == 3 and argv[1] == "descriptor":
        with open(argv[2], "rb") as fh:
            offset, fields = find_descriptor(fh.read())
        print("at 0x%X: %s" % (offset, json.dumps(fields)))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
