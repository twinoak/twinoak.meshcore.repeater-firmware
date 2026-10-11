# The UF2 image, for a consumer of the package.
#
# MeshCore attaches create-uf2.py to its own project ([nrf52_base].extra_scripts): a custom
# target that runs "$PROJECT_DIR/bin/uf2conv/uf2conv.py" over the .hex. A consumer's
# $PROJECT_DIR has no bin/uf2conv, and a script inside a dependency cannot be named in
# extra_scripts, so the same conversion is done here with the converter read straight out of the
# pinned checkout - nothing copied - and as a post-action on every build rather than a target to
# remember: the images a build leaves are then
#
#   firmware.hex   what the Walter flashes over SWD (and a J-Link or OpenOCD at a bench)
#   firmware.zip   the DFU package for adafruit-nrfutil over the adapter's USB-C (the
#                  nordicnrf52 platform makes it itself for a board whose upload protocol is
#                  nrfutil, from the same .hex)
#   firmware.uf2   for the bootloader's drag-and-drop drive, made here
#   firmware.elf   for the debugger and for the release tooling (phase 0's task 02)
#
# The family id 0xADA52840 is the nRF52840's, as upstream's create-uf2.py passes it. A pin bump
# that moves or renames bin/uf2conv/uf2conv.py fails here, loudly - docs/upstream.md lists this
# file among the places a bump re-reads.

import os

Import("env")  # noqa: F821  (SCons injects it)

meshcore = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"), "MeshCore")
uf2conv = os.path.join(meshcore, "bin", "uf2conv", "uf2conv.py")

if not os.path.isfile(uf2conv):
    raise Exception(
        "MeshCore's bin/uf2conv/uf2conv.py is not at %s - has the pin moved it? (docs/upstream.md)"
        % uf2conv
    )

firmware_hex = os.path.join("$BUILD_DIR", "${PROGNAME}.hex")
firmware_uf2 = os.path.join("$BUILD_DIR", "${PROGNAME}.uf2")


def create_uf2(source, target, env):  # noqa: ARG001 (SCons' signature)
    cmd = " ".join(
        [
            '"$PYTHONEXE"',
            '"%s"' % uf2conv,
            "-f",
            "0xADA52840",
            "-c",
            '"%s"' % firmware_hex,
            "-o",
            '"%s"' % firmware_uf2,
        ]
    )
    if env.Execute(cmd) != 0:
        raise Exception("uf2conv failed on %s" % env.subst(firmware_hex))


env.AddPostAction(firmware_hex, create_uf2)
