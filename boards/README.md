# `boards/` — the one file of upstream's that is vendored

PlatformIO reads `boards_dir` (this directory, by default) **before** it clones
a dependency, so a board definition that lives inside MeshCore's package is not
one a consumer of that package can point `board =` at. There is exactly one such
board here, copied from the pin and unedited — byte for byte (`sha256` of the
file equals `git show <pin>:boards/rak4631.json`'s), and `.gitattributes` keeps
git from changing a line ending in it:

| File | Copied from | At |
|---|---|---|
| `rak4631.json` | `boards/rak4631.json` in [MeshCore](https://github.com/meshcore-dev/MeshCore) | the commit `[meshcore].pin` names in `platformio.ini` (`repeater-v1.17.1`, `d9296435`) |

Nothing here is edited: a difference between this file and the pinned package's
is a bug, not a feature, and a pin bump re-copies it (`docs/upstream.md`, which
lists this directory among the places a bump re-reads). The RAK4630 stamp on
our adapter is the RAK4631 core module with the same pin map, which is why
NoiseScope in the hardware repository builds with the same definition.

What the definition names, and where it is found:

- `"variant": "WisCore_RAK4631_Board"` — the Arduino variant directory of the
  Adafruit nRF52 core (`platform_packages` in `platformio.ini` pins MeshCore's
  fork of it). MeshCore's own `variants/rak4631/variant.{h,cpp}` — the same pin
  map plus the LoRa pin defines its board object needs — is reached with
  `-I ${meshcore.dir}/variants/rak4631` in the environment's `build_flags` and
  compiled into the MeshCore library by `MC_VARIANT=rak4631` (upstream's
  `build_as_lib.py`). Neither is copied.
- `"ldscript": "nrf52840_s140_v6.ld"` — the core's linker script for SoftDevice
  S140 6.1.1: the application starts at `0x26000`
  (`docs/ARCHITECTURE.md` §9). The release tooling reads the application
  region from the linker script the build used, never from a constant.
- `"protocol": "nrfutil"` — which is what makes the nordicnrf52 platform
  produce the DFU package `firmware.zip` beside `firmware.hex` on every build.

So the one thing vendored is the one thing that cannot be referenced.
