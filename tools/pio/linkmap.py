# The linker map, beside the images, on every build.
#
# A map file says which object every symbol came from - which is how a reviewer sees that
# MyMesh's methods come from the pinned checkout's examples/simple_repeater/MyMesh.cpp, that
# setup() and loop() come from our src/roles/repeater/main.cpp, and that upstream's main.cpp and target.cpp are
# not in the link at all (tasks/phase-1/01, acceptance). tools/verify.ps1 copies it to
# out/build/<env>/ with the rest of the evidence.
#
# A post-script, because LINKFLAGS are set by the platform's own build script, which runs after
# pre: and before post:. Appending here still reaches the link: SCons expands $LINKFLAGS when the
# link command runs, not when the program target is declared.

import os

Import("env")  # noqa: F821  (SCons injects it)

map_file = os.path.join(env.subst("$BUILD_DIR"), env.subst("${PROGNAME}") + ".map").replace("\\", "/")
env.Append(LINKFLAGS=["-Wl,-Map,%s" % map_file])
