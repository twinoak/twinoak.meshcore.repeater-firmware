# MeshCore's RAK4631 variant sources, for a consumer that brings its own target.cpp.
#
# Upstream selects a board with -D MC_VARIANT=<name>, and build_as_lib.py then compiles every
# .cpp of variants/<name>/ into the MeshCore library: the board object (RAK4631Board.cpp), the
# Arduino pin map (variant.cpp) *and* target.cpp - the file that constructs `board`, the radio,
# the clock and the sensors for a WisBlock base with a GPS on Serial1. On our adapter Serial1 is
# the link to the Walter and the battery is read by an INA3221, so target.cpp is ours
# (src/common/board/target.cpp, docs/ARCHITECTURE.md section 3) and MC_VARIANT is deliberately not set.
#
# The two files of the variant that are still upstream's are compiled here, out of the pinned
# checkout, never copied - the way meshcore_private_libs.py compiles lib/ed25519. variant.cpp is
# needed because the Adafruit core at MeshCore's pin has no WisCore_RAK4631_Board variant of its
# own: the pin map comes from MeshCore's tree in upstream's build and in ours.
#
# A post-script, on purpose. A pre-script runs before the platform has configured the toolchain
# and the core's include paths, so nothing compiled from one would build; a post-script sees the
# finished environment. But by then the program's source list is closed (BuildSources refuses), so
# the two files are compiled into a small static library that is added to LIBS - which the link
# reads when it runs, inside the --start-group/--end-group PlatformIO already puts around every
# library, so the order does not matter. The global environment knows the core's include paths but
# not the libraries' (those belong to the project environment), and RAK4631Board.cpp includes
# Wire.h, a library of the Adafruit core, and MeshCore's own headers: a clone of the environment
# gets exactly those two directories. If a bump makes either file include more, the compile says
# which header it could not find and this is the list to extend. The clone also leaves our own
# -Werror (build_src_flags) where it belongs - on src/ - and not on upstream's files.
#
# **A role that compiles no MeshCore gets variant.cpp and nothing else.** The scanner
# (src/roles/scanner/, phase 3's task 02) has no mesh and no board object, but it is the same
# stamp and needs the same Arduino pin map, so it takes variant.cpp out of the same checkout and
# leaves RAK4631Board.cpp - which is MeshCore - behind. The build says which it is by whether it
# ignores the MeshCore library at all (platformio.ini: lib_ignore), because that is the same
# switch that decides whether upstream's own sources are compiled.
#
# This script fails loudly, with docs/upstream.md's name in the message, when the pin moves or
# renames what it reaches for, or when MC_VARIANT is set after all (two `board`s, two targets).

import os

Import("env")  # noqa: F821  (SCons injects it)

VARIANT = "rak4631"
# The Arduino pin map, which every role of this board needs, and upstream's board object, which
# only a role that compiles MeshCore can use.
VARIANT_SOURCES = ("variant.cpp",)
MESHCORE_SOURCES = ("RAK4631Board.cpp",)

meshcore = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"), "MeshCore")
variant_dir = os.path.join(meshcore, "variants", VARIANT)

for item in env.get("CPPDEFINES", []):
    name = item[0] if isinstance(item, (list, tuple)) else item
    if name == "MC_VARIANT":
        raise Exception(
            "MC_VARIANT is set, so build_as_lib.py would compile upstream's variants/%s/target.cpp "
            "beside our src/common/board/target.cpp; remove the define (docs/ARCHITECTURE.md section 3)"
            % VARIANT
        )

ignored = env.GetProjectOption("lib_ignore", [])
if isinstance(ignored, str):
    ignored = ignored.split()
meshcore_in_build = "MeshCore" not in [name.strip() for name in ignored]

sources = VARIANT_SOURCES + (MESHCORE_SOURCES if meshcore_in_build else ())

for name in sources:
    if not os.path.isfile(os.path.join(variant_dir, name)):
        raise Exception(
            "MeshCore's variants/%s/%s is not at %s - has the pin moved it? (docs/upstream.md)"
            % (VARIANT, name, variant_dir)
        )

framework = env.PioPlatform().get_package_dir("framework-arduinoadafruitnrf52")
wire = os.path.join(framework, "libraries", "Wire")
if not os.path.isfile(os.path.join(wire, "Wire.h")):
    raise Exception("the Adafruit core's Wire library is not at %s (docs/upstream.md)" % wire)

venv = env.Clone()
venv.Append(CPPPATH=[os.path.join(meshcore, "src"), wire])
lib = venv.BuildLibrary(
    os.path.join("$BUILD_DIR", "meshcore_variant"),
    variant_dir,
    src_filter="-<*> " + " ".join("+<%s>" % name for name in sources),
)
env.Prepend(LIBS=[lib])
