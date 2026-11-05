# The release

A release of this firmware is a GitHub Release that three readers can use: a
person with a laptop, MeshMaster's firmware catalogue, and the Walter that
flashes the RAK over SWD. What it carries and how each reader checks it is
`spec/FIRMWARE-MANIFEST.md`; this page is how it is made here, and what to do
when it refuses. What to *do* with one — which file, which cable, what to check
afterwards — is [`docs/flashing.md`](flashing.md).

**Merging never releases.** A release is a person pushing a tag. CI on `main`
proves a commit is releasable; it publishes nothing.

**And it is built away from home.** Every job of `release.yml` runs on a
GitHub-hosted `ubuntu-latest`, while everything that only checks — `ci.yml`,
`upstream.yml` — runs on the organisation's own Unraid runners
(`[self-hosted, unraid]`; README.md, *Where CI runs*). This is what ships: the
images every repeater's update trusts, signed with `RELEASE_SIGNING_KEY`. A
self-hosted runner is shared with every branch a task session pushes and keeps
its home — `~/.platformio` and its toolchains — from one job to the next, and
signing somewhere else would keep the key off that runner but would still sign
whatever it built. A tag is rare, so the minutes are few.
`tools/ci/check-runners.ps1`, which runs as `ci.yml`'s first step, fails when a
job of this file moves home or when any other job stays away.

## Making one

**First, on a branch: the words.** `release-notes/v<X.Y.Z>.md` is the
hand-written top of the release body and it is written before the tag, in the
pull request that precedes it, so that it is reviewed like everything else here
(*The notes*, below). A tag with no such file is refused.

```powershell
git checkout main
git pull --ff-only
git tag v0.3.0            # the commit must be one of main's, ci green on it, and its notes written
git push origin v0.3.0
```

That starts `.github/workflows/release.yml`, which does, in order:

1. **`ci-green`** — refuses a tag whose commit `ci` has not passed, refuses a
   tag that is not on `main`, refuses a tag when there is no signing key,
   refuses a tag whose `release-notes/vX.Y.Z.md` is not in the repository, and
   works out the version: `X.Y.Z+<sha7>`, which is what the firmware itself
   reports.
2. **`build`** — one job per board environment of `platformio.ini`, built with
   `-D TWINOAK_FW_VERSION="X.Y.Z+<sha7>"` so the version is *inside* the image.
   `tools/pio/buildinfo.py` leaves `buildinfo.json` and a copy of the linker
   script beside the images, which is what the next job reads instead of being
   told anything.
3. **`publish`** — the body is `release-notes/vX.Y.Z.md` and then what
   `tools/release/release_notes.py` puts after it;
   `tools/release/build_release.py` writes the files,
   `tools/release/sign_manifest.py` signs the manifest with the repository
   secret `RELEASE_SIGNING_KEY`, `spec/tools/manifestref.py verify` proves that
   signature **before** anything is published, and then `gh release create`
   uploads the lot.

### A rehearsal

*Actions → release → Run workflow*, with `ref` left empty or set to a commit or
branch. It is the same three jobs and the same code; nothing is published. The
files are the run's artifact, the version is `0.0.0-dryrun+<sha7>` and the
signature is the specification's **public test key** — so an artifact of a
rehearsal can never be mistaken for a release, and
`tools/release/sign_manifest.py` would refuse to sign a real `X.Y.Z` with that
key.

The same rehearsal happens on this machine on every commit:
`tools/verify.ps1`'s `release` step runs the tooling's tests and then a dry run
over the build it has just made (`out/release/`, `out/verify/release.log`). The
release path is therefore exercised without GitHub and without the key.

## What is in it

Per image — one role on one board in one band — three files, named
`twinoak-<role>-<board>-<band>-vX.Y.Z.<ext>`:

| File | `use` | For |
|---|---|---|
| `.bin` | `swd` | **the Walter.** The raw application binary with its `load_address`: exactly the bytes that go in the application region and nothing else. |
| `.zip` | `usb-dfu` | a laptop on the adapter's USB-C port: `adafruit-nrfutil dfu serial -pkg <file> -p <PORT> -b 115200` |
| `.uf2` | `usb-uf2` | a laptop on the USB-C port: double-tap reset, then copy the file onto the drive that appears |

once per board family, listed under every image of that family:

| File | `use` | For |
|---|---|---|
| `twinoak-base-rak4630-<bootloader>-s140-6.1.1.hex` | `provision` | **the Walter, once per new radio.** The base under the application — MBR, SoftDevice S140 6.1.1, bootloader, the UICR words they need — written by a person's `rak provision` and never by an update (*The base*, below) |

and once for the whole release:

| File | |
|---|---|
| `manifest.json` | what each image is: role, board, band, MCU, the application region, and every file's size and SHA-256 (`spec/FIRMWARE-MANIFEST.md` §2) |
| `manifest.json.sig` | one ASCII line, `ed25519 <key id> <base64>`, over the manifest's bytes (§3) |
| `THIRD-PARTY-NOTICES` | the notices the base's licences ask for: Adafruit's bootloader (MIT), RAK's build of it (MIT), Nordic's SoftDevice (its own licence) |
| `SHA256SUMS` | every file above, the manifest included |

Because the manifest carries the hash of every file, **one signature covers the
whole release**, and a file is identified by its hash and never by its name.

### The application region

`app_region` in the manifest is read from the linker script the build actually
used — the `FLASH` region of its `MEMORY` block — and is never a constant in
the tooling. On the RAK4630 today that is `0x00026000`..`0x000ED000`: the
application starts at `0x26000` above the MBR and the SoftDevice, and ends
where the file system holding the node's identity, its settings, the ACL and
the clock file begins. An update writes that span and nothing else; no chip
erase, ever (`spec/SYSTEM.md` S-5, `docs/ARCHITECTURE.md` §9). A firmware
change that moves the end of the region moves `app_region` with it by itself,
and the Walter refuses a manifest that asks to write outside the region it
knows for that board.

`build_release.py` also refuses an image that is **not one contiguous span**
(the raw binary is written at one address, so a gap would leave whatever the
flash held before it) and one with **any byte outside the region**.

### The descriptor

Every image carries 96 bytes saying what it is — role, slot, the box-link
version it speaks, its own version, the board and the band, with a CRC-32 over
the first 92 (`spec/FIRMWARE-MANIFEST.md` §4) — so that a renamed or mixed-up
file is caught by the Walter before it is flashed. The firmware's build places
it as a `const` object and reads its own `HELLO` out of it
(`src/common/fwdesc/`, `docs/ARCHITECTURE.md` §8).

It is checked three times, and the first two are here:

1. **in the build.** `tools/pio/descriptor.py` runs after every link: it cuts
   the raw application binary out of the `.hex`, finds the descriptor with
   `spec/tools/manifestref.py`'s own reader, and fails the build unless there
   is **exactly one** and its role, board, band and version are the
   environment's. So an image that is wrong about itself never reaches a
   release. It leaves the raw binary as `firmware.bin` beside the other
   images, which is what `tools/verify.ps1` copies to
   `out/build/<env>/firmware.bin` — `python spec/tools/manifestref.py
   descriptor out/build/<env>/firmware.bin` reads it back.
2. **in the release.** `build_release.py --require-descriptor` is **on**, in
   `.github/workflows/release.yml` and in the local dry run of
   `tools/verify.ps1`: an image with no descriptor, with two, or with one that
   disagrees with its manifest entry stops the release with one line.
3. **in the Walter**, which reads the bytes out of what it downloaded before it
   flashes anything, and after the flash holds the `HELLO` it gets against the
   descriptor it wrote (`spec/FIRMWARE-MANIFEST.md` §5).

Each image entry also carries `requires.link_major`: the box-link major the
image speaks, so a Walter of another major is not offered it at all.

### The base

A RAK4630 arrives from the factory with RAK's RUI3, not the MBR, the
SoftDevice and the bootloader this firmware is linked against. The Walter
gives it that base **once**, when a person types `rak provision <radio>
confirm` on its console (`spec/SYSTEM.md` S-5a), from the release's
`provision` file; an update never touches it. So the release carries the base,
and because nobody here built it, what the release carries is a vendored file
at a pinned source with its hash written down:

| | |
|---|---|
| The file | `boards/base/rak4630/wiscore_rak4631_board_bootloader-0.4.4_s140_6.1.1.hex`, byte for byte as downloaded from RAKwireless' WisBlock repository; [`boards/base/rak4630/README.md`](../boards/base/rak4630/README.md) is the source, the commit, the hash, the versions, the licences, and what is known about APPROTECT on a revision 3 part |
| The pin | `boards/base/rak4630/base.json`: the SHA-256, the size, the bootloader's version, the SoftDevice and what `manifestref.py base` returns for it |
| The check | `tools/base/check.py`: the file's hash is the pin's; `manifestref.validate_provision` accepts it against the application region read from the linker script (the seven rules of `spec/FIRMWARE-MANIFEST.md` §4a: an MBR, S140 6.1.1 ending exactly where the application starts, a bootloader page above the region that `UICR.NRFFW[0]` points at, nothing in between, nothing in `UICR.APPROTECT`); the summary and the README say the same numbers. `tools/release/selfcheck.py` runs it on every commit, so a changed file fails on this machine first |
| In a release | `twinoak-base-rak4630-0.4.4-s140-6.1.1.hex`, `use: provision`, `format: hex`, no `load_address` (its records say where every byte goes), under **every** `rak4630-*` image — the repeater's and the scanner's — with one hash. `build_release.py` lists it, and refuses a `rak4630-*` image whose base is missing or fails the check; `sign_manifest.py` refuses to sign a manifest that lacks it or whose copy beside the manifest is not the pinned one. Nothing is signed that could not provision a radio |
| What the Walter does with it | §5's step 5p: it holds the file to §4a against the region *its* build knows for the board — a manifest cannot move the region — and stores it as the slot's base, never as a candidate. Writing it is the Walter's own procedure (`walter-firmware`, `docs/provisioning.md`): CTRL-AP erase, the base's flash and UICR words, `UICR.APPROTECT` from the chip's revision and never from the file, one reset at the end |

A new board family (`rak4630-dual-a` is the same module and gets the same
base; a different module gets its own `boards/base/<family>/`) is a directory
with the file, `base.json` and a README, and a family name in
`tools/base/check.py`.

## The notes

A release body is two parts, in this order:

1. **what a person wrote**, `release-notes/v<X.Y.Z>.md` in this repository: what
   this release is, which MeshCore it is, the link to
   [`docs/flashing.md`](flashing.md), and which Walter release it goes with. It
   is committed on the branch before the tag and reviewed in that pull request,
   and `tools/release/release_notes.py` puts it at the top of the body with a
   `---` under it;
2. **what the tooling generates** from the manifest — the version the firmware
   reports, the table of files and what each one is for, the application region,
   and where to read about flashing one.

**A tag whose file is missing is refused in `ci-green`**, before anything is
built, the way a tag with no signing key is: nothing else would catch it, and a
release whose notes are only the generated table is a release nobody can read.
An empty file is refused too. A *rehearsal* is not refused — its version is
`0.0.0-dryrun+<sha7>`, no file is written for it, and the body says so in one
line where the top would be, which is how that path is exercised without a tag.
`tools/verify.ps1` reads every `release-notes/*.md` of the commit with the same
code (`tools/release/selfcheck.py`), so a file named `v0.1.md`, or one that says
nothing, fails on a commit.

## The key

One Ed25519 key signs releases. Its private half is the repository secret
`RELEASE_SIGNING_KEY` and lives nowhere else; its public half is
`keys/release.pub` — one line, `ed25519 <key id> <64 hex>`, which is the form
the Walter firmware's trust table uses for the same key, and whose key id is
recomputed from the key when it is read. [`keys/README.md`](../keys/README.md)
is how it is made, by a person, once. The key that signs releases today is
`73efdd383a957596`, made on 20 October 2026.

Nothing in the pipeline prints a key, and no error message quotes one: when
something is wrong with the key the message says what is wrong and not what it
was given.

## When a tag is refused

The run stops in `ci-green` or at the signing step, and nothing is published —
a refused tag has published nothing, so the fix is a new commit and a new tag
(`v0.3.1`), not a forced one.

| What the log says | What happened | What to do |
|---|---|---|
| `RELEASE_SIGNING_KEY is not set` | the repository has no signing key | make it: `keys/README.md`. Until it exists, no tag can be released, which is the safe state |
| `… is not a commit of main` | the tag is on a side branch | tag a commit of `main` |
| `ci has completed for … without a green run` | `ci` was red on that commit | fix it on `main`, then tag the new commit |
| `ci has not gone green for … within 20 minutes` | `ci` never ran, or is still running | wait for `ci` on `main` to finish and tag again; a tag whose commit CI has not passed is not released |
| `release-notes/vX.Y.Z.md is not in the repository` | the tag has no hand-written notes | write that file on a branch, merge it, and tag the commit it merged as. *The notes*, above, says what goes in it |
| `… does not hold a public key on its first line of content` / `… names key id …, and the key on that line is …` | `keys/release.pub` is not one line of `ed25519 <key id> <64 hex>` or of 64 hexadecimal characters, or its two halves disagree | `keys/README.md` has the one form. `tools/verify.ps1` reads the real file on every commit, so this should never be met at a tag |
| `… is not the key keys/release.pub publishes` | the secret and the published public half are not two halves of one key | one of them is wrong. Replace the secret, or publish the right public half in a commit — and work out which, because a Walter trusts only what `keys/release.pub` says |
| `this manifest says version X.Y.Z, which is a release` | something tried to sign a release with the test key | that is the guard working; a release is signed with `RELEASE_SIGNING_KEY` |
| `is not one contiguous span` / `outside the application region` | the linked image is not a single span inside the application region | a change to the linker script or to a `KEEP` section put bytes somewhere an update may not write. Read the addresses in the message against the script line it quotes |
| `was built as version … and this release says …` | the images were not built for this release | re-run the workflow; the version is a build flag and must be in the image |
| `… is not the pinned base: its SHA-256 is …` | the vendored base under `boards/base/` is not the file its `base.json` names | a changed byte is a different base. Put the pinned file back, or — a new bootloader, decided by a person — rewrite the pin, the README and `THIRD-PARTY-NOTICES` together |
| `… is refused against the application region … (section 4a)` | the base fails one of §4a's rules against the region the linker script gives (the message quotes the rule) | the file is not a base for this board, or the application region moved: read the rule against `boards/base/rak4630/README.md` |
| `no base is pinned under boards/base/…` / `has 0 provision file(s)` | a `rak4630-*` image without its base, in the build or in a manifest about to be signed | a release of this board carries the base or it is not signed (S-5a); see *The base* |

The same messages come out of `tools/verify.ps1` on this machine, with no tag
and no key involved, which is where they are meant to be met first.

## Versions

`X.Y.Z+<sha7>` is what the firmware reports and what the manifest records; the
file names carry `vX.Y.Z` without the build metadata. A tag is `vX.Y.Z` and
nothing else — the workflow's trigger is `v[0-9]+.[0-9]+.[0-9]+`, so a
pre-release tag does not start it.

A version with a pre-release part (`0.0.0-dev` from a local build,
`0.0.0-dryrun+<sha7>` from a rehearsal) is not a release version: that is the
line `sign_manifest.py` draws when it decides whether the test key may sign.
