# What the build knows about itself, written beside the images it leaves.
#
# The release tooling (tools/release/) must not be told by hand what a build is: the role, the
# board, the band, the MCU, the version baked into the image, the MeshCore pin it was built on
# and - the one the task is strict about - the linker script that decided where the application
# lives. All of it is in the SCons environment at link time and nowhere else, so this post-script
# writes it into the build directory:
#
#   $BUILD_DIR/buildinfo.json   the facts, as JSON
#   $BUILD_DIR/ldscript.ld      a byte copy of the linker script the link command used
#
# The copy is there because the release is assembled on a machine that may not be the one that
# linked it (in CI the publish job downloads the build job's artifact, and the toolchain package
# is not in it). What is copied is the file env.GetActualLDScript() resolved - the script the
# build actually used - so reading the copy is reading that script and not a transcription of it
# (spec/FIRMWARE-MANIFEST.md section 2: "The real numbers come from the firmware's own linker
# script").
#
# This is a post-script on purpose: LINKFLAGS, and therefore the linker script, are set by the
# platform's own build script, which runs before post: and after pre:.

import json
import os
import shutil

Import("env")  # noqa: F821  (SCons injects it)


def define(name, default=None):
    """The value of a -D <name>='"text"' build flag as plain text.

    A string define reaches SCons still wearing the quoting the compiler needs - PlatformIO
    keeps it as '\\"repeater\\"' - and the manifest wants the three characters inside.
    """
    for item in env.get("CPPDEFINES", []):
        if isinstance(item, (list, tuple)) and len(item) == 2 and item[0] == name:
            value = str(env.subst(str(item[1]))).strip()
            for quote in ('\\"', '"', "'"):
                if len(value) >= 2 * len(quote) and value.startswith(quote) and value.endswith(quote):
                    return value[len(quote):-len(quote)]
            return value
        if item == name:
            return ""
    return default


ldscript = env.GetActualLDScript()
if not ldscript or not os.path.isfile(ldscript):
    raise Exception(
        "the linker script of this build was not found (%r); the release tooling reads the "
        "application region from it (spec/FIRMWARE-MANIFEST.md section 2)" % (ldscript,)
    )

build_dir = env.subst("$BUILD_DIR")
if not os.path.isdir(build_dir):
    os.makedirs(build_dir)
shutil.copyfile(ldscript, os.path.join(build_dir, "ldscript.ld"))

info = {
    "about": "Written by tools/pio/buildinfo.py while this environment was built. "
             "Read by tools/release/build_release.py; never edited by hand.",
    "env": env.subst("$PIOENV"),
    "progname": env.subst("$PROGNAME"),
    # The three strings a manifest entry and the image descriptor are made of
    # (spec/FIRMWARE-MANIFEST.md sections 2 and 4), as platformio.ini defines them.
    "role": define("TWINOAK_ROLE"),
    "board": define("TWINOAK_BOARD"),
    "band": define("TWINOAK_BAND"),
    "mcu": env.BoardConfig().get("build.mcu"),
    # The version the image itself reports. src/version.h has the same default for a build that
    # no release passed TWINOAK_FW_VERSION to; the release tooling refuses to publish a version
    # other than this one.
    "version": define("TWINOAK_FW_VERSION", "0.0.0-dev"),
    "upstream": {
        "name": "MeshCore",
        "tag": define("TWINOAK_MESHCORE_TAG"),
        "commit": define("TWINOAK_MESHCORE_PIN"),
    },
    "ldscript_name": os.path.basename(ldscript),
    "ldscript_source": ldscript.replace("\\", "/"),
    "ldscript_copy": "ldscript.ld",
}

missing = [k for k in ("role", "board", "band", "mcu") if not info[k]]
if missing:
    raise Exception(
        "this environment defines no %s; a manifest entry cannot be written without it "
        "(platformio.ini: TWINOAK_ROLE, TWINOAK_BOARD, TWINOAK_BAND)" % ", ".join(missing)
    )

with open(os.path.join(build_dir, "buildinfo.json"), "w", newline="\n") as fh:
    fh.write(json.dumps(info, indent=2) + "\n")
