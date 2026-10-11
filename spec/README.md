# `spec/` — a copy

This directory is a copy of the system specification that lives in
`twinoak.meshcore.standard-repeater-design-v2/software/spec`. Start with
[`SYSTEM.md`](SYSTEM.md); the link between the two boxes is
[`LINK-PROTOCOL.md`](LINK-PROTOCOL.md); what a release says about itself is
[`FIRMWARE-MANIFEST.md`](FIRMWARE-MANIFEST.md). `fixtures/` are the bytes both
firmwares test against, `tools/` the reference code that made them, and
`proposals/` what is asked of MeshMaster and not yet decided.

**Nothing here is edited in this repository.** [`MANIFEST.txt`](MANIFEST.txt)
lists every file with its SHA-256 and size, and

```
python spec/tools/specsync.py verify spec
```

fails when one differs. `tools/verify.ps1` and CI run it. `.gitattributes`
keeps git from touching a line ending under `spec/`, because the comparison is
byte for byte.

When the specification is wrong or silent, say so in the task's report. It is
changed where it lives, and copied here again from there:

```
python tools/specsync.py copy ../../../<this repository's folder>
```

run in the hardware repository's `software/spec`. This file and `MANIFEST.txt`
are the only two that belong to this repository; the copy leaves this file
alone and rewrites the other.
