#!/usr/bin/env python3
"""The base under the application, held to its pin: boards/base/<family>/.

    python tools/base/check.py --build-dir out/build/<env> [--build-dir ...]
    python tools/base/check.py --ldscript <nrf52840_s140_v6.ld>
    python tools/base/check.py --region 0x26000 0xED000

A factory RAK4630 carries RAK's RUI3, not the MBR, the SoftDevice and the bootloader this firmware
is linked against. The Walter writes that base once, from the release's `provision` file, on a
person's command (spec/SYSTEM.md S-5a; spec/FIRMWARE-MANIFEST.md section 4a). Nobody here built that
file: it is vendored byte for byte under boards/base/<family>/ beside a base.json that pins it -
where it came from, its SHA-256, and what spec/tools/manifestref.py's `base` command says of it -
and a README.md that says the same in words. This module holds the three to each other and to the
specification, on every run of tools/verify.ps1 and in every release:

  1. the file's SHA-256 is the pin's (a changed byte is a different base, whatever its name);
  2. manifestref.validate_provision accepts it against the application region the release reads
     from the linker script - the seven rules of section 4a, in the specification's own words;
  3. the summary that check returns (the bootloader's address, the SoftDevice, the UICR words) is
     the pin's, and the README says the same numbers.

What is not here: writing it to a radio (the Walter's), and any opinion on the bytes beyond what
section 4a asks. tools/release/build_release.py lists the file under every image of the family and
refuses a release without it; tools/release/sign_manifest.py refuses to sign a manifest that lacks
it or whose copy of it fails these checks; tools/release/selfcheck.py runs this on every commit.
"""

import argparse
import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "spec", "tools"))

import manifestref  # noqa: E402  (spec/ is a copy and is never edited)

BASES_DIR = os.path.join(REPO, "boards", "base")
PIN_FILE = "base.json"
README_FILE = "README.md"

# The board families whose images must carry a base. An image's `board` is `<family>-<variant>`
# (spec/FIRMWARE-MANIFEST.md section 2: rak4630-single, later rak4630-dual-a); every variant of a
# family is the same module and takes the same base. A family listed here with no pin directory is
# a refused release, not a release without a base.
FAMILIES = ("rak4630",)


class BaseError(Exception):
    """A base that is not the pinned one, or a release that lacks it. Printed as one line."""


def family_of(board):
    """`rak4630` of `rak4630-single`; None for a board of no family listed here."""
    family = (board or "").split("-", 1)[0]
    return family if family in FAMILIES else None


def load_pin(family, base_dir=None):
    """boards/base/<family>/base.json, or None when the family has no pin directory."""
    base_dir = base_dir or BASES_DIR
    path = os.path.join(base_dir, family, PIN_FILE)
    if not os.path.isfile(path):
        return None
    with open(path, "r", encoding="utf-8") as fh:
        pin = json.load(fh)
    for key in ("family", "file", "sha256", "size", "bootloader", "softdevice", "source", "summary"):
        if key not in pin:
            raise BaseError("%s is missing %r" % (path, key))
    if pin["family"] != family:
        raise BaseError("%s says family %r but lies under %r" % (path, pin["family"], family))
    pin["_dir"] = os.path.join(base_dir, family)
    return pin


def read_base(pin):
    """The vendored file's bytes, exactly as committed."""
    path = os.path.join(pin["_dir"], pin["file"])
    if not os.path.isfile(path):
        raise BaseError("%s is not there: the base named by %s is missing" % (path, PIN_FILE))
    with open(path, "rb") as fh:
        return fh.read()


def release_name(pin):
    """twinoak-base-<family>-<bootloader version>-<softdevice>.hex, the name in a release:
    twinoak-base-rak4630-0.4.4-s140-6.1.1.hex."""
    softdevice = pin["softdevice"].lower().replace(" ", "-")
    return "twinoak-base-%s-%s-%s.hex" % (pin["family"], pin["bootloader"]["version"], softdevice)


def check_base(pin, data, app_start, app_end, what=None):
    """The three checks of the module docstring, in that order. Returns the summary."""
    what = what or pin["file"]
    digest = hashlib.sha256(data).hexdigest()
    if digest != pin["sha256"] or len(data) != pin["size"]:
        raise BaseError(
            "%s is not the pinned base: its SHA-256 is %s (%d bytes), the pin in %s says %s "
            "(%d bytes). A changed byte is a different base; the pin, the README and the "
            "licences are rewritten together or not at all."
            % (what, digest, len(data), PIN_FILE, pin["sha256"], pin["size"]))
    try:
        summary = manifestref.validate_provision(data, app_start, app_end)
    except ValueError as exc:
        raise BaseError("%s is refused against the application region 0x%08X..0x%08X "
                        "(spec/FIRMWARE-MANIFEST.md section 4a): %s" % (what, app_start, app_end, exc))
    if summary != pin["summary"]:
        raise BaseError(
            "%s is not what %s says it is: manifestref.py base returns %s, the pin says %s"
            % (what, PIN_FILE, json.dumps(summary, sort_keys=True),
               json.dumps(pin["summary"], sort_keys=True)))
    return summary


def check_readme(pin):
    """The README says the same numbers as the pin: the hash, the version, the SoftDevice, the
    source commit and what the check returns. Returns the README's path."""
    path = os.path.join(pin["_dir"], README_FILE)
    if not os.path.isfile(path):
        raise BaseError("%s is not there: a vendored base is documented or it is not vendored" % path)
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    wanted = {
        "the file's SHA-256": pin["sha256"],
        "the file's name": pin["file"],
        "the bootloader's version": pin["bootloader"]["version"],
        "the SoftDevice": pin["softdevice"],
        "the source commit": pin["source"]["commit"],
        "the source path": pin["source"]["path"],
        "the bootloader's address": pin["summary"]["bootloader"],
        "the MBR parameter page": pin["summary"]["mbr_params"],
        "the flash byte count": str(pin["summary"]["flash_bytes"]),
    }
    missing = ["%s (%s)" % (k, v) for k, v in wanted.items() if v not in text]
    if missing:
        raise BaseError("%s does not say what %s says: missing %s" % (path, PIN_FILE, "; ".join(missing)))
    return path


def check_manifest(manifest, out_dir, base_dir=None):
    """Every image of a family in FAMILIES carries exactly one `provision` file, it is the pinned
    base, its bytes are beside the manifest and pass against the image's app_region. What
    build_release.py holds before writing a manifest and sign_manifest.py before signing one.
    Returns {image id: summary}."""
    seen = {}
    for image in manifest.get("images", []):
        family = family_of(image.get("board"))
        if family is None:
            continue
        pin = load_pin(family, base_dir)
        if pin is None:
            raise BaseError("image %s is a %s board and no base is pinned under boards/base/%s/"
                            % (image.get("id"), family, family))
        files = [f for f in image.get("files", []) if f.get("use") == "provision"]
        if len(files) != 1:
            raise BaseError(
                "image %s has %d provision file(s), and a %s image carries exactly one: the base "
                "(spec/SYSTEM.md S-5a). Nothing is signed without it."
                % (image.get("id"), len(files), family))
        entry = files[0]
        if entry.get("sha256") != pin["sha256"] or entry.get("size") != pin["size"]:
            raise BaseError("image %s lists a base with SHA-256 %s, and the pinned base is %s"
                            % (image.get("id"), entry.get("sha256"), pin["sha256"]))
        path = os.path.join(out_dir, entry.get("name") or "")
        if not os.path.isfile(path):
            raise BaseError("image %s names %s, which is not beside the manifest"
                            % (image.get("id"), entry.get("name")))
        with open(path, "rb") as fh:
            data = fh.read()
        region = image.get("app_region") or {}
        try:
            start, end = int(region["start"], 16), int(region["end"], 16)
        except (KeyError, TypeError, ValueError):
            raise BaseError("image %s has no app_region to hold its base against" % image.get("id"))
        seen[image["id"]] = check_base(pin, data, start, end, what=entry.get("name"))
    return seen


def main(argv=None):
    parser = argparse.ArgumentParser(description="Holds the vendored base to its pin and to the specification.")
    parser.add_argument("--build-dir", action="append", default=[], metavar="DIR",
                        help="a built environment's directory: its ldscript.ld gives the region")
    parser.add_argument("--ldscript", default=None, help="a linker script to read the region from")
    parser.add_argument("--region", nargs=2, default=None, metavar=("START", "END"),
                        help="the application region, as two hex numbers")
    parser.add_argument("--base-dir", default=None, help="instead of boards/base (tests)")
    args = parser.parse_args(argv)

    sys.path.insert(0, os.path.join(REPO, "tools", "release"))
    import build_release  # noqa: E402  (its linker-script reader, so the region is read once)

    regions = []
    for build_dir in args.build_dir:
        info = build_release.read_build_info(build_dir)
        regions.append((build_dir, build_release.read_app_region(
            os.path.join(build_dir, info["ldscript_copy"]), info["ldscript_name"])))
    if args.ldscript:
        regions.append((args.ldscript, build_release.read_app_region(args.ldscript)))
    if args.region:
        start, end = int(args.region[0], 16), int(args.region[1], 16)
        regions.append(("--region", build_release.AppRegion(start, end, "--region", 0, "")))
    if not regions:
        parser.error("give --build-dir, --ldscript or --region: the base is checked against a region")

    try:
        for family in FAMILIES:
            pin = load_pin(family, args.base_dir)
            if pin is None:
                raise BaseError("no base is pinned under boards/base/%s/" % family)
            data = read_base(pin)
            for where, region in regions:
                summary = check_base(pin, data, region.start, region.end)
                print("%s: %s is the pinned base (sha256 %s), passes section 4a against "
                      "0x%08X..0x%08X (%s): bootloader at %s, %s, MBR parameters at %s"
                      % (family, pin["file"], pin["sha256"][:12], region.start, region.end, where,
                         summary["bootloader"], summary["softdevice"], summary["mbr_params"]))
            readme = check_readme(pin)
            print("%s: %s says the same" % (family, os.path.relpath(readme, REPO)))
    except (BaseError, build_release.ReleaseError) as exc:
        sys.stderr.write("error: %s\n" % exc)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
