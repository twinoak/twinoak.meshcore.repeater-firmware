# The release

A release of this firmware is a GitHub Release that three readers can use: a
person with a laptop, MeshMaster's firmware catalogue, and the Walter that
flashes the RAK over SWD. What it carries and how each reader checks it is
`spec/FIRMWARE-MANIFEST.md`; this page is how it is made here, and what to do
when it refuses. What to *do* with one — which file, which cable, what to check
afterwards — is [`docs/flashing.md`](flashing.md).

**Merging never releases.** A release is a person pressing *Run workflow*. CI on
`main` proves a commit is releasable; it publishes nothing. **And a tag pushed
by hand starts nothing** — there is no tag trigger here any more; the tag of a
release is made by the run that publishes it, on the commit it built.

**And it is built on GitHub's own machine.** Every job of `release.yml` runs on
a GitHub-hosted `ubuntu-latest` — as does every job of every other workflow
here, because this repository is public (README.md, *Where CI runs*). For this
file that is more than the rule: this is what ships, the images every repeater's
update trusts, signed with `RELEASE_SIGNING_KEY`, and a runner that begins each
job new and is gone after it is where that should happen. Signing somewhere else
would keep the key off a shared runner but would still sign whatever that runner
built. A release is rare, so the minutes are few in any case.
`tools/ci/check-runners.ps1`, which `ci.yml` runs as a step, fails when a job of
any workflow here names a self-hosted runner.

## Making one

**Press the button.** *Actions → release → Run workflow*, branch `main`, **dry
run** left unticked. Nothing is typed.

That is the whole of it, because the two things a release used to be told are
already in the repository:

| | Where | Who puts it there |
|---|---|---|
| the version | [`VERSION`](../VERSION) at the root, one line of `X.Y.Z`. It names the release that **comes next**, and after a release the workflow moves it to the next patch | the workflow, in a pull request of its own. A bigger step than a patch is that one line edited beforehand, in the pull request that earns it |
| the words | `release-notes/unreleased/<task>.md`, one line per task, plus the standing paragraph `release-notes/standing.md` | each task, in its own pull request (*The notes*, below) |

`.github/workflows/release.yml` then does, in order:

1. **`ci-green`**, before anything is built: the refusals (*When a release is
   refused*, below), the signing key, and `ci` green for the commit. The
   version is `X.Y.Z+<sha7>` of `VERSION`, which is what the firmware itself
   reports.
2. **`build`** — one job per board environment of `platformio.ini`, built with
   `-D TWINOAK_FW_VERSION="X.Y.Z+<sha7>"` so the version is *inside* the image.
   `tools/pio/buildinfo.py` leaves `buildinfo.json` and a copy of the linker
   script beside the images, which is what the next job reads instead of being
   told anything.
3. **`publish`** — the body is the waiting lines and the standing paragraph
   (`tools/release/release_notes.py`) and then what the manifest generates;
   `tools/release/build_release.py` writes the files,
   `tools/release/sign_manifest.py` signs the manifest with the repository
   secret `RELEASE_SIGNING_KEY`, `spec/tools/manifestref.py verify` proves that
   signature **before** anything is published, and then `gh release create`
   makes the tag on the commit that was built, publishes it as **Latest** and
   uploads the lot.
4. **the move**, in the same job and only once the release is out: the lines
   become `release-notes/vX.Y.Z.md` as published, `release-notes/unreleased/`
   is emptied, `VERSION` goes to the next patch, and that one commit is filed
   as a pull request the job opens, tries to merge, and leaves open for a
   person when `main`'s rules do not let it.

**The release stands whatever happens to that pull request, and the run is green
when the pull request is only waiting.** `main` requires a pull request before
merging and asks for the `verify` check, so the job cannot merge the one it
opened: **every release leaves it for a person**, and that is the ordinary end of
a release rather than a failure of one. The step therefore ends green and the
first line of the run's summary says which pull request is left
(`tools/release/notes_pr.py`, where that is decided and tested). The run is
**red** only for what did fail — the branch could not be pushed, or no pull
request could be opened for it — and then nothing is waiting for a person,
because there is nothing to merge. Either way nothing is undone, and the next
release is refused until the version has moved (*When a release is refused*).

**What to do with that pull request.** It is `VERSION` and `release-notes/` and
nothing else, and either way of finishing it is right:

- **approve its `ci` run and merge it when `verify` is green.** GitHub does
  start a run for that pull request and it is **held for a maintainer's
  approval** — *Actions* → the run of `release-notes/vX.Y.Z` → *Approve and
  run*. Unapproved it never runs: after about an hour it expires and is listed
  as a failed run, which is what the second red run of 13 November 2026 was;
- **or merge it without waiting.** `ci` runs on `main` when that commit lands,
  and the release after this one stands on that run — or, where there is none,
  on the run of the commit below it (*The commit a release leaves behind*).

Whether `main`'s rules change so that the job can finish by itself is Thomas's
to decide (`tasks/phase-3.12/FOLLOWUPS.md`); `tools/release/notes_pr.py` says
the same thing in the pull request's own body, so whichever of the two is read
first is enough.

### A rehearsal

The same button with **dry run** ticked, from any branch, with `ref` optionally
naming a commit or branch to rehearse on (only a rehearsal may). It is the same
three jobs and the same code; nothing is published and no tag is made. The files
are the run's artifact, the version is `0.0.0-dryrun+<sha7>` and the signature
is the specification's **public test key** — so an artifact of a rehearsal can
never be mistaken for a release, and `tools/release/sign_manifest.py` would
refuse to sign a real `X.Y.Z` with that key. The body is compiled from the same
waiting lines, so what a rehearsal prints is what the release would publish.

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

**A line is written when the change is made, by the task that makes it.** That
is the only moment anybody knows, in one plain sentence, what a person with a
radio will notice. Nothing is summarised afterwards; a release is those lines.

One file, in the same pull request as the change:

```
release-notes/unreleased/<task>.md
```

`<task>` is the task's number (`p3.11-01.md`) or, for a change made by hand, any
short name. The file holds **one line**: at most 240 characters, plain words and
nothing else — no heading, no list marker, no table — about **what a person will
notice** rather than what was done, and naming no task, decision or spec
section. A change nobody can see — a test, a refactoring, a workflow, a document
— writes **no** file. If you are not sure, ask what somebody who looks after a
repeater would do differently; if the answer is *nothing*, there is no line.

A release body is then two parts, in this order:

1. **the top this repository compiles**: the waiting lines, in the order the
   tasks merged — the commit that *added* each file, along `main`'s first
   parent, so a line is placed by its pull request and not by its file name —
   and under them [`release-notes/standing.md`](../release-notes/standing.md):
   what every hand-written top used to repeat and no manifest can generate,
   **The Walter it goes with** first among it. The task that changes one of
   those things edits that file in the same pull request;
2. **what the tooling generates** from the manifest — the version the firmware
   reports, which MeshCore it is, the table of files and what each one is for,
   the application region, and where to read about flashing one — with a `---`
   between the two.

Once the release is out, the lines become `release-notes/vX.Y.Z.md`: a
release's body as it was published, which is what the four tops written by hand
for `v0.1.0` to `v0.1.3` are. Nothing there is ever edited afterwards.

**A release with no waiting line is refused in `ci-green`**, before anything is
built, the way one with no signing key is: a release with nothing to say is a
release nobody reads. A *rehearsal* is not refused — with nothing waiting its
body says so in one line where the lines would be, which is how that path is
exercised without publishing anything. `tools/verify.ps1` reads `VERSION`, every
waiting line, the standing paragraph and every published
`release-notes/vX.Y.Z.md` with the same code
(`tools/release/selfcheck.py`), so a line of three sentences, or a file named
`v0.1.md`, fails on a commit.

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

## When a release is refused

The run stops in `ci-green` or at the signing step, and nothing is published —
a refused release has published nothing and made no tag, so the fix is a commit
on `main` and the button again.

| What the log says | What happened | What to do |
|---|---|---|
| `there is nothing to release` | no line is waiting in `release-notes/unreleased/` | there is nothing a person would notice since the last release. *The notes*, above |
| `v0.1.4 is already a tag` | the version was not moved on after the last release: the pull request that moves it never landed | merge it if it is still open. If it was never opened: `python tools/release/release_notes.py move` on a branch off `main`, merge that, and press the button again |
| `a release is published from main` | the run was started on another branch | choose `main` in the branch box, or tick *dry run* to rehearse where you are |
| `only a *dry run* may rehearse somewhere else` | a release was asked for with a `ref` | leave `ref` empty to publish |
| `RELEASE_SIGNING_KEY is not set` | the repository has no signing key | make it: `keys/README.md`. Until it exists, nothing can be released, which is the safe state |
| `… is not a commit of main` | `main` moved between the press and the checkout | press the button again |
| `ci has completed for … without a green run` | `ci` was red on that commit | fix it on `main`, then press the button again |
| `ci has not gone green for … within 20 minutes` | `ci` never ran, or is still running | wait for `ci` on `main` to finish and press again; a commit CI has not passed is not released |
| `… does not hold a public key on its first line of content` / `… names key id …, and the key on that line is …` | `keys/release.pub` is not one line of `ed25519 <key id> <64 hex>` or of 64 hexadecimal characters, or its two halves disagree | `keys/README.md` has the one form. `tools/verify.ps1` reads the real file on every commit, so this should never be met in a release |
| `… is not the key keys/release.pub publishes` | the secret and the published public half are not two halves of one key | one of them is wrong. Replace the secret, or publish the right public half in a commit — and work out which, because a Walter trusts only what `keys/release.pub` says |
| `this manifest says version X.Y.Z, which is a release` | something tried to sign a release with the test key | that is the guard working; a release is signed with `RELEASE_SIGNING_KEY` |
| `is not one contiguous span` / `outside the application region` | the linked image is not a single span inside the application region | a change to the linker script or to a `KEEP` section put bytes somewhere an update may not write. Read the addresses in the message against the script line it quotes |
| `was built as version … and this release says …` | the images were not built for this release | re-run the workflow; the version is a build flag and must be in the image |
| `… is not the pinned base: its SHA-256 is …` | the vendored base under `boards/base/` is not the file its `base.json` names | a changed byte is a different base. Put the pinned file back, or — a new bootloader, decided by a person — rewrite the pin, the README and `THIRD-PARTY-NOTICES` together |
| `… is refused against the application region … (section 4a)` | the base fails one of §4a's rules against the region the linker script gives (the message quotes the rule) | the file is not a base for this board, or the application region moved: read the rule against `boards/base/rak4630/README.md` |
| `no base is pinned under boards/base/…` / `has 0 provision file(s)` | a `rak4630-*` image without its base, in the build or in a manifest about to be signed | a release of this board carries the base or it is not signed (S-5a); see *The base* |

The same messages come out of `tools/verify.ps1` on this machine, with nothing
published and no key involved, which is where they are meant to be met first.

## Versions

**`VERSION` at the root is the one place** — one line of `X.Y.Z`, naming the
release that comes next, read by `tools/release/version.py` and by nothing else.
`X.Y.Z+<sha7>` is what the firmware reports and what the manifest records; the
file names carry `vX.Y.Z` without the build metadata, and a tag is `vX.Y.Z` and
nothing else.

A release moves the file on by a patch, in its own pull request. **A minor or a
major is that line edited by hand beforehand**, on the branch whose change earns
it, in the same pull request as the line that explains it — then the button
publishes what the file says.

### The commit a release leaves behind

The pull request the workflow files touches `VERSION` and `release-notes/` and
nothing else, and **it may have no `ci` run of its own**: nothing starts a run
for a push raised with the job's own token, and the run GitHub does start for
the *pull request* is the branch's commit's — held for a maintainer's approval —
and never that of the commit a merge puts on `main`. Merged by a person, that
commit arrives as an ordinary push and does have a run; the walk is what covers
the times it does not. The next release's *ci is green* gate therefore walks
back from `main`'s head over exactly those commits — a commit that touched one
other file stops the walk at once — to the newest commit that does have a run,
and that run is the evidence (`tools/release/gate.py`). The images are still
built from `main`'s head; nothing those commits carry can reach one.

A version with a pre-release part (`0.0.0-dev` from a local build,
`0.0.0-dryrun+<sha7>` from a rehearsal) is not a release version: that is the
line `sign_manifest.py` draws when it decides whether the test key may sign.
