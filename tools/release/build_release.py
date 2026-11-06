#!/usr/bin/env python3
"""Writes a release of this firmware: the images, manifest.json and SHA256SUMS.

    python tools/release/build_release.py --commit <40 hex>
        --build-dir .pio/build/repeater_rak4630_single [--build-dir ...]
        [--version X.Y.Z+sha7] [--tag vX.Y.Z] [--out out/release] [--clean]
        [--require-descriptor]

spec/FIRMWARE-MANIFEST.md is the specification and spec/tools/manifestref.py is that document as
code: the canonical form, the validation, the file entries and the descriptor are *imported* from
it and not re-implemented here. What is here is the part only this repository can do - turning one
PlatformIO build into the files a release carries:

  twinoak-<role>-<board>-<band>-vX.Y.Z.bin   the raw application binary for the Walter's SWD
                                             flasher (use: swd, with its load_address)
  twinoak-<role>-<board>-<band>-vX.Y.Z.zip   the nrfutil DFU package, for a laptop on the USB-C
  twinoak-<role>-<board>-<band>-vX.Y.Z.uf2   for the bootloader's drag-and-drop drive
  twinoak-base-<family>-<ver>-s140-6.1.1.hex the base under the application - MBR, SoftDevice,
                                             bootloader - for the Walter's one-time `rak provision`
                                             (use: provision), the same file under every image of
                                             the board family; vendored, never built here
                                             (boards/base/<family>/, tools/base/check.py)
  THIRD-PARTY-NOTICES                        the notices the base's licences ask for
  manifest.json                              the canonical manifest, validated before it is written
  SHA256SUMS                                 over every file above, the manifest included

Nothing here is typed in that the build already knows. tools/pio/buildinfo.py leaves
buildinfo.json and a byte copy of the linker script in each build directory, so:

  * the role, the board, the band, the MCU, the version in the image and the MeshCore pin are
    read from buildinfo.json (and a --version given on the command line must agree with it: a
    manifest may not claim a version the image does not report);
  * app_region comes from the linker script the build actually used - start is where the
    application begins, end is the first byte above it, which is where the file system holding
    the node's identity begins (docs/ARCHITECTURE.md section 9). Never a constant in this file;
  * the raw binary is cut from the linked image (the Intel HEX the build leaves, which is what a
    flasher consumes), so its load_address is the lowest address the image occupies. A gap in
    the image, or a byte outside the application region, is an error and no release is written.

The signature is a separate step: tools/release/sign_manifest.py, which never sees a command line
with a key on it. SHA256SUMS therefore covers the manifest but not manifest.json.sig, which does
not exist yet when this tool runs - the signature covers the manifest, and the manifest covers
every other file.
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(REPO, "spec", "tools"))
sys.path.insert(0, os.path.join(REPO, "tools", "base"))

import manifestref  # noqa: E402  (after sys.path; spec/ is a copy and is never edited)
import check as base_check  # noqa: E402  (tools/base/check.py: the base under the application)

PRODUCT = "twinoak-repeater-firmware"
NOTICES = "THIRD-PARTY-NOTICES"


class ReleaseError(Exception):
    """Something that must stop a release. Printed as one line; nothing is published."""


# ------------------------------------------------------------------ the linker script's numbers

_LD_TOKEN = re.compile(r"0[xX][0-9a-fA-F]+[KMGkmg]?|\d+[KMGkmg]?|[-+*/()]|\s+")
_LD_SCALE = {"k": 1024, "m": 1024 ** 2, "g": 1024 ** 3}


def _ld_tokens(text):
    out = []
    i = 0
    while i < len(text):
        m = _LD_TOKEN.match(text, i)
        if not m:
            raise ReleaseError("cannot read %r as a linker-script expression" % text.strip())
        token = m.group(0)
        i = m.end()
        if token.strip():
            out.append(token)
    return out


def ld_expression(text):
    """As much of GNU ld's arithmetic as a MEMORY region needs: integers (decimal or 0x hex, with
    ld's K/M/G multipliers), + - * / and parentheses.

    A parser and not an eval: a linker script is read here, never run.
    """
    tokens = _ld_tokens(text)
    pos = [0]

    def peek():
        return tokens[pos[0]] if pos[0] < len(tokens) else None

    def take():
        token = peek()
        if token is None:
            raise ReleaseError("the expression %r ends too soon" % text.strip())
        pos[0] += 1
        return token

    def primary():
        token = take()
        if token == "(":
            value = expression()
            if take() != ")":
                raise ReleaseError("unbalanced () in %r" % text.strip())
            return value
        if token == "-":
            return -primary()
        if token == "+":
            return primary()
        scale = 1
        if token[-1].lower() in _LD_SCALE:
            scale = _LD_SCALE[token[-1].lower()]
            token = token[:-1]
        try:
            return int(token, 16 if token[:2].lower() == "0x" else 10) * scale
        except ValueError:
            raise ReleaseError("%r is not a number in %r" % (token, text.strip()))

    def product():
        value = primary()
        while peek() in ("*", "/"):
            op = take()
            right = primary()
            if op == "*":
                value *= right
            else:
                if right == 0:
                    raise ReleaseError("division by zero in %r" % text.strip())
                value //= right
        return value

    def expression():
        value = product()
        while peek() in ("+", "-"):
            value = value + product() if take() == "+" else value - product()
        return value

    value = expression()
    if pos[0] != len(tokens):
        raise ReleaseError("trailing %r in the expression %r" % (peek(), text.strip()))
    return value


def _strip_ld_comments(text):
    """/* ... */ replaced by spaces, so line numbers still mean what the file's own do."""
    out = []
    i = 0
    while True:
        start = text.find("/*", i)
        if start < 0:
            out.append(text[i:])
            return "".join(out)
        end = text.find("*/", start + 2)
        if end < 0:
            end = len(text) - 2
        out.append(text[i:start])
        out.append("".join(ch if ch == "\n" else " " for ch in text[start:end + 2]))
        i = end + 2


_LD_REGION = re.compile(
    r"(?P<name>[A-Za-z_][A-Za-z_0-9]*)\s*(?:\([^)]*\))?\s*:\s*"
    r"(?:ORIGIN|org)\s*=\s*(?P<origin>[^,]+),\s*(?:LENGTH|len)\s*=\s*(?P<length>[^\n}]+)"
)


class AppRegion(object):
    """The flash an update may write on this board, and the line of the script it was read from."""

    def __init__(self, start, end, source, lineno, line):
        self.start = start
        self.end = end
        self.source = source
        self.lineno = lineno
        self.line = line

    def where(self):
        return "%s:%d  %s" % (self.source, self.lineno, self.line)

    def __repr__(self):
        return "0x%08X..0x%08X (%s)" % (self.start, self.end, self.where())

    def as_manifest(self):
        return {"start": "0x%08X" % self.start, "end": "0x%08X" % self.end}


def read_app_region(path, name=None):
    """The application region, from the FLASH region of a linker script's MEMORY block.

    start is the application's ORIGIN; end is ORIGIN + LENGTH, the first byte above it - on this
    board the start of the file system that holds the node's identity and settings, which no
    update may touch (docs/ARCHITECTURE.md section 9, spec/FIRMWARE-MANIFEST.md section 2).
    """
    name = name or os.path.basename(path)
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            raw = fh.read()
    except IOError as exc:
        raise ReleaseError("the build's linker script is not readable: %s" % exc)
    text = _strip_ld_comments(raw)
    block = re.search(r"\bMEMORY\b\s*\{", text)
    if not block:
        raise ReleaseError("%s has no MEMORY block: the application region cannot be read" % name)
    depth = 0
    close = None
    for i in range(block.end() - 1, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                close = i
                break
    if close is None:
        raise ReleaseError("%s has an unterminated MEMORY block" % name)
    found = {}
    for m in _LD_REGION.finditer(text[block.end():close]):
        found[m.group("name")] = (m, block.end() + m.start())
    if "FLASH" not in found:
        raise ReleaseError(
            "%s has no FLASH region in its MEMORY block (it has %s); the application region is "
            "read from that region and is never a constant here"
            % (name, ", ".join(sorted(found)) or "none"))
    m, offset = found["FLASH"]
    start = ld_expression(m.group("origin"))
    length = ld_expression(m.group("length"))
    if length <= 0:
        raise ReleaseError("%s gives the FLASH region a length of %d" % (name, length))
    lineno = raw.count("\n", 0, offset) + 1
    return AppRegion(start, start + length, name, lineno, raw.splitlines()[lineno - 1].strip())


# ------------------------------------------------------------------ the linked image

def parse_intel_hex(text, what="the image"):
    """The contiguous runs of an Intel HEX file, as [(address, bytes)] in address order.

    Every record's checksum is checked and the end-of-file record must be there: a truncated .hex
    must not become a short image that a flasher writes happily.
    """
    data = {}
    base = 0
    saw_eof = False
    for lineno, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        if saw_eof:
            raise ReleaseError("%s has records after its end-of-file record (line %d)" % (what, lineno))
        if not line.startswith(":"):
            raise ReleaseError("%s line %d is not an Intel HEX record" % (what, lineno))
        try:
            raw = bytes.fromhex(line[1:])
        except ValueError:
            raise ReleaseError("%s line %d is not hexadecimal" % (what, lineno))
        if len(raw) < 5 or len(raw) != raw[0] + 5:
            raise ReleaseError("%s line %d has the wrong length" % (what, lineno))
        if sum(raw) & 0xFF:
            raise ReleaseError("%s line %d fails its checksum" % (what, lineno))
        offset = (raw[1] << 8) | raw[2]
        rtype = raw[3]
        payload = raw[4:4 + raw[0]]
        if rtype == 0x00:
            for i, byte in enumerate(payload):
                address = base + offset + i
                if address in data:
                    raise ReleaseError("%s writes 0x%08X twice (line %d)" % (what, address, lineno))
                data[address] = byte
        elif rtype == 0x01:
            saw_eof = True
        elif rtype == 0x02:          # extended segment address: the segment, times sixteen
            base = ((payload[0] << 8) | payload[1]) << 4
        elif rtype == 0x04:          # extended linear address: the high half of the address
            base = ((payload[0] << 8) | payload[1]) << 16
        elif rtype in (0x03, 0x05):  # a start address, for a host that runs the file; not ours
            pass
        else:
            raise ReleaseError("%s line %d has record type %d" % (what, lineno, rtype))
    if not saw_eof:
        raise ReleaseError("%s has no end-of-file record: it is truncated" % what)
    runs = []
    for address in sorted(data):
        if runs and address == runs[-1][0] + len(runs[-1][1]):
            runs[-1][1].append(data[address])
        else:
            runs.append((address, bytearray([data[address]])))
    return [(address, bytes(blob)) for address, blob in runs]


def application_binary(runs, region, what="the image"):
    """The one contiguous span the image occupies, and the address it is written at.

    A gap is refused: the raw binary is written at one load_address, and the bytes of a gap would
    be whatever the flash held before. A byte outside the application region is refused too - a
    release may not ask for a write to the bootloader, the SoftDevice, the file system or UICR
    (spec/FIRMWARE-MANIFEST.md section 2).
    """
    if not runs:
        raise ReleaseError("%s holds no data" % what)
    if len(runs) > 1:
        gaps = ["0x%08X..0x%08X (%d bytes)" % (a + len(b), runs[i + 1][0], runs[i + 1][0] - a - len(b))
                for i, (a, b) in enumerate(runs[:-1])]
        raise ReleaseError(
            "%s is not one contiguous span: %d runs, gaps at %s. The raw binary is written at one "
            "load address, so a gap would leave whatever the flash held before it."
            % (what, len(runs), ", ".join(gaps)))
    start, blob = runs[0]
    end = start + len(blob)
    if start < region.start or end > region.end:
        raise ReleaseError(
            "%s occupies 0x%08X..0x%08X, outside the application region 0x%08X..0x%08X (%s). An "
            "update writes the application region and nothing else."
            % (what, start, end, region.start, region.end, region.where()))
    return start, blob


# ------------------------------------------------------------------ the manifest

def read_build_info(build_dir):
    """What the build knew about itself (tools/pio/buildinfo.py)."""
    path = os.path.join(build_dir, "buildinfo.json")
    if not os.path.isfile(path):
        raise ReleaseError(
            "%s has no buildinfo.json: build that environment first (tools/pio/buildinfo.py "
            "writes it on every build)" % build_dir)
    with open(path, "r", encoding="utf-8") as fh:
        info = json.load(fh)
    for key in ("env", "progname", "role", "board", "band", "mcu", "version", "upstream",
                "ldscript_name", "ldscript_copy"):
        if not info.get(key):
            raise ReleaseError("%s is missing %r" % (path, key))
    return info


LINK_NUMBERS = os.path.join("src", "common", "link", "codec", "Numbers.h")
_LINK_CONSTANT = re.compile(
    r"^constexpr\s+uint8_t\s+kProto(Major|Minor)\s*=\s*(\d+)\s*;", re.M)


def link_version(repo=REPO):
    """The box-link version this *firmware* speaks: `kProtoMajor` and `kProtoMinor` of
    src/common/link/codec/Numbers.h.

    The manifest's `link` is what the firmware speaks, not what the specification's newest
    fixture file happens to be called, and since p3.8-01 the firmware has a constant of its own -
    which is also what the image descriptor's `link_major`/`link_minor` are built from
    (src/common/fwdesc/ImageDescriptor.cpp). Reading that one place is what makes
    check_descriptor() below a real check instead of two copies of the same number: an image
    built at another minor than the manifest claims fails the release.

    Until p3.8-01 this read spec/fixtures/link/link-v1.json's `link` field, which stopped being
    the answer the moment 1.1's fixtures arrived beside 1.0's: that file is 1.0's and never
    changes.
    """
    path = os.path.join(repo, LINK_NUMBERS)
    with open(path, "r", encoding="utf-8") as fh:
        found = dict((m.group(1), int(m.group(2))) for m in _LINK_CONSTANT.finditer(fh.read()))
    for key in ("Major", "Minor"):
        if key not in found:
            raise ReleaseError("%s has no `constexpr uint8_t kProto%s = <n>;` line: the box-link "
                               "version a release claims is read from there and nowhere else"
                               % (LINK_NUMBERS, key))
    return {"major": found["Major"], "minor": found["Minor"]}


def name_for(info, version, extension):
    """twinoak-<role>-<board>-<band>-vX.Y.Z.<ext>, without the version's build metadata: that is
    inside the image and in the manifest, and a file name with a + in it is a nuisance."""
    return "twinoak-%s-%s-%s-v%s.%s" % (info["role"], info["board"], info["band"],
                                        version.split("+", 1)[0], extension)


def write_file(out_dir, name, blob):
    with open(os.path.join(out_dir, name), "wb") as fh:
        fh.write(blob)


def check_descriptor(image, application, version, link, required, log):
    """The 96 bytes an image says about itself (spec/FIRMWARE-MANIFEST.md section 4), held against
    the manifest entry.

    --require-descriptor turns the check on. Every image this repository builds has carried one
    since phase 3's task 01 (src/common/fwdesc/), and both the release workflow and the local dry
    run of tools/verify.ps1 pass the flag; the switch stays because a build directory assembled
    by hand, or an older artifact, is still a thing this tool can be pointed at.
    """
    if not required:
        log("%s: no descriptor yet (--require-descriptor turns the check on)" % image["id"])
        return
    try:
        offset, fields = manifestref.find_descriptor(application)
    except ValueError as exc:
        raise ReleaseError("%s: %s (spec/FIRMWARE-MANIFEST.md section 4)" % (image["id"], exc))
    expected = {"role": image["role"], "board": image["board"], "band": image["band"],
                "version": version, "link_major": link["major"], "link_minor": link["minor"]}
    wrong = ["%s is %r, the manifest says %r" % (key, fields.get(key), want)
             for key, want in sorted(expected.items()) if fields.get(key) != want]
    if wrong:
        raise ReleaseError("%s: the descriptor at 0x%X disagrees with the manifest: %s"
                           % (image["id"], offset, "; ".join(wrong)))
    log("%s: descriptor at 0x%X agrees with the manifest" % (image["id"], offset))


def build_image(build_dir, info, version, out_dir, require_descriptor, link, log):
    """One manifest image entry, with its three files written into out_dir."""
    region = read_app_region(os.path.join(build_dir, info["ldscript_copy"]), info["ldscript_name"])
    log("%s: app_region 0x%08X..0x%08X  (%s)" % (info["env"], region.start, region.end, region.where()))

    hex_path = os.path.join(build_dir, info["progname"] + ".hex")
    if not os.path.isfile(hex_path):
        raise ReleaseError("%s is not there: that environment has not been built" % hex_path)
    with open(hex_path, "r", encoding="ascii", errors="replace") as fh:
        runs = parse_intel_hex(fh.read(), os.path.basename(hex_path))
    load_address, application = application_binary(runs, region, os.path.basename(hex_path))

    files = []
    bin_name = name_for(info, version, "bin")
    write_file(out_dir, bin_name, application)
    files.append(manifestref.file_entry("swd", bin_name, application, load_address))
    for extension, use in (("zip", "usb-dfu"), ("uf2", "usb-uf2")):
        source = os.path.join(build_dir, info["progname"] + "." + extension)
        if not os.path.isfile(source):
            raise ReleaseError("%s is not there: the build left no %s file" % (source, use))
        with open(source, "rb") as fh:
            blob = fh.read()
        name = name_for(info, version, extension)
        write_file(out_dir, name, blob)
        files.append(manifestref.file_entry(use, name, blob))

    image = {
        "id": "%s.%s.%s" % (info["role"], info["board"], info["band"]),
        "role": info["role"],
        "board": info["board"],
        "band": info["band"],
        "mcu": info["mcu"],
        "app_region": region.as_manifest(),
        # What must already be there for this image to be usable (spec/FIRMWARE-MANIFEST.md
        # section 2). The box link is the one thing it needs of the Walter: a Walter that speaks
        # another major speaks to this image not at all, and the descriptor inside the image
        # carries the same number, so a unit can be refused before anything is flashed. No
        # minimum Walter version is asked for - nothing of this firmware needs one yet.
        "requires": {"link_major": link["major"]},
        "files": files,
    }
    check_descriptor(image, application, version, link, require_descriptor, log)
    log("%s: %s  0x%08X..0x%08X  %d bytes" % (info["env"], bin_name, load_address,
                                              load_address + len(application), len(application)))
    return image


def add_bases(images, out_dir, base_dir, log):
    """The base under every image of a board family that needs one (spec/FIRMWARE-MANIFEST.md
    section 2 and 4a; tools/base/check.py FAMILIES): one `provision` file entry per image, the same
    vendored file - written once into out_dir under its release name - and the same hash under
    each. The file is held to its pin and to section 4a against each image's own app_region before
    it is listed, and a family image with no pinned base stops the release: a RAK4630 release
    without its base cannot provision a radio (spec/SYSTEM.md S-5a).
    """
    written = {}
    for image in images:
        family = base_check.family_of(image["board"])
        if family is None:
            continue
        pin = base_check.load_pin(family, base_dir)
        if pin is None:
            raise ReleaseError(
                "%s is a %s board and no base is pinned under boards/base/%s/: a release of it "
                "would leave a new radio with nothing to boot (spec/SYSTEM.md S-5a)"
                % (image["id"], family, family))
        try:
            data = base_check.read_base(pin)
            region = image["app_region"]
            summary = base_check.check_base(pin, data, int(region["start"], 16), int(region["end"], 16))
        except base_check.BaseError as exc:
            raise ReleaseError("the base of %s: %s" % (image["id"], exc))
        name = base_check.release_name(pin)
        if family not in written:
            write_file(out_dir, name, data)
            written[family] = name
        image["files"].append(manifestref.file_entry("provision", name, data))
        log("%s: %s  bootloader at %s, %s, %d bytes (use: provision, the base; boards/base/%s/)"
            % (image["id"], name, summary["bootloader"], summary["softdevice"], len(data), family))
    return written


def build_release(build_dirs, commit, out_dir, version=None, tag=None, built_at=None,
                  require_descriptor=False, log=None, base_dir=None, notices=None):
    """Writes the release into out_dir and returns the manifest it wrote.

    base_dir and notices are for the tests: boards/base and THIRD-PARTY-NOTICES of this repository
    otherwise.
    """
    log = log or (lambda line: None)
    if not re.match(r"^[0-9a-f]{40}$", commit or ""):
        raise ReleaseError("the commit must be the 40 hexadecimal characters of a commit, not %r" % commit)
    if not build_dirs:
        raise ReleaseError("no build directory given: there is nothing to release")

    infos = []
    for build_dir in build_dirs:
        info = read_build_info(build_dir)
        if version and info["version"] != version:
            raise ReleaseError(
                "%s was built as version %r and this release says %r. A manifest may not claim a "
                "version the image does not report: build with -D TWINOAK_FW_VERSION."
                % (build_dir, info["version"], version))
        infos.append(info)
    version = version or infos[0]["version"]
    for build_dir, info in zip(build_dirs, infos):
        if info["version"] != version:
            raise ReleaseError("%s was built as version %r and another environment as %r: one "
                               "release is one version" % (build_dir, info["version"], version))
        if info["upstream"] != infos[0]["upstream"]:
            raise ReleaseError("%s was built on %r and another environment on %r: one release is "
                               "one MeshCore pin" % (build_dir, info["upstream"], infos[0]["upstream"]))

    tag = tag or "v" + version.split("+", 1)[0]
    built_at = built_at or datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    link = link_version()

    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)
    log("version %s, tag %s, commit %s" % (version, tag, commit))

    images = [build_image(build_dir, info, version, out_dir, require_descriptor, link, log)
              for build_dir, info in zip(build_dirs, infos)]
    add_bases(images, out_dir, base_dir, log)

    manifest = {
        "schema": manifestref.SCHEMA,
        "product": PRODUCT,
        "version": version,
        "tag": tag,
        "commit": commit,
        "built_at": built_at,
        "link": link,
        "upstream": infos[0]["upstream"],
        "images": images,
    }
    try:
        manifestref.validate(manifest)
    except ValueError as exc:
        raise ReleaseError("the manifest this build makes is not valid: %s" % exc)
    # The same refusal sign_manifest.py makes before signing, made here before the manifest is
    # written: every family image carries the pinned base, beside the manifest, and it passes.
    try:
        base_check.check_manifest(manifest, out_dir, base_dir)
    except base_check.BaseError as exc:
        raise ReleaseError("the manifest this build makes has no valid base: %s" % exc)
    write_file(out_dir, "manifest.json", manifestref.canonical(manifest))
    log("manifest.json: valid (manifestref.validate), %d image(s), the base under every %s image"
        % (len(images), "/".join(base_check.FAMILIES)))

    # The notices the base's licences ask for, beside the files they are about
    # (boards/base/<family>/README.md, "Licences"). Not a manifest file - not an image - but
    # SHA256SUMS covers it like everything else here.
    notices = notices or os.path.join(REPO, NOTICES)
    if not os.path.isfile(notices):
        raise ReleaseError("%s is not there: the base's licences ask for their notices in every "
                           "release" % notices)
    with open(notices, "rb") as fh:
        write_file(out_dir, NOTICES, fh.read())
    log("%s: the base's licence notices" % NOTICES)

    names = sorted(name for name in os.listdir(out_dir)
                   if name != "SHA256SUMS" and os.path.isfile(os.path.join(out_dir, name)))
    lines = []
    for name in names:
        with open(os.path.join(out_dir, name), "rb") as fh:
            lines.append("%s  %s\n" % (hashlib.sha256(fh.read()).hexdigest(), name))
    with open(os.path.join(out_dir, "SHA256SUMS"), "w", encoding="ascii", newline="\n") as fh:
        fh.writelines(lines)
    log("SHA256SUMS: %d file(s), the manifest included" % len(names))
    return manifest


def clean(out_dir):
    """Empties out_dir: a stale image of an earlier run beside a new manifest is a file nothing
    covers."""
    if os.path.isdir(out_dir):
        for name in os.listdir(out_dir):
            path = os.path.join(out_dir, name)
            if os.path.isdir(path):
                shutil.rmtree(path)
            else:
                os.remove(path)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Writes a release: the images, manifest.json and SHA256SUMS.")
    parser.add_argument("--build-dir", action="append", default=[], metavar="DIR",
                        help="a built environment's directory (.pio/build/<env>); once per image")
    parser.add_argument("--commit", required=True, help="the 40 hexadecimal characters of the commit")
    parser.add_argument("--version", default=None,
                        help="X.Y.Z+sha7; must be the version the build reports (default: that one)")
    parser.add_argument("--tag", default=None,
                        help="the git tag (default: v and the version without its +build metadata)")
    parser.add_argument("--built-at", default=None, help="UTC, YYYY-MM-DDTHH:MM:SSZ (default: now)")
    parser.add_argument("--out", default=os.path.join("out", "release"),
                        help="where to write (default: out/release)")
    parser.add_argument("--clean", action="store_true", help="empty the output directory first")
    parser.add_argument("--require-descriptor", action="store_true",
                        help="refuse an image without exactly one descriptor that agrees with the manifest")
    args = parser.parse_args(argv)

    try:
        if args.clean:
            clean(args.out)
        build_release(args.build_dir, args.commit, args.out, version=args.version, tag=args.tag,
                      built_at=args.built_at, require_descriptor=args.require_descriptor,
                      log=print)
    except ReleaseError as exc:
        sys.stderr.write("error: %s\n" % exc)
        return 1
    print("wrote %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
