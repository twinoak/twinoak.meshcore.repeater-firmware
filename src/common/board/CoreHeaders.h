// The Arduino core's own headers - and nothing above them - as system headers.
//
// src/ is built with -Wall -Wextra -Werror (platformio.ini, build_src_flags) and the headers
// this firmware is built on top of are not written to that standard; `#pragma GCC system_header`
// makes the rest of this file a system header, and GCC treats what a system header includes as
// system headers too, so -Werror stays about our own code. That is the whole of what
// src/common/board/UpstreamHeaders.h does for the repeater's side of the tree, and this file is
// the first two lines of it.
//
// **Why two wrappers and not one.** An environment compiles src/common/ beside exactly one role
// (docs/ARCHITECTURE.md section 8), and the scanner compiles no MeshCore at all
// (src/roles/scanner/): a file of common/ that needs only the core's `Wire` must not drag
// MeshCore, RadioLib and upstream's board class into a build that has none of them. So
// UpstreamHeaders.h includes this one for the core, and a file that needs the core alone - today
// src/common/power/WireBus.cpp - includes this one and stops here.
//
// A file of ours includes this, or UpstreamHeaders.h, or a role's own wrapper
// (src/roles/repeater/app/UpstreamMyMesh.h, src/roles/scanner/ScannerHeaders.h), and never an
// upstream header directly: a header first met outside a system-header context is compiled with
// our warnings on.

#pragma once

#pragma GCC system_header

#include <Arduino.h>
#include <Wire.h>
