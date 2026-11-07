#!/usr/bin/env python3
"""The release tooling's own tests.

    python tools/release/test_release.py            (needs the `cryptography` of requirements.txt)

tools/verify.ps1 runs these on every commit, before the dry run of the real thing, so the release
path is exercised without GitHub. What is held here:

  * the manifest's shape, against spec/fixtures/manifest/ - the specification's own output, so a
    manifest this repository writes is the one a Walter and MeshMaster's catalogue expect;
  * the application region, against a linker script rather than a constant;
  * the raw binary, against a small synthetic image: its load address, a gap in it, and a byte
    outside the application region - the two refusals that keep an update off the bootloader, the
    SoftDevice and the file system;
  * the descriptor check, off by default and on with --require-descriptor;
  * the names: every [img_*] section of platformio.ini - the one place a role, a board and a
    band are written - becomes a manifest entry whose id and file names are that section's
    (test/test_image holds the firmware's half of the same rule);
  * what may be signed: the specification's public test key never signs a release version, a tag
    with no key in the environment is refused and says how the key is made, and no message
    carries a key;
  * the published public half, in the one form both repositories write a key in: the trust
    table's `ed25519 <key id> <64 hex>` line and the 64 hexadecimal characters alone are the same
    key, a named key id that is not the key's own is refused, and the **real** keys/release.pub
    of this commit reads as the key it says it is;
  * the hand-written top of a release body (release-notes/vX.Y.Z.md): which file a version
    names, that a release with none is refused and says what to write, that an empty one is
    refused too, that a rehearsal is not - and that **this** commit's release-notes/ holds a
    readable file for the version platformio.ini is about to be tagged as, if one is there;
  * the base under the application (boards/base/, tools/base/check.py; spec/SYSTEM.md S-5a,
    spec/FIRMWARE-MANIFEST.md section 4a): every RAK4630 image of a release lists the one
    vendored file as `use: provision` and the manifest validates; a changed byte in that file is
    refused with its hash; a bootloader-only file in its place is refused in section 4a's words;
    a manifest built without the base is refused before it is signed; and the pin, the file and
    its README agree with each other on this commit.
"""

import contextlib
import glob
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "spec", "tools"))
sys.path.insert(0, os.path.join(REPO, "tools", "base"))

import build_release  # noqa: E402
import check as base_check  # noqa: E402  (tools/base/check.py)
import manifestref  # noqa: E402
import release_notes  # noqa: E402
import sign_manifest  # noqa: E402

FIXTURES = os.path.join(REPO, "spec", "fixtures", "manifest")
BASES = os.path.join(REPO, "boards", "base")
COMMIT = "abc1234" + "0" * 33

# The MeshCore pin the synthetic build directories below were built against. Plainly not a
# release: a bump moves platformio.ini's [meshcore] and nothing in this file, so these tests
# pass unchanged and CI on a bump's pull request is about upstream rather than about a
# literal (docs/upstream.md, "What a bump costs today"). test/test_version holds the other
# half of the same rule.
UPSTREAM = {"name": "MeshCore", "tag": "repeater-vX.Y.Z",
            "commit": "0123abcd" + "ef0123456789abcdef0123456789abcd"}


def platformio_images(path=None):
    """Every [img_*] section of platformio.ini: {section: {"role", "board", "band"}}.

    That file is the only place any of the three is written (phase 3's task 01), so a test that
    wants to know what this repository builds reads it rather than restating it.
    """
    path = path or os.path.join(REPO, "platformio.ini")
    images = {}
    section = None
    with open(path, "r", encoding="utf-8") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line[0] in ";#":
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1]
                if section.startswith("img_"):
                    images[section] = {}
                continue
            if section in images and "=" in line:
                key, value = line.split("=", 1)
                images[section][key.strip()] = value.strip()
    return images

# The board this repository builds today, and the numbers of the linker script it builds with. The
# script below is a copy of the shape the Adafruit nRF52 core's nrf52840_s140_v6.ld has, not of its
# numbers' meaning: what is tested is that the tool reads whatever the script says.
LDSCRIPT = """/* Linker script to configure memory regions. */

SEARCH_DIR(.)

MEMORY
{
  FLASH (rx)     : ORIGIN = 0x26000, LENGTH = 0xED000 - 0x26000

  RAM (rwx) :  ORIGIN = 0x20006000, LENGTH = 0x20040000 - 0x20006000
}
"""
APP_START = 0x26000
APP_END = 0xED000


def hex_record(rtype, offset, data):
    raw = bytes([len(data), (offset >> 8) & 0xFF, offset & 0xFF, rtype]) + data
    return ":" + (raw + bytes([(-sum(raw)) & 0xFF])).hex().upper()


def intel_hex(runs, eof=True):
    """An Intel HEX file holding the given (address, bytes) runs, 16 bytes to a record."""
    lines = []
    upper = None
    for address, blob in runs:
        for i in range(0, len(blob), 16):
            at = address + i
            if (at >> 16) != upper:
                upper = at >> 16
                lines.append(hex_record(0x04, 0, bytes([(upper >> 8) & 0xFF, upper & 0xFF])))
            lines.append(hex_record(0x00, at & 0xFFFF, blob[i:i + 16]))
    if eof:
        lines.append(hex_record(0x01, 0, b""))
    return "\n".join(lines) + "\n"


def filler(size, seed=1):
    out = bytearray()
    state = seed
    while len(out) < size:
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        out += bytes([state & 0xFF, (state >> 8) & 0xFF, (state >> 16) & 0xFF, (state >> 24) & 0xFF])
    return bytes(out[:size])


class ReleaseTestCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="twinoak-release-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.out = os.path.join(self.tmp, "release")

    def build_dir(self, runs=None, version="0.3.0+abc1234", role="repeater",
                  board="rak4630-single", band="868", ldscript=LDSCRIPT, name="build"):
        """A build directory shaped like one tools/pio/buildinfo.py leaves."""
        runs = runs if runs is not None else [(APP_START, filler(4096))]
        path = os.path.join(self.tmp, name)
        if not os.path.isdir(path):
            os.makedirs(path)
        info = {
            "env": name, "progname": "firmware", "role": role, "board": board, "band": band,
            "mcu": "nrf52840", "version": version,
            "upstream": dict(UPSTREAM),
            "ldscript_name": "nrf52840_s140_v6.ld", "ldscript_copy": "ldscript.ld",
        }
        self.write(os.path.join(path, "buildinfo.json"), json.dumps(info, indent=2) + "\n")
        self.write(os.path.join(path, "ldscript.ld"), ldscript)
        self.write(os.path.join(path, "firmware.hex"), intel_hex(runs))
        self.write(os.path.join(path, "firmware.zip"), b"a DFU package, for this test")
        self.write(os.path.join(path, "firmware.uf2"), b"a UF2 image, for this test")
        return path

    @staticmethod
    def write(path, data):
        if isinstance(data, bytes):
            with open(path, "wb") as fh:
                fh.write(data)
        else:
            with open(path, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(data)

    def release(self, build_dirs=None, **kwargs):
        kwargs.setdefault("built_at", "2026-10-12T08:00:00Z")
        return build_release.build_release(build_dirs or [self.build_dir()], COMMIT, self.out,
                                           **kwargs)


# ------------------------------------------------------------------ the application region

class TheApplicationRegion(ReleaseTestCase):
    def region(self, text, name="test.ld"):
        path = os.path.join(self.tmp, "x.ld")
        self.write(path, text)
        return build_release.read_app_region(path, name)

    def test_it_is_the_flash_region_of_the_memory_block(self):
        region = self.region(LDSCRIPT)
        self.assertEqual((region.start, region.end), (APP_START, APP_END))
        self.assertEqual(region.as_manifest(), {"start": "0x00026000", "end": "0x000ED000"})

    def test_it_says_which_line_it_came_from(self):
        region = self.region(LDSCRIPT)
        self.assertEqual(region.lineno, 7)
        self.assertTrue(region.line.startswith("FLASH (rx)"), region.line)
        self.assertEqual(LDSCRIPT.splitlines()[region.lineno - 1].strip(), region.line)

    def test_a_comment_does_not_move_the_line_number(self):
        text = "/* one\n   two */\nMEMORY\n{\n  FLASH (rx) : ORIGIN = 0x1000, LENGTH = 4K\n}\n"
        region = self.region(text)
        self.assertEqual((region.start, region.end, region.lineno), (0x1000, 0x2000, 5))

    def test_lds_arithmetic(self):
        for text, want in (("0x10", 0x10), ("16", 16), ("1K", 1024), ("2M", 2 * 1024 ** 2),
                           ("0xED000 - 0x26000", 0xED000 - 0x26000), ("(1 + 3) * 2K", 8192),
                           ("8K / 2", 4096), ("-16 + 32", 16)):
            self.assertEqual(build_release.ld_expression(text), want, text)

    def test_ld_name_aliases(self):
        region = self.region("MEMORY {\n FLASH (rx) : org = 0x8000, len = 32K\n}\n")
        self.assertEqual((region.start, region.end), (0x8000, 0x8000 + 32 * 1024))

    def test_a_script_without_a_flash_region_is_refused(self):
        with self.assertRaises(build_release.ReleaseError) as caught:
            self.region("MEMORY\n{\n  RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K\n}\n")
        self.assertIn("no FLASH region", str(caught.exception))
        self.assertIn("RAM", str(caught.exception))

    def test_an_expression_that_is_not_arithmetic_is_refused(self):
        with self.assertRaises(build_release.ReleaseError):
            self.region("MEMORY {\n FLASH : ORIGIN = __start__, LENGTH = 4K\n}\n")

    def test_the_region_in_the_manifest_is_the_scripts(self):
        """The acceptance criterion: app_region is what the linker script says."""
        manifest = self.release()
        region = build_release.read_app_region(os.path.join(self.tmp, "build", "ldscript.ld"))
        self.assertEqual(manifest["images"][0]["app_region"], region.as_manifest())


# ------------------------------------------------------------------ the linked image

class TheLinkedImage(ReleaseTestCase):
    def test_the_runs_of_a_hex_file_come_back(self):
        runs = [(APP_START, filler(100)), (APP_START + 0x1000, filler(48, seed=2))]
        self.assertEqual(build_release.parse_intel_hex(intel_hex(runs)), runs)

    def test_a_record_that_fails_its_checksum_is_refused(self):
        text = intel_hex([(APP_START, filler(32))])
        lines = text.splitlines()
        lines[1] = lines[1][:-2] + ("00" if lines[1][-2:] != "00" else "01")
        with self.assertRaises(build_release.ReleaseError) as caught:
            build_release.parse_intel_hex("\n".join(lines))
        self.assertIn("checksum", str(caught.exception))

    def test_a_truncated_hex_file_is_refused(self):
        with self.assertRaises(build_release.ReleaseError) as caught:
            build_release.parse_intel_hex(intel_hex([(APP_START, filler(32))], eof=False))
        self.assertIn("end-of-file", str(caught.exception))

    def test_segment_address_records_are_understood(self):
        """The nRF52 build's own .hex uses record type 2, not 4."""
        blob = filler(32)
        text = "\n".join([hex_record(0x02, 0, bytes([0x20, 0x00])),
                          hex_record(0x00, 0x6000, blob),
                          hex_record(0x01, 0, b"")]) + "\n"
        self.assertEqual(build_release.parse_intel_hex(text), [(0x26000, blob)])

    def test_the_load_address_is_the_lowest_address_the_image_occupies(self):
        manifest = self.release(build_dirs=[self.build_dir(runs=[(APP_START + 0x40, filler(256))])])
        swd = [f for f in manifest["images"][0]["files"] if f["use"] == "swd"][0]
        self.assertEqual(swd["load_address"], "0x%08X" % (APP_START + 0x40))
        self.assertEqual(swd["size"], 256)
        with open(os.path.join(self.out, swd["name"]), "rb") as fh:
            self.assertEqual(fh.read(), filler(256))

    def test_a_gap_in_the_image_is_refused(self):
        runs = [(APP_START, filler(256)), (APP_START + 0x1000, filler(256, seed=7))]
        with self.assertRaises(build_release.ReleaseError) as caught:
            self.release(build_dirs=[self.build_dir(runs=runs)])
        self.assertIn("not one contiguous span", str(caught.exception))
        self.assertIn("0x00026100..0x00027000 (3840 bytes)", str(caught.exception))

    def test_a_byte_below_the_application_region_is_refused(self):
        with self.assertRaises(build_release.ReleaseError) as caught:
            self.release(build_dirs=[self.build_dir(runs=[(APP_START - 16, filler(64))])])
        self.assertIn("outside the application region", str(caught.exception))
        self.assertIn("nrf52840_s140_v6.ld:7", str(caught.exception))

    def test_a_byte_above_the_application_region_is_refused(self):
        with self.assertRaises(build_release.ReleaseError) as caught:
            self.release(build_dirs=[self.build_dir(runs=[(APP_END - 16, filler(64))])])
        self.assertIn("outside the application region", str(caught.exception))

    def test_an_image_that_exactly_fills_the_region_is_allowed(self):
        manifest = self.release(build_dirs=[self.build_dir(runs=[(APP_END - 64, filler(64))])])
        swd = [f for f in manifest["images"][0]["files"] if f["use"] == "swd"][0]
        self.assertEqual(int(swd["load_address"], 16) + swd["size"], APP_END)


# ------------------------------------------------------------------ the manifest

class TheManifest(ReleaseTestCase):
    def setUp(self):
        ReleaseTestCase.setUp(self)
        # The repeater's manifest with its provision file (spec/FIRMWARE-MANIFEST.md section 6):
        # what a release of a RAK4630 board carries since task p3.4-01.
        with open(os.path.join(FIXTURES, "manifest-repeater-base.json"), "r", encoding="utf-8") as fh:
            self.fixture = json.load(fh)

    def test_the_specifications_own_fixtures_still_check(self):
        """Signing and the descriptor are tested against these, so they are proven first."""
        self.assertGreater(manifestref.check(FIXTURES), 0)

    def test_it_has_the_shape_of_the_specifications_fixture(self):
        manifest = self.release()
        self.assertEqual(list(manifest), list(self.fixture))
        self.assertEqual(manifest["schema"], self.fixture["schema"])
        self.assertEqual(manifest["product"], self.fixture["product"])
        self.assertEqual(list(manifest["link"]), list(self.fixture["link"]))
        self.assertEqual(list(manifest["upstream"]), list(self.fixture["upstream"]))
        image, want = manifest["images"][0], self.fixture["images"][0]
        self.assertEqual(list(image), list(want))
        self.assertEqual(list(image["app_region"]), list(want["app_region"]))
        for got in image["files"]:
            same_use = [f for f in want["files"] if f["use"] == got["use"]][0]
            self.assertEqual(list(got), list(same_use), got["use"])

    def test_the_file_names_are_the_specifications(self):
        """The fixture's release is 0.3.0 of this role, board and band: the names must be its.
        The base's name is the one exception: the fixture's stand-in is twinoak-base-rak4630-
        s140-6.1.1.hex, and ours carries the bootloader's version as well (task p3.4-01), so it
        is held to its own rule in TheBase."""
        manifest = self.release()
        self.assertEqual([f["name"] for f in manifest["images"][0]["files"] if f["use"] != "provision"],
                         [f["name"] for f in self.fixture["images"][0]["files"] if f["use"] != "provision"])

    def test_the_image_id_is_role_board_band(self):
        self.assertEqual(self.release()["images"][0]["id"], "repeater.rak4630-single.868")

    def test_every_image_of_platformio_ini_becomes_its_own_entry(self):
        """The names, from the one place they are written, through to the manifest."""
        images = platformio_images()
        self.assertTrue(images, "platformio.ini has no [img_*] section")
        for section, names in sorted(images.items()):
            role, board, band = names["role"], names["board"], names["band"]
            # The section is named after the environment, and the environment after the image.
            self.assertEqual(section, "img_%s_%s" % (role, board.replace("-", "_")))
            manifest = self.release(build_dirs=[self.build_dir(
                role=role, board=board, band=band, name=section)])
            image = manifest["images"][0]
            self.assertEqual(image["id"], "%s.%s.%s" % (role, board, band))
            self.assertEqual([image["role"], image["board"], image["band"]], [role, board, band])
            names = ["twinoak-%s-%s-%s-v0.3.0.%s" % (role, board, band, ext)
                     for ext in ("bin", "zip", "uf2")]
            if base_check.family_of(board):
                # ... and the base under it, the same file whatever the role (TheBase).
                names.append(base_check.release_name(base_check.load_pin(base_check.family_of(board))))
            self.assertEqual([f["name"] for f in image["files"]], names)

    def test_the_image_requires_the_link_major_it_speaks(self):
        """A Walter of another box-link major is not given this image at all
        (spec/FIRMWARE-MANIFEST.md section 2, `requires`; section 5, step 3)."""
        manifest = self.release()
        self.assertEqual(manifest["images"][0]["requires"],
                         {"link_major": manifest["link"]["major"]})

    def test_it_validates_and_is_canonical(self):
        manifest = self.release()
        manifestref.validate(manifest)
        with open(os.path.join(self.out, "manifest.json"), "rb") as fh:
            self.assertEqual(fh.read(), manifestref.canonical(manifest))

    def test_the_link_version_is_the_firmwares_own_constant(self):
        """`link` is read from src/common/link/codec/Numbers.h - what the firmware speaks - and
        not from a fixture file's name (p3.8-01). The specification must have a fixture file for
        that version saying so, and the firmware may not speak a version the specification's
        newest fixture does not have: that is the firmware ahead of the specification, which is
        drift. The specification ahead of the firmware - a newer fixture in spec/ than the version
        the firmware speaks - is a pile in progress (spec/LINK-PROTOCOL.md section 11 names the
        newest file; the task that implements it moves the constant), not a red release: pile
        3.9's 1.2 arrived in spec/ before its task 02 had run, and task 01 found every release of
        the day red for it (p3.9-01)."""
        path = os.path.join(REPO, build_release.LINK_NUMBERS)
        with open(path, "r", encoding="utf-8") as fh:
            text = fh.read()
        major = int(re.search(r"kProtoMajor\s*=\s*(\d+)", text).group(1))
        minor = int(re.search(r"kProtoMinor\s*=\s*(\d+)", text).group(1))
        self.assertEqual(self.release()["link"], {"major": major, "minor": minor})

        def version_of(path):
            return tuple(int(n) for n in os.path.basename(path)[6:-5].split("."))

        files = glob.glob(os.path.join(REPO, "spec", "fixtures", "link", "link-v*.json"))
        self.assertTrue(files, "no link fixture in spec/fixtures/link/")
        # The fixture of the version the firmware speaks: `link-v1.json` is 1.0's and
        # `link-v1.1.json` is 1.1's (section 11), so a minor of 0 is the file with no minor.
        own_name = "link-v%d.json" % major if minor == 0 else "link-v%d.%d.json" % (major, minor)
        own = os.path.join(REPO, "spec", "fixtures", "link", own_name)
        self.assertTrue(os.path.exists(own),
                        "%s: the firmware speaks %d.%d and the specification has no fixture for it"
                        % (own_name, major, minor))
        with open(own, "r", encoding="utf-8") as fh:
            self.assertEqual(json.load(fh)["link"], "%d.%d" % (major, minor),
                             "%s does not say the version its name does" % own_name)
        newest = max(files, key=version_of)
        self.assertGreaterEqual(version_of(newest), (major, minor),
                                "the firmware speaks %d.%d and the newest fixture is %s: the "
                                "firmware is ahead of the specification" % (major, minor,
                                                                           os.path.basename(newest)))

    def test_the_link_version_is_refused_when_the_constants_are_gone(self):
        """A rename of either constant is a release that stops, not a release that guesses."""
        with tempfile.TemporaryDirectory() as tmp:
            os.makedirs(os.path.join(tmp, os.path.dirname(build_release.LINK_NUMBERS)))
            with open(os.path.join(tmp, build_release.LINK_NUMBERS), "w", encoding="utf-8") as fh:
                fh.write("constexpr uint8_t kProtoMajor = 1;\n")
            with self.assertRaises(build_release.ReleaseError) as caught:
                build_release.link_version(repo=tmp)
            self.assertIn("kProtoMinor", str(caught.exception))

    def test_the_upstream_pin_is_the_builds(self):
        # The build directory's own buildinfo.json, whatever pin that says - not a pin this
        # file restates (tools/pio/buildinfo.py writes it from platformio.ini's [meshcore]).
        self.assertEqual(self.release()["upstream"], UPSTREAM)

    def test_sha256sums_covers_every_file_and_the_manifest(self):
        self.release()
        sums = {}
        with open(os.path.join(self.out, "SHA256SUMS"), "r", encoding="ascii") as fh:
            for line in fh.read().splitlines():
                digest, name = line.split("  ", 1)
                sums[name] = digest
        present = sorted(n for n in os.listdir(self.out) if n != "SHA256SUMS")
        self.assertEqual(sorted(sums), present)
        self.assertIn("manifest.json", sums)
        for name, digest in sums.items():
            with open(os.path.join(self.out, name), "rb") as fh:
                self.assertEqual(hashlib.sha256(fh.read()).hexdigest(), digest, name)

    def test_a_version_the_build_does_not_report_is_refused(self):
        with self.assertRaises(build_release.ReleaseError) as caught:
            self.release(version="9.9.9+deadbee")
        self.assertIn("was built as version", str(caught.exception))

    def test_the_version_the_build_reports_is_the_default(self):
        self.assertEqual(self.release()["version"], "0.3.0+abc1234")
        self.assertEqual(self.release()["tag"], "v0.3.0")

    def test_a_commit_that_is_not_a_commit_is_refused(self):
        with self.assertRaises(build_release.ReleaseError):
            build_release.build_release([self.build_dir()], "abc1234", self.out)

    def test_two_environments_of_different_versions_are_refused(self):
        dirs = [self.build_dir(name="a"), self.build_dir(name="b", version="0.4.0+abc1234")]
        with self.assertRaises(build_release.ReleaseError) as caught:
            self.release(build_dirs=dirs)
        self.assertIn("one release is one version", str(caught.exception))

    def test_a_build_directory_that_was_never_built_is_refused(self):
        with self.assertRaises(build_release.ReleaseError) as caught:
            self.release(build_dirs=[self.tmp])
        self.assertIn("buildinfo.json", str(caught.exception))


# ------------------------------------------------------------------ the descriptor

class TheDescriptor(ReleaseTestCase):
    def image_with(self, descriptor=None, at=0x400, size=4096, twice=False):
        blob = bytearray(filler(size))
        if descriptor is not None:
            blob[at:at + len(descriptor)] = descriptor
            if twice:
                blob[at + 0x400:at + 0x400 + len(descriptor)] = descriptor
        return self.build_dir(runs=[(APP_START, bytes(blob))])

    @staticmethod
    def descriptor(board="rak4630-single", version="0.3.0+abc1234", band="868", link=None):
        # The box link this firmware speaks, read where a release reads it (p3.8-01): a
        # descriptor must agree with the manifest, and the manifest's `link` is this. Writing a
        # minor here by hand would make a bump of the protocol fail this test instead of the
        # thing the test is about.
        link = link or build_release.link_version()
        return manifestref.make_descriptor("repeater", version, board, band,
                                           link["major"], link["minor"])

    def test_a_descriptor_of_another_link_minor_is_refused(self):
        """An image built before a minor bump, released with a manifest that claims the bump:
        the descriptor and the manifest come from the same constant, so they can only differ if
        the image is not the one that was just built."""
        older = dict(build_release.link_version())
        older["minor"] = older["minor"] - 1 if older["minor"] > 0 else 1
        with self.assertRaises(build_release.ReleaseError) as caught:
            self.release(build_dirs=[self.image_with(self.descriptor(link=older))],
                         require_descriptor=True)
        self.assertIn("link_minor", str(caught.exception))

    def test_without_the_flag_an_image_without_one_is_fine(self):
        said = []
        self.release(log=said.append)
        self.assertTrue([line for line in said if "no descriptor yet" in line], said)

    def test_with_the_flag_a_matching_descriptor_passes(self):
        said = []
        self.release(build_dirs=[self.image_with(self.descriptor())], require_descriptor=True,
                     log=said.append)
        self.assertTrue([line for line in said if "descriptor at 0x400 agrees" in line], said)

    def test_with_the_flag_an_image_without_one_is_refused(self):
        with self.assertRaises(build_release.ReleaseError) as caught:
            self.release(require_descriptor=True)
        self.assertIn("0 valid descriptors", str(caught.exception))

    def test_two_descriptors_are_refused(self):
        with self.assertRaises(build_release.ReleaseError) as caught:
            self.release(build_dirs=[self.image_with(self.descriptor(), twice=True)],
                         require_descriptor=True)
        self.assertIn("2 valid descriptors", str(caught.exception))

    def test_a_descriptor_that_disagrees_with_the_manifest_is_refused(self):
        with self.assertRaises(build_release.ReleaseError) as caught:
            self.release(build_dirs=[self.image_with(self.descriptor(board="rak4630-dual-a"))],
                         require_descriptor=True)
        self.assertIn("disagrees with the manifest", str(caught.exception))
        self.assertIn("board", str(caught.exception))

    def test_a_descriptor_of_another_version_is_refused(self):
        with self.assertRaises(build_release.ReleaseError) as caught:
            self.release(build_dirs=[self.image_with(self.descriptor(version="0.2.0+abc1234"))],
                         require_descriptor=True)
        self.assertIn("version", str(caught.exception))


# ------------------------------------------------------------------ what may be signed

class TheSignature(ReleaseTestCase):
    def setUp(self):
        ReleaseTestCase.setUp(self)
        self.test_public = sign_manifest.test_public_key()
        self.pub_file = os.path.join(self.tmp, "release.pub")

    def manifest(self, version="0.0.0-dryrun+abc1234"):
        self.release(build_dirs=[self.build_dir(version=version)])
        return os.path.join(self.out, "manifest.json")

    def test_a_rehearsal_signed_with_the_test_key_verifies(self):
        path = self.manifest()
        public = sign_manifest.sign_manifest(path, use_test_key=True)
        self.assertEqual(public, self.test_public)
        with open(path, "rb") as fh:
            message = fh.read()
        with open(path + ".sig", "r", encoding="ascii") as fh:
            self.assertTrue(manifestref.verify([public], message, fh.read()))

    def test_the_signature_is_one_ascii_line_with_lf(self):
        path = self.manifest()
        sign_manifest.sign_manifest(path, use_test_key=True)
        with open(path + ".sig", "rb") as fh:
            raw = fh.read()
        self.assertTrue(raw.startswith(b"ed25519 "))
        self.assertEqual(raw.count(b"\n"), 1)
        self.assertNotIn(b"\r", raw)

    def test_the_test_key_refuses_a_release_version(self):
        path = self.manifest(version="1.2.3+abc1234")
        with self.assertRaises(sign_manifest.SigningError) as caught:
            sign_manifest.sign_manifest(path, use_test_key=True)
        self.assertIn("which is a release", str(caught.exception))
        self.assertIn("RELEASE_SIGNING_KEY", str(caught.exception))
        self.assertFalse(os.path.exists(path + ".sig"))

    def test_which_versions_are_releases(self):
        for version in ("1.2.3", "0.1.0+abc1234", "10.20.30+a.b"):
            self.assertTrue(sign_manifest.is_release_version(version), version)
        for version in ("0.0.0-dev", "0.0.0-dryrun+abc1234", "1.2.3-rc1", "", None):
            self.assertFalse(sign_manifest.is_release_version(version), version)

    def test_a_tag_with_no_key_is_refused_and_says_how_to_make_one(self):
        path = self.manifest(version="1.2.3+abc1234")
        with self.assertRaises(sign_manifest.SigningError) as caught:
            sign_manifest.sign_manifest(path, environ={}, public_key_file=self.pub_file)
        message = str(caught.exception)
        self.assertIn("RELEASE_SIGNING_KEY is not set", message)
        self.assertIn("manifestref.py keygen", message)
        self.assertIn(os.path.join("keys", "release.pub"), message)
        self.assertFalse(os.path.exists(path + ".sig"))

    def test_the_real_path_signs_and_verifies(self):
        """The tag path end to end, with the test key standing in for the one Thomas makes: the
        key comes from the environment and its public half from the file."""
        self.write(self.pub_file, "# a stand-in for keys/release.pub\n%s\n" % self.test_public.hex())
        path = self.manifest(version="1.2.3+abc1234")
        public = sign_manifest.sign_manifest(
            path, environ={"RELEASE_SIGNING_KEY": manifestref.TEST_PRIVATE.hex()},
            public_key_file=self.pub_file)
        with open(path, "rb") as fh:
            message = fh.read()
        with open(path + ".sig", "r", encoding="ascii") as fh:
            self.assertTrue(manifestref.verify([public], message, fh.read()))

    def test_a_key_that_is_not_the_published_one_is_refused(self):
        other = hashlib.sha256(b"some other key").hexdigest()
        self.write(self.pub_file, "%s\n" % self.test_public.hex())
        path = self.manifest(version="1.2.3+abc1234")
        with self.assertRaises(sign_manifest.SigningError) as caught:
            sign_manifest.sign_manifest(path, environ={"RELEASE_SIGNING_KEY": other},
                                        public_key_file=self.pub_file)
        self.assertIn("is not the key", str(caught.exception))
        self.assertNotIn(other, str(caught.exception))
        self.assertFalse(os.path.exists(path + ".sig"))

    def test_a_missing_public_key_file_says_how_to_make_the_key(self):
        path = self.manifest(version="1.2.3+abc1234")
        with self.assertRaises(sign_manifest.SigningError) as caught:
            sign_manifest.sign_manifest(
                path, environ={"RELEASE_SIGNING_KEY": manifestref.TEST_PRIVATE.hex()},
                public_key_file=self.pub_file)
        self.assertIn("is not there", str(caught.exception))
        self.assertIn("manifestref.py keygen", str(caught.exception))

    def test_no_message_ever_carries_the_key(self):
        path = self.manifest()
        for value in ("nonsense", "00" * 31, manifestref.TEST_PRIVATE.hex() + "ff"):
            with self.assertRaises(sign_manifest.SigningError) as caught:
                sign_manifest.sign_manifest(path, environ={"RELEASE_SIGNING_KEY": value},
                                            public_key_file=self.pub_file)
            self.assertNotIn(value, str(caught.exception))

    def test_the_real_path_takes_the_trust_tables_line_for_the_public_half(self):
        """What keys/release.pub actually holds: `ed25519 <key id> <64 hex>`, the line the Walter
        firmware's trust table carries for the same key. Before p3.7-01 the tag path read this
        file and refused it, after the builds (tasks/phase-3.7/01-the-first-release.md)."""
        self.write(self.pub_file, "ed25519 %s %s\n"
                   % (manifestref.key_id(self.test_public), self.test_public.hex()))
        path = self.manifest(version="1.2.3+abc1234")
        public = sign_manifest.sign_manifest(
            path, environ={"RELEASE_SIGNING_KEY": manifestref.TEST_PRIVATE.hex()},
            public_key_file=self.pub_file)
        self.assertEqual(public, self.test_public)
        with open(path, "rb") as fh:
            message = fh.read()
        with open(path + ".sig", "r", encoding="ascii") as fh:
            self.assertTrue(manifestref.verify([public], message, fh.read()))


# ------------------------------------------------------------- the public half, as it is written

class ThePublishedPublicHalf(ReleaseTestCase):
    """keys/release.pub, in the one form both repositories write a key in (keys/README.md):
    `ed25519 <key id> <64 hex>` - the trust table's line - or the 64 hexadecimal characters alone.
    The key id is recomputed from the key, so the two halves of that line cannot disagree."""

    def setUp(self):
        ReleaseTestCase.setUp(self)
        self.test_public = sign_manifest.test_public_key()
        self.pub_file = os.path.join(self.tmp, "release.pub")

    def test_the_key_alone_is_a_key(self):
        self.write(self.pub_file, "%s\n" % self.test_public.hex())
        self.assertEqual(sign_manifest.read_public_key(self.pub_file), self.test_public)

    def test_the_trust_tables_line_is_a_key(self):
        self.write(self.pub_file, "ed25519 %s %s\n"
                   % (manifestref.key_id(self.test_public), self.test_public.hex()))
        self.assertEqual(sign_manifest.read_public_key(self.pub_file), self.test_public)

    def test_an_upper_case_key_id_is_the_same_key_id(self):
        self.write(self.pub_file, "ed25519 %s %s\n"
                   % (manifestref.key_id(self.test_public).upper(), self.test_public.hex()))
        self.assertEqual(sign_manifest.read_public_key(self.pub_file), self.test_public)

    def test_a_key_id_that_is_not_the_keys_own_is_refused(self):
        """The id is recomputed, so a hand-edited half is caught: the message names both ids -
        a key id is public, a key is not - and never the key."""
        self.write(self.pub_file, "ed25519 %s %s\n" % ("0" * 16, self.test_public.hex()))
        with self.assertRaises(sign_manifest.SigningError) as caught:
            sign_manifest.read_public_key(self.pub_file)
        message = str(caught.exception)
        self.assertIn("0" * 16, message)
        self.assertIn(manifestref.key_id(self.test_public), message)
        self.assertNotIn(self.test_public.hex(), message)

    def test_the_public_key_file_takes_comments(self):
        self.write(self.pub_file, "# who made it, and when\n\n%s\n" % self.test_public.hex())
        self.assertEqual(sign_manifest.read_public_key(self.pub_file), self.test_public)

    def test_a_public_key_file_that_is_not_a_key_is_refused(self):
        for content in ("not a key\n",                      # three fields, not a signature line
                        "ed25519 %s\n" % self.test_public.hex(),          # two fields
                        "ssh-ed25519 %s %s\n" % (manifestref.key_id(self.test_public),
                                                 self.test_public.hex()),  # another algorithm
                        "%s\n" % self.test_public.hex()[:-1],             # 63 characters
                        "%szz\n" % self.test_public.hex()[:-2]):          # not hexadecimal
            self.write(self.pub_file, content)
            with self.assertRaises(sign_manifest.SigningError):
                sign_manifest.read_public_key(self.pub_file)

    def test_a_missing_file_and_an_empty_one_both_say_how_to_make_the_key(self):
        with self.assertRaises(sign_manifest.SigningError) as caught:
            sign_manifest.read_public_key(self.pub_file)
        self.assertIn("manifestref.py keygen", str(caught.exception))
        self.write(self.pub_file, "# nothing but a comment\n\n")
        with self.assertRaises(sign_manifest.SigningError) as caught:
            sign_manifest.read_public_key(self.pub_file)
        self.assertIn("manifestref.py keygen", str(caught.exception))

    def test_the_committed_file_is_the_key_thomas_made(self):
        """The **real** keys/release.pub, with the reader the tag path uses: key id
        73efdd383a957596, made on 20 October 2026 and shared with the Walter firmware
        (keys/README.md). tools/release/selfcheck.py runs this reader over the same file on every
        commit, so a malformed file fails on a commit and not at a tag."""
        public = sign_manifest.read_public_key(
            os.path.join(REPO, sign_manifest.PUBLIC_KEY_FILE))
        self.assertEqual(len(public), 32)
        self.assertEqual(manifestref.key_id(public), "73efdd383a957596")


# ------------------------------------------------------- the hand-written top of a release body

class TheHandWrittenNotes(ReleaseTestCase):
    """release-notes/vX.Y.Z.md: what a person wrote, in front of what the manifest generates
    (tools/release/release_notes.py). A tag without it is refused in release.yml's ci-green, the
    way a tag without a signing key is; a rehearsal has none and says so."""

    VERSION = "1.2.3+abc1234"

    def notes_dir(self):
        path = os.path.join(self.tmp, release_notes.NOTES_DIR)
        os.makedirs(path, exist_ok=True)
        return path

    @staticmethod
    def command_line(args):
        """release_notes.py's main, with what it printed instead of the console."""
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = release_notes.main(args)
        return code, out.getvalue(), err.getvalue()

    def test_the_file_is_named_by_the_tag_and_not_by_the_build_metadata(self):
        self.assertEqual(release_notes.tag_of("1.2.3+abc1234"), "v1.2.3")
        self.assertEqual(release_notes.tag_of("1.2.3"), "v1.2.3")
        self.assertEqual(release_notes.tag_of("0.0.0-dryrun+abc1234"), "v0.0.0")
        self.assertEqual(release_notes.tag_of("10.20.30+a.b"), "v10.20.30")
        self.assertTrue(release_notes.notes_path(self.VERSION, self.tmp).endswith(
            os.path.join(release_notes.NOTES_DIR, "v1.2.3.md")))

    def test_a_version_that_is_not_one_is_refused(self):
        for version in ("", None, "v1.2.3", "1.2", "latest"):
            with self.assertRaises(release_notes.NotesError):
                release_notes.tag_of(version)

    def test_the_top_is_the_files_text_with_a_divider_under_it(self):
        self.write(os.path.join(self.notes_dir(), "v1.2.3.md"), "**The first release.**\n")
        text = release_notes.hand_written(self.VERSION, self.tmp)
        self.assertTrue(text.startswith("**The first release.**\n"))
        self.assertTrue(text.endswith("\n---\n\n"))
        # A `---` directly under a paragraph is a Markdown heading: there is a blank line above it.
        self.assertIn("\n\n---\n", text)

    def test_a_release_with_no_hand_written_top_is_refused_and_says_what_to_write(self):
        with self.assertRaises(release_notes.NotesError) as caught:
            release_notes.hand_written(self.VERSION, self.tmp)
        message = str(caught.exception)
        self.assertIn("release-notes/v1.2.3.md", message)
        self.assertIn("docs/flashing.md", message)
        self.assertIn("not released", message)
        code, out, err = self.command_line(["--version", self.VERSION, "--require",
                                            "--repo", self.tmp])
        self.assertEqual((code, out), (1, ""))
        self.assertIn("release-notes/v1.2.3.md", err)

    def test_an_empty_hand_written_top_is_refused_too(self):
        self.write(os.path.join(self.notes_dir(), "v1.2.3.md"), "\n   \n")
        with self.assertRaises(release_notes.NotesError) as caught:
            release_notes.hand_written(self.VERSION, self.tmp)
        self.assertIn("holds nothing", str(caught.exception))

    def test_a_rehearsal_is_not_refused_and_says_there_is_no_top(self):
        text = release_notes.no_hand_written("0.0.0-dryrun+abc1234", self.tmp)
        self.assertIn("release-notes/v0.0.0.md", text)
        self.assertIn("No hand-written top", text)
        self.assertTrue(text.endswith("\n---\n\n"))
        code, out, err = self.command_line(["--version", "0.0.0-dryrun+abc1234",
                                            "--repo", self.tmp])
        self.assertEqual((code, err, out), (0, "", text))

    def test_the_check_the_tag_job_runs_prints_the_path_and_not_the_body(self):
        self.write(os.path.join(self.notes_dir(), "v1.2.3.md"), "**The first release.**\n")
        code, out, err = self.command_line(["--version", self.VERSION, "--require", "--check",
                                            "--repo", self.tmp])
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(out.strip(), "release-notes/v1.2.3.md")
        self.assertNotIn("The first release", out)

    def test_the_script_writes_utf_8_whatever_the_console_is(self):
        """The notes become a release body, which GitHub reads as UTF-8. On Windows a plain
        sys.stdout.write would have put cp1252 in the file (an em dash as 0x97), so the script
        writes the bytes itself - and this runs it as a process, because that is the only way the
        encoding of its stdout is the real one."""
        em_dash, nbsp = "\u2014", "\u00a0"
        self.write(os.path.join(self.notes_dir(), "v1.2.3.md"),
                   "**The first release** %s an em dash, and a non-breaking%sspace.\n"
                   % (em_dash, nbsp))
        raw = subprocess.check_output(
            [sys.executable, os.path.join(HERE, "release_notes.py"),
             "--version", self.VERSION, "--require", "--repo", self.tmp])
        self.assertIn(em_dash.encode("utf-8"), raw)
        self.assertIn(nbsp.encode("utf-8"), raw)
        self.assertEqual(raw.decode("utf-8"), release_notes.hand_written(self.VERSION, self.tmp))

    def test_this_commits_own_notes_are_readable(self):
        """Every release-notes/*.md of this repository: a version this tooling understands, and
        something in it. The file is written on the branch before the tag, so this is where a
        typo in its name is caught - on a commit, and not at the tag."""
        directory = os.path.join(REPO, release_notes.NOTES_DIR)
        if not os.path.isdir(directory):
            self.skipTest("no release-notes/ in this repository yet")
        names = sorted(n for n in os.listdir(directory) if n.endswith(".md"))
        for name in names:
            version = name[1:-3] if name.startswith("v") else name[:-3]
            self.assertEqual(release_notes.tag_of(version), name[:-3],
                             "%s is not named vX.Y.Z.md" % name)
            text = release_notes.hand_written(version, REPO)
            self.assertIn("docs/flashing.md", text,
                          "%s does not point a reader at the flashing guide" % name)


# ------------------------------------------------------------------ the base under the application

class TheBase(ReleaseTestCase):
    """boards/base/<family>/ in every release of that family, held to its pin (tools/base/check.py)
    and to spec/FIRMWARE-MANIFEST.md section 4a. The real vendored file is used throughout: the
    pin and the file are the thing under test, and a stand-in would prove nothing about them."""

    FAMILY = "rak4630"

    def setUp(self):
        ReleaseTestCase.setUp(self)
        self.pin = base_check.load_pin(self.FAMILY)
        self.assertIsNotNone(self.pin, "boards/base/%s/base.json is missing" % self.FAMILY)
        self.data = base_check.read_base(self.pin)
        with open(os.path.join(FIXTURES, "index.json"), "r", encoding="utf-8") as fh:
            self.index = json.load(fh)

    def base_dir(self, data=None, sha256=None, summary=None):
        """A boards/base look-alike under the temporary directory: the real pin and README, and
        the file (or another), with the pin's hash (or another) - so a test can change one byte,
        or one number, and see what refuses it."""
        root = os.path.join(self.tmp, "base")
        family = os.path.join(root, self.FAMILY)
        os.makedirs(family)
        pin = {k: v for k, v in self.pin.items() if not k.startswith("_")}
        data = self.data if data is None else data
        pin["sha256"] = sha256 or hashlib.sha256(data).hexdigest()
        pin["size"] = len(data)
        if summary is not None:
            pin["summary"] = summary
        self.write(os.path.join(family, base_check.PIN_FILE), json.dumps(pin, indent=2) + "\n")
        self.write(os.path.join(family, pin["file"]), data)
        shutil.copyfile(os.path.join(self.pin["_dir"], base_check.README_FILE),
                        os.path.join(family, base_check.README_FILE))
        return root

    def fixture_base(self, name):
        with open(os.path.join(FIXTURES, name), "rb") as fh:
            return fh.read()

    def why(self, name):
        """The words the specification's index gives for refusing one of its base fixtures."""
        return [b for b in self.index["provision"]["bases"] if b["file"] == name][0]["why"]

    def provision_files(self, image):
        return [f for f in image["files"] if f["use"] == "provision"]

    # --- the real file, the pin and the README, on this commit

    def test_the_vendored_file_is_the_pinned_one_and_passes_against_the_linker_script(self):
        """The acceptance criterion: the hash is the README's, section 4a accepts it against the
        region the release reads from the linker script, and the summary is the README's."""
        region = build_release.read_app_region(os.path.join(self.build_dir(), "ldscript.ld"))
        summary = base_check.check_base(self.pin, self.data, region.start, region.end)
        self.assertEqual(summary, self.pin["summary"])
        self.assertEqual(summary["softdevice"], "S140 6.1.1")
        self.assertTrue(base_check.check_readme(self.pin).endswith(base_check.README_FILE))
        self.assertEqual(hashlib.sha256(self.data).hexdigest(), self.pin["sha256"])

    def test_the_release_name_carries_the_bootloader_version_and_the_softdevice(self):
        self.assertEqual(base_check.release_name(self.pin),
                         "twinoak-base-rak4630-%s-s140-6.1.1.hex" % self.pin["bootloader"]["version"])

    def test_a_readme_that_says_other_numbers_is_refused(self):
        root = self.base_dir()
        readme = os.path.join(root, self.FAMILY, base_check.README_FILE)
        with open(readme, "r", encoding="utf-8") as fh:
            text = fh.read()
        self.write(readme, text.replace(self.pin["sha256"], "0" * 64))
        with self.assertRaises(base_check.BaseError) as caught:
            base_check.check_readme(base_check.load_pin(self.FAMILY, root))
        self.assertIn("SHA-256", str(caught.exception))

    # --- in a release

    def test_every_rak4630_image_carries_the_base_and_the_manifest_validates(self):
        """The dry run's manifest: one provision file per rak4630-* image, the same file under
        each (one hash), named as the task says, beside the manifest, and manifestref.validate
        passes on the whole."""
        dirs = [self.build_dir(name="repeater", role="repeater"),
                self.build_dir(name="scanner", role="scanner")]
        manifest = self.release(build_dirs=dirs)
        manifestref.validate(manifest)
        name = base_check.release_name(self.pin)
        for image in manifest["images"]:
            files = self.provision_files(image)
            self.assertEqual(len(files), 1, image["id"])
            self.assertEqual(files[0]["name"], name)
            self.assertEqual(files[0]["format"], "hex")
            self.assertEqual(files[0]["sha256"], self.pin["sha256"])
            self.assertEqual(files[0]["size"], self.pin["size"])
            self.assertNotIn("load_address", files[0])
        with open(os.path.join(self.out, name), "rb") as fh:
            self.assertEqual(fh.read(), self.data)
        with open(os.path.join(self.out, "SHA256SUMS"), "r", encoding="ascii") as fh:
            self.assertIn("%s  %s\n" % (self.pin["sha256"], name), fh.read())

    def test_the_provision_entry_has_the_shape_of_the_specifications_fixture(self):
        with open(os.path.join(FIXTURES, "manifest-repeater-base.json"), "r", encoding="utf-8") as fh:
            fixture = json.load(fh)
        want = [f for f in fixture["images"][0]["files"] if f["use"] == "provision"][0]
        got = self.provision_files(self.release()["images"][0])[0]
        self.assertEqual(list(got), list(want))

    def test_a_board_of_no_family_gets_no_base(self):
        manifest = self.release(build_dirs=[self.build_dir(board="walter")])
        self.assertEqual(self.provision_files(manifest["images"][0]), [])

    def test_the_notices_are_in_the_release(self):
        self.release()
        with open(os.path.join(REPO, build_release.NOTICES), "rb") as want:
            with open(os.path.join(self.out, build_release.NOTICES), "rb") as got:
                self.assertEqual(got.read(), want.read())
        with open(os.path.join(self.out, "SHA256SUMS"), "r", encoding="ascii") as fh:
            self.assertIn("  %s\n" % build_release.NOTICES, fh.read())

    # --- what refuses a release

    def test_a_changed_byte_is_refused_with_the_hash(self):
        changed = bytearray(self.data)
        changed[len(changed) // 2] ^= 0x01
        root = self.base_dir(data=bytes(changed), sha256=self.pin["sha256"])
        with self.assertRaises(build_release.ReleaseError) as caught:
            self.release(base_dir=root)
        message = str(caught.exception)
        self.assertIn("not the pinned base", message)
        self.assertIn(self.pin["sha256"], message)
        self.assertIn(hashlib.sha256(bytes(changed)).hexdigest(), message)
        self.assertFalse(os.path.exists(os.path.join(self.out, "manifest.json")))

    def test_a_bootloader_only_file_in_its_place_is_refused_in_the_specifications_words(self):
        """The first board's reports warned against a bootloader-only file; section 4a refuses it
        with these words, and so does the release (the pin's hash is made to match, so that it is
        the rule and not the hash that refuses)."""
        only = self.fixture_base("base-bootloader-only.hex")
        root = self.base_dir(data=only)
        with self.assertRaises(build_release.ReleaseError) as caught:
            self.release(base_dir=root)
        self.assertIn(self.why("base-bootloader-only.hex"), str(caught.exception))
        self.assertIn("section 4a", str(caught.exception))

    def test_a_file_that_writes_approtect_is_refused_in_the_specifications_words(self):
        root = self.base_dir(data=self.fixture_base("base-sets-approtect.hex"))
        with self.assertRaises(build_release.ReleaseError) as caught:
            self.release(base_dir=root)
        self.assertIn(self.why("base-sets-approtect.hex"), str(caught.exception))

    def test_a_pin_whose_summary_differs_from_the_file_is_refused(self):
        summary = dict(self.pin["summary"], bootloader="0x000F0000")
        root = self.base_dir(summary=summary)
        with self.assertRaises(build_release.ReleaseError) as caught:
            self.release(base_dir=root)
        self.assertIn("not what base.json says", str(caught.exception))

    def test_a_family_board_with_no_pinned_base_is_refused(self):
        empty = os.path.join(self.tmp, "nobase")
        os.makedirs(empty)
        with self.assertRaises(build_release.ReleaseError) as caught:
            self.release(base_dir=empty)
        self.assertIn("no base is pinned", str(caught.exception))
        self.assertIn("S-5a", str(caught.exception))

    def test_a_manifest_without_the_base_is_refused_before_signing(self):
        """A manifest that lost its provision entry after the build - edited, or built by
        something else - is not signed: no key is touched and no .sig is written."""
        self.release(build_dirs=[self.build_dir(version="0.0.0-dryrun+abc1234")])
        path = os.path.join(self.out, "manifest.json")
        with open(path, "r", encoding="utf-8") as fh:
            manifest = json.load(fh)
        for image in manifest["images"]:
            image["files"] = [f for f in image["files"] if f["use"] != "provision"]
        with open(path, "wb") as fh:
            fh.write(manifestref.canonical(manifest))
        with self.assertRaises(sign_manifest.SigningError) as caught:
            sign_manifest.sign_manifest(path, use_test_key=True)
        self.assertIn("no valid base", str(caught.exception))
        self.assertIn("0 provision file(s)", str(caught.exception))
        self.assertFalse(os.path.exists(path + ".sig"))

    def test_a_base_beside_the_manifest_that_is_not_the_pinned_one_is_refused_before_signing(self):
        self.release(build_dirs=[self.build_dir(version="0.0.0-dryrun+abc1234")])
        path = os.path.join(self.out, "manifest.json")
        with open(os.path.join(self.out, base_check.release_name(self.pin)), "ab") as fh:
            fh.write(b"\n")
        with self.assertRaises(sign_manifest.SigningError) as caught:
            sign_manifest.sign_manifest(path, use_test_key=True)
        self.assertIn("not the pinned base", str(caught.exception))
        self.assertFalse(os.path.exists(path + ".sig"))

    def test_a_manifest_with_the_base_signs(self):
        self.release(build_dirs=[self.build_dir(version="0.0.0-dryrun+abc1234")])
        path = os.path.join(self.out, "manifest.json")
        said = []
        sign_manifest.sign_manifest(path, use_test_key=True, log=said.append)
        self.assertTrue(os.path.exists(path + ".sig"))
        self.assertTrue([line for line in said if "the base is the pinned one" in line], said)

    @staticmethod
    def command_line(args):
        """tools/base/check.py's main, with what it printed instead of the console."""
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = base_check.main(args)
        return code, out.getvalue(), err.getvalue()

    def test_the_command_line_check_passes_on_this_commit(self):
        ldscript = os.path.join(self.build_dir(), "ldscript.ld")
        code, out, err = self.command_line(["--ldscript", ldscript])
        self.assertEqual((code, err), (0, ""))
        self.assertIn("passes section 4a", out)
        self.assertIn("says the same", out)
        self.assertEqual(self.command_line(["--region", "0x26000", "0xED000"])[0], 0)

    def test_the_command_line_check_fails_on_a_changed_file(self):
        changed = bytearray(self.data)
        changed[-10] ^= 0x01
        root = self.base_dir(data=bytes(changed), sha256=self.pin["sha256"])
        code, out, err = self.command_line(["--region", "0x26000", "0xED000", "--base-dir", root])
        self.assertEqual(code, 1)
        self.assertIn("not the pinned base", err)
        self.assertIn(self.pin["sha256"], err)


if __name__ == "__main__":
    unittest.main(verbosity=2)
