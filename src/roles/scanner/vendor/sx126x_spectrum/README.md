<!--
VENDORED, NOT OURS.

  repository  https://github.com/sleeply4cat/MeshCore (a fork of meshcore-dev/MeshCore)
  branch      mesh-pocket-spectrum
  commit      f40f469d1e00eb8ae1b8ca07390dfc96a33ab707
  path        src/helpers/radiolib/sx126x_plugins/README.md
  licence     MIT, Copyright (c) 2025 Scott Powell / rippleradios.com - LICENSE.txt beside
              this file is the fork's own license.txt at the commit above

Everything below this comment is the file above, byte for byte. One thing it describes is *not*
how this repository works: here the assembled header **is** committed, beside the .asm, and no
build step generates it (tasks/phase-3.1/01-the-fast-sweep.md; docs/noisescope.md section 6).
-->

# SX126x PRAM plugins

Code that runs on the SX126x's own core (CoolRISC 816), loaded into its program RAM at run
time (`SX126x::uploadPatch`). Written in sxplug assembler; `build-sx126x-plugins.py` (a
PlatformIO pre-script of the Mesh Pocket envs) generates `X.h` from `X.asm` when the header is
missing or older than the source, and fails the build on assembler or lint errors. The headers
are build output and not committed.

- `spectrum.asm` -- a whole frequency sweep inside the radio (opcode 0x9B), used by
  `SX126xSpectrumScanner` for the spectrum screen. Verified on an SX1262, ROM "SX1261 V2D 2D02".
