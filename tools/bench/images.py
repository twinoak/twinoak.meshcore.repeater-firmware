"""What may be flashed, and only that: the guards on an image, run before any port is opened.

`bench flash <image>` takes one kind of file - the DFU package the build leaves as firmware.zip
and the release publishes as twinoak-<role>-<board>-<band>-vX.Y.Z.zip (tools/pio/meshcore_uf2.py
says how the build makes it; tools/release/build_release.py names it `usb-dfu`). Everything else
is refused with the reason, and the reason is read out of the file and not off its name:

  * not a zip, or a zip without adafruit-nrfutil's manifest.json: not a DFU package;
  * a package whose manifest carries a `bootloader`, a `softdevice` or a `softdevice_bootloader`
    entry: this tool writes the application and nothing else (CLAUDE.md: never write outside the
    application region; spec/SYSTEM.md S-5);
  * an application whose bytes do not carry exactly one valid descriptor, by the rules of
    spec/FIRMWARE-MANIFEST.md section 4 through spec/tools/manifestref.py's own find_descriptor;
  * a descriptor whose board is not the bench's, or whose role is not one this board's family
    builds (`manager` is the Walter's);
  * an application that does not fit the app_region the release tooling reads from the linker
    script (tools/release/build_release.py's read_app_region, over the ldscript.ld the build left
    beside the image or the core's own script) - never a constant here;
  * a role that transmits (repeater, bridge) when the RF port is not the dummy load. The scanner
    never transmits (test/test_rails holds it to that) and is allowed on any RF port.

UF2, Intel HEX and raw binaries are refused by name as well as by content: no UF2, no SWD, no
erase, by any command of this tool.
"""

import hashlib
import io
import json
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
for _path in (os.path.join(REPO, "spec", "tools"), os.path.join(REPO, "tools", "release")):
    if _path not in sys.path:
        sys.path.insert(0, _path)

import build_release  # noqa: E402  (the application region, read from a linker script)
import manifestref  # noqa: E402  (the descriptor, as the specification defines it)

from config import Refused  # noqa: E402

# The roles this board's family builds (docs/ARCHITECTURE.md section 8) and which of them put a
# packet on the air. `manager` is a role of spec/FIRMWARE-MANIFEST.md section 4 too, but it is the
# Walter's and no image of it belongs on this board.
FAMILY_ROLES = ("repeater", "bridge", "scanner")
TRANSMITTING_ROLES = ("repeater", "bridge")

DFU_MANIFEST = "manifest.json"
FORBIDDEN_DFU_PARTS = ("bootloader", "softdevice", "softdevice_bootloader")


class Image(object):
    """One DFU package that passed every guard."""

    def __init__(self, path, package_sha256, application, app_sha256, offset, descriptor):
        self.path = path
        self.package_sha256 = package_sha256
        self.application = application
        self.app_sha256 = app_sha256
        self.offset = offset
        self.descriptor = descriptor

    @property
    def role(self):
        return self.descriptor["role"]

    @property
    def board(self):
        return self.descriptor["board"]

    @property
    def version(self):
        return self.descriptor["version"]

    @property
    def transmits(self):
        return role_transmits(self.role)

    def summary(self):
        d = self.descriptor
        return "%s %s %s version %s (descriptor at 0x%X, %d bytes, sha256 %s)" % (
            d["role"], d["board"], d["band"], d["version"], self.offset, len(self.application),
            self.package_sha256)

    def log_record(self):
        return {
            "image": os.path.basename(self.path),
            "package_sha256": self.package_sha256,
            "application_sha256": self.app_sha256,
            "application_bytes": len(self.application),
            "descriptor": dict(self.descriptor),
            "descriptor_offset": "0x%X" % self.offset,
        }


def role_transmits(role):
    return role in TRANSMITTING_ROLES


def read_dfu_package(path):
    """(the application bytes, the nrfutil manifest) of a DFU package, or Refused."""
    name = os.path.basename(path)
    lower = name.lower()
    if lower.endswith(".uf2"):
        raise Refused("%s is a UF2 image: this tool flashes the DFU package (firmware.zip) through "
                      "adafruit-nrfutil and nothing else - no UF2 drive, no SWD" % name)
    if lower.endswith(".hex") or lower.endswith(".bin") or lower.endswith(".elf"):
        raise Refused("%s is not a DFU package: a .hex, .bin or .elf is the Walter's or a "
                      "debugger's, written over SWD, which no command of this tool does" % name)
    if not os.path.isfile(path):
        raise Refused("%s is not a file" % path)
    with open(path, "rb") as fh:
        data = fh.read()
    if not zipfile.is_zipfile(io.BytesIO(data)):
        raise Refused("%s is not a zip file, so not a DFU package" % name)
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as zf:
            names = zf.namelist()
            if DFU_MANIFEST not in names:
                raise Refused("%s has no %s: not an adafruit-nrfutil DFU package" % (name, DFU_MANIFEST))
            try:
                manifest = json.loads(zf.read(DFU_MANIFEST).decode("utf-8"))["manifest"]
            except (ValueError, KeyError, UnicodeDecodeError) as exc:
                raise Refused("%s: its %s is not a DFU manifest (%s)" % (name, DFU_MANIFEST, exc))
            if not isinstance(manifest, dict):
                raise Refused("%s: its %s is not a DFU manifest" % (name, DFU_MANIFEST))
            present = [part for part in FORBIDDEN_DFU_PARTS if manifest.get(part)]
            if present:
                raise Refused(
                    "%s carries %s: this tool writes the application region and nothing else - "
                    "never a bootloader or a SoftDevice (spec/SYSTEM.md S-5, CLAUDE.md)"
                    % (name, " and ".join("a %s" % p.replace("_", "+") for p in present)))
            app = manifest.get("application")
            if not isinstance(app, dict) or not app.get("bin_file"):
                raise Refused("%s has no application in its DFU manifest" % name)
            bin_name = app["bin_file"]
            if bin_name not in names:
                raise Refused("%s names %s and does not contain it" % (name, bin_name))
            application = zf.read(bin_name)
    except zipfile.BadZipFile as exc:
        raise Refused("%s is not a readable zip (%s)" % (name, exc))
    return data, application, manifest


def inspect_image(path, board, rf, region):
    """Every guard, in order. Returns an Image or raises Refused. Opens no port: it reads a file
    and a linker script's numbers and nothing else."""
    name = os.path.basename(path)
    data, application, _manifest = read_dfu_package(path)
    try:
        offset, descriptor = manifestref.find_descriptor(application)
    except ValueError as exc:
        raise Refused("%s: %s - not an image this build or this release made "
                      "(spec/FIRMWARE-MANIFEST.md section 4)" % (name, exc))
    if descriptor["role"] not in FAMILY_ROLES:
        raise Refused("%s says it is a %r image: not a role of this board's family (%s)"
                      % (name, descriptor["role"], ", ".join(FAMILY_ROLES)))
    if descriptor["board"] != board:
        raise Refused("%s is for board %r and the bench is %r: an image for another board is never "
                      "flashed here" % (name, descriptor["board"], board))
    if region is None:
        raise Refused("%s: the application region could not be read, so whether the image fits is "
                      "unknown. Build the environment once (tools/verify.ps1 leaves "
                      "out/build/<env>/ldscript.ld) or install the platform" % name)
    room = region.end - region.start
    if len(application) > room:
        raise Refused("%s is %d bytes and the application region %s is %d bytes: it does not fit, "
                      "and an image that does not fit would be written over the file system that "
                      "holds the node's identity" % (name, len(application), region, room))
    if role_transmits(descriptor["role"]) and rf != "dummy-load":
        raise Refused("%s is a %s image, a role that transmits, and the bench's RF port is %r: a "
                      "transmitting image is flashed only with the dummy load on the antenna port "
                      "(bench.local.json's \"rf\")" % (name, descriptor["role"], rf))
    return Image(path, hashlib.sha256(data).hexdigest(), application,
                 hashlib.sha256(application).hexdigest(), offset, descriptor)


# ---- where the application region comes from

def platformio_core_dir():
    return os.environ.get("PLATFORMIO_CORE_DIR") or os.path.join(os.path.expanduser("~"), ".platformio")


def ldscript_name(repo=REPO):
    """The linker script boards/rak4631.json names (the one place it is written)."""
    try:
        with open(os.path.join(repo, "boards", "rak4631.json"), "r", encoding="utf-8") as fh:
            return json.load(fh)["build"]["arduino"]["ldscript"]
    except (IOError, OSError, ValueError, KeyError):
        return None


def find_region(image_path, repo=REPO, core_dir=None):
    """The application region: the ldscript.ld the build left beside the image if the image is in
    a build directory (tools/pio/buildinfo.py copies it there), else the core's own script, which
    boards/rak4631.json names and the pinned platform package holds. None when neither is there -
    inspect_image then refuses, because an unknown region is not a region."""
    beside = os.path.join(os.path.dirname(os.path.abspath(image_path)), "ldscript.ld")
    candidates = [beside]
    name = ldscript_name(repo)
    if name:
        core = core_dir or os.path.join(platformio_core_dir(), "packages", "framework-arduinoadafruitnrf52")
        candidates.append(os.path.join(core, "cores", "nRF5", "linker", name))
    for path in candidates:
        if os.path.isfile(path):
            try:
                return build_release.read_app_region(path, os.path.basename(path))
            except build_release.ReleaseError:
                continue
    return None
