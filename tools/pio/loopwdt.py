# The loop watchdog's marker gets a place in RAM, on every build of the scanner.
#
# src/roles/scanner/loopwdt.ld is one `INSERT AFTER .bss` block - the same mechanism the Adafruit
# core's own ldscript.ld uses for .svc_data and .fs_data - and it has to reach the linker as a
# *second* -T, after the core's script: an INSERT needs the section it names to have been read
# already. That rules out platformio.ini's build_flags, where PlatformIO reads `-Wl,-T` as a
# request to replace board_build.ldscript altogether (it says so, and then cannot find the file).
#
# So it is appended here, as tools/pio/linkmap.py appends its -Wl,-Map: a post-script runs after
# the platform's own build script, and SCons expands $LINKFLAGS when the link command runs rather
# than when the program target is declared.
#
# What the script asserts at link time is in the script itself (after .bss, before .heap). If this
# file ever stops running, .noinit becomes an orphan section placed by the linker's own rules and
# those assertions go with it - which is why docs/bench.md section 9's watchdog line is walked on
# a board and not taken on trust.

import os

Import("env")  # noqa: F821  (SCons injects it)

fragment = os.path.join(env.subst("$PROJECT_DIR"), "src", "roles", "scanner",
                        "loopwdt.ld").replace("\\", "/")
if not os.path.isfile(fragment):
    raise SystemExit("tools/pio/loopwdt.py: %s is missing, and the scanner's .noinit marker has "
                     "nowhere to live" % fragment)
env.Append(LINKFLAGS=["-Wl,-T,%s" % fragment])
