# The post-build check: the raw application binary of this build holds **exactly one** valid
# image descriptor, and its fields are this environment's.
#
# spec/FIRMWARE-MANIFEST.md section 4 is the specification and spec/tools/manifestref.py is that
# document as code; the finding and the parsing here are *imported* from it - the same function
# the Walter's own tests and the release tooling use - so nothing re-implements a descriptor.
#
# It runs on every build of every board environment, which is how it is part of tools/verify.ps1
# without a step of its own: a build whose descriptor is missing, doubled, broken or for another
# board does not succeed. That is the only place it can be caught before a release, because the
# descriptor is placed by the compiler (src/common/fwdesc/ImageDescriptor.cpp) and a wrong one
# is a wrong image, not a wrong manifest.
#
# It also leaves the raw binary beside the other images:
#
#   $BUILD_DIR/firmware.bin   the bytes the Walter writes over SWD, cut out of the .hex exactly
#                             as tools/release/build_release.py cuts them - so a reviewer can run
#                             `python spec/tools/manifestref.py descriptor <file>` on the very
#                             bytes that were checked.

import os
import sys

Import("env")  # noqa: F821  (SCons injects it)

# A SCons script has no __file__; $PROJECT_DIR is this repository's root.
REPO = env.subst("$PROJECT_DIR")  # noqa: F821
for path in (os.path.join(REPO, "spec", "tools"), os.path.join(REPO, "tools", "release")):
    if path not in sys.path:
        sys.path.insert(0, path)

import build_release  # noqa: E402  (after sys.path)
import manifestref  # noqa: E402


def define(name, default=None):
    """The value of a -D <name>='"text"' build flag as plain text (tools/pio/buildinfo.py's)."""
    for item in env.get("CPPDEFINES", []):  # noqa: F821
        if isinstance(item, (list, tuple)) and len(item) == 2 and item[0] == name:
            value = str(env.subst(str(item[1]))).strip()  # noqa: F821
            for quote in ('\\"', '"', "'"):
                if len(value) >= 2 * len(quote) and value.startswith(quote) and value.endswith(quote):
                    return value[len(quote):-len(quote)]
            return value
        if item == name:
            return ""
    return default


firmware_hex = os.path.join("$BUILD_DIR", "${PROGNAME}.hex")


def check_descriptor(source, target, env):  # noqa: ARG001 (SCons' signature)
    hex_path = env.subst(firmware_hex)
    bin_path = env.subst(os.path.join("$BUILD_DIR", "${PROGNAME}.bin"))
    with open(hex_path, "r", encoding="ascii", errors="replace") as fh:
        runs = build_release.parse_intel_hex(fh.read(), os.path.basename(hex_path))
    if len(runs) != 1:
        raise Exception(
            "%s is not one contiguous span (%d runs): the raw binary a flasher writes could not "
            "be cut from it, so the descriptor cannot be checked either"
            % (os.path.basename(hex_path), len(runs))
        )
    application = runs[0][1]
    with open(bin_path, "wb") as fh:
        fh.write(application)

    try:
        offset, fields = manifestref.find_descriptor(application)
    except ValueError as exc:
        raise Exception(
            "the image this build produced does not carry exactly one valid descriptor: %s. "
            "src/common/fwdesc/ImageDescriptor.cpp places it and the firmware references it "
            "through imageFields(); spec/FIRMWARE-MANIFEST.md section 4 is the format." % exc
        )

    expected = {
        "role": define("TWINOAK_ROLE"),
        "board": define("TWINOAK_BOARD"),
        "band": define("TWINOAK_BAND"),
        "version": define("TWINOAK_FW_VERSION", "0.0.0-dev"),
    }
    wrong = [
        "%s is %r in the image and %r in this environment" % (key, fields.get(key), want)
        for key, want in sorted(expected.items())
        if fields.get(key) != want
    ]
    if wrong:
        raise Exception(
            "the descriptor at 0x%X is not this environment's: %s. The three names come from one "
            "place (platformio.ini's [img_*] section) and everything reads them from there."
            % (offset, "; ".join(wrong))
        )

    print(
        "descriptor: one, at 0x%X of %s - %s %s %s, version %s, slot %s, link %d.%d"
        % (offset, os.path.basename(bin_path), fields["role"], fields["board"], fields["band"],
           fields["version"], fields["slot"], fields["link_major"], fields["link_minor"])
    )


env.AddPostAction(firmware_hex, check_descriptor)  # noqa: F821
