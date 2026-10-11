// Upstream's MyMesh.h (examples/simple_repeater, compiled from the pin), as a system header - see
// src/common/board/UpstreamHeaders.h for why. MyMesh.h includes <target.h>, which is ours
// (src/common/board),
// and that in turn includes UpstreamHeaders.h, so one include of this file brings the whole
// upstream side in with its warnings kept out of our -Werror.

#pragma once

#pragma GCC system_header

#include <MyMesh.h>
