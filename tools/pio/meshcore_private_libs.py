# MeshCore's private libraries, for a consumer of the package.
#
# MeshCore keeps its ed25519 implementation in its own lib/ed25519 - a PlatformIO *private*
# library of its project, which is exactly what a project that depends on MeshCore as a package
# does NOT get: lib_extra_dirs pointed at it does not reach the dependency's own sources either,
# because a library with a manifest resolves its dependencies from that manifest and ed25519 is
# not in it (src/Identity.cpp then fails with "ed_25519.h: No such file"). So the sources are
# added to the build here, from the pinned checkout and never copied into this repository, and
# the include path is in platformio.ini beside the variant's.
#
# This is the agent firmware's tools/pio/meshcore_private_libs.py, unchanged in what it does.
# A pin bump that moves or renames lib/ed25519 fails here, loudly, which is the point -
# docs/upstream.md lists this file among the places a bump re-reads.

import os

Import("env")  # noqa: F821  (SCons injects it)

meshcore = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"), "MeshCore")
ed25519 = os.path.join(meshcore, "lib", "ed25519")

if not os.path.isdir(ed25519):
    raise Exception(
        "MeshCore's lib/ed25519 is not at %s - has the pin moved it? (docs/upstream.md)" % ed25519
    )

env.BuildSources(os.path.join("$BUILD_DIR", "meshcore_ed25519"), ed25519)
