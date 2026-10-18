# 02 — A bench a session may use

**Depends on:** 01.
**Spec:** `CLAUDE.md` (*never write outside the application region, never chip-erase*, never print a password); [`tasks/RUNBOOK.md`](../RUNBOOK.md) §2 (*no session can flash a board*, which this task changes, narrowly) and §5; [`docs/bench.md`](../../docs/bench.md) §0 (the SWD line: not a session's), §1, §2, §8–§10; `spec/FIRMWARE-MANIFEST.md` §4 and `spec/tools/manifestref.py` (`descriptor`); `tools/release/build_release.py` (the three files of an image, the `app_region` it reads from the linker script), `tools/pio/meshcore_uf2.py` (how `firmware.zip` is made); `tools/linkcli.py` (the link over a serial port); the phase-3 review ([`../phase-3/FOLLOWUPS.md`](../phase-3/FOLLOWUPS.md), *Nothing on this phase's bench list has been walked*).
**Size:** one session, long.
**Model:** claude-fable-5-1

## Goal

Thomas has a RAK4630 on the USB port of the machine the driver runs on.
Most of `docs/bench.md` is a person typing at a console and copying what it
prints; a script can do that, and a session can then run the script. Build
`tools/bench/`: the one way a session touches a board, with the rails written
into the tool rather than into the session's good intentions. This task
builds the tool and its guards; 03 writes the walks.

## Deliverables

- **The rig is described, not discovered.** A git-ignored (the line is in
  `.gitignore` already) `bench.local.json` (an example `bench.example.json` is committed) names the
  RAK by its **USB serial number** (the chip's id, which the bootloader's port
  and the application's port share — confirm that on the board if it is
  there, and say what the tool does if they differ), and says what is on its
  RF port (`dummy-load`, `antenna` or `nothing`), whether its identity may be
  thrown away (`keep` or `disposable`), and, optionally, the serial number of
  a 3.3 V USB-UART dongle on the link pins. No config, or no port with that
  serial number present: every command exits with one documented code that
  means *no bench* and prints why; nothing else is an error. The tool never
  picks "the only COM port" by itself.
- **Flashing, and only one kind.** `bench flash <image>` takes one of *our*
  images and nothing else: the DFU package (`firmware.zip`, the release's
  `.zip`) through `adafruit-nrfutil dfu serial` with the 1200-baud touch,
  pinned (PlatformIO's bundled copy or a pip pin, say which). Before the
  bootloader is touched it refuses — with the reason — a package that holds a
  bootloader or a SoftDevice, an application whose `.bin` does not carry
  exactly one valid descriptor (`fwdesc`'s rules, through `manifestref.py`),
  a descriptor whose board is not the bench's, an application that does not
  fit the `app_region` the release tooling reads from the linker script, and
  anything that is not a file the build or the release made (the descriptor
  and the region are the proof; the path is not). **No UF2, no SWD, no
  erase, no `reboot 2`/`3`** by any command of the tool. A repeater image (a
  role that transmits) is refused when the RF port is not `dummy-load`; a
  scanner image is allowed on any. Every flash is logged with the image's
  descriptor and SHA-256.
- **The board is left as it was found.** Before the first flash of a run the
  tool records what is on the board (`hello` over the link if a dongle is
  there, else the console's `Repeater ID:` and `sensor get fw.version`, or
  the scanner's `info`), and `bench restore` puts that role back — the
  recorded version if the run was given its image, otherwise the run's own
  image of that role, and the last line says which — and checks the
  `Repeater ID` is the one it recorded. A run that cannot restore says so in its last line, in capitals.
- **One user at a time.** A lock per board (a file under the user's local
  application data, with the process id and the start time; a stale one is
  named and taken over only when its process is gone), held for the whole of
  a command, so a session and a person at a terminal cannot flash the same
  board at once.
- **A console a script can drive.** `bench console` sends one line (CR, as
  the console wants), reads until the reply's `  -> ` and the quiet after it,
  with a timeout, and returns the reply text. It **refuses to send** any line
  that sets a password (`password …`, `set guest.password …`) unless the run
  is in the disposable-identity mode 03 uses for §10, and it **never prints**
  a reply line that begins `password now:`. The scanner's NDJSON lines are
  read as JSON, a line that does not parse kept as text and counted.
- **Host tests for every guard** (`tools/bench/test_bench.py`, run by
  `tools/verify.ps1`'s release step beside the release tooling's tests, which
  is Python already): a package with a bootloader, one with no descriptor, one
  with two, one for another board, one too large for the region, a repeater
  image with the RF port on an antenna — each refused before any port is
  opened (a fake serial layer proves nothing was opened); the lock taken,
  refused and recovered; the config's *no bench* paths; the console's password
  refusals and its redaction. CI has no board and needs none.
- **The rule, written down.** `tasks/RUNBOOK.md` §2 says: a session touches a
  board only through `tools/bench/`, only when its task says so, never with an
  RF port on an antenna for a repeater image, and it leaves the board as it
  found it; *no bench* is not a failure (the walk goes to *Not verified here*,
  as today). `docs/bench.md` gets a short §0a: what `bench.local.json` says,
  and that a walked line is ticked with the date, the commit and *bench* so a
  person can tell it from one they walked themselves.

## Acceptance criteria

- [ ] Every refusal above is a host test, and none of them opens a port.
- [ ] With no `bench.local.json`, every command exits *no bench* and `tools/verify.ps1` is green.
- [ ] If the board is on this machine: `bench flash` of the repeater image built by `tools/verify.ps1` (with the RF port on the dummy load), `bench console "ver"`, `bench restore` — the transcript, without a password, in the report. If it is not: say so; the tool is still complete.
- [ ] `tools/verify.ps1` green.

## Not verified here

Recovery of a board whose application never enumerates USB: the tool says
*double-tap reset* and stops — that is a person's hand, and the bootloader is
untouched, so it is all it takes.

## Out of scope

The walks themselves (03). The Walter on USB (its own repository; this tool's
shape is what it can copy). SWD of any kind, ever.
