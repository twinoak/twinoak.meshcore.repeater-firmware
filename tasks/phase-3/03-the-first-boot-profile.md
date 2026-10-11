# 03 — The first-boot profile

**Depends on:** 01.
**Spec:** [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-12, S-13; [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §2.
**Size:** one session.
**Model:** claude-opus-5

## Goal

A unit fresh from the flasher, with nobody near it, is already a well-behaved
member of the Danish mesh: right channel, sane defaults, no password anyone
knows. And there is one written procedure for making it *this* unit — its name,
its place, its passwords — whether a person does it at the USB port or the
Walter does it over the link.

## Read first, in the pinned checkout

`MyMesh`'s constructor (the defaults upstream chooses), `CommonCLI.cpp`'s `set`
and `get` branches (every key, its range, and which ones only the console may
set), and how preferences are saved and loaded.

## Deliverables

- **The profile** (`src/roles/repeater/firstboot/`): a table of console
  command lines that the firmware runs **once**, through
  `handleCommand(0, …)`, on the boot that finds no saved preferences — after
  the random admin password of phase 1's task 01, and before the radio is
  used. A table of text, on purpose: it uses only upstream's public command
  interface, a pin bump that renames a key shows up as a reply that is not
  `OK`, and Thomas can read and change it without reading C++.
  - The table as shipped is **short and each line has its reason beside it**:
    `set path.hash.mode 1` (adverts with two-byte path hashes — S-12 and
    meshmaster.dk's D-33) is the one line this task is sure of. Add a line
    only where a document in `spec/` or the hardware repository gives the
    value; list in the report the keys you considered and left at upstream's
    default, with that default, so the reviewer can fill the table from the
    network's own standard.
  - The radio's channel is **not** in the table: it is the build's default
    (band block, task 01), because a unit must be on the right frequency even
    if the profile never ran.
  - Each line's reply is checked; a line that is refused is recorded, the rest
    still run, and the outcome is readable afterwards: `sensor get
    firstboot.state` says `done`, `partial: <n> refused`, or `never` (settings
    existed already).
  - The mechanism is portable — a list of lines in, a command interface it is
    given, a result out — with native tests; the table is data.
- **Running once is exact**: decided by the same test phase 1 used for the
  random password, in the same place; a power cut between the two leaves the
  node either unprovisioned-and-retried or done, never half. Say how.
- **`docs/provisioning.md`**:
  - *Out of the box*: the channel, the profile's lines, a random admin
    password, upstream's defaults for the rest — and what that means in
    practice (it repeats; nobody can administer it over the mesh yet).
  - *Making it a unit*: the settings a unit needs and the command for each —
    name, position, owner info, the admin password, the guest password (and
    what leaving it empty means: upstream answers a blank login at guest),
    regions if the network uses them.
  - *At the USB port*: the session, line by line.
  - *Over the link*: the same lines as `CLI` requests — which is what the
    Walter will do — as a `linkcli.py provision <file>` that reads a small
    text file of lines, sends them one by one, stops at the first refusal and
    prints passwords as stars. The file format is the profile's own.
  - *Afterwards*: what MeshMaster needs to be told (the key from `IDENTITY`,
    the admin password it will hold), and that the password set here is the
    only copy until it is.
- **`README.md`**: a "Provisioning it" section pointing there.

## Tests (native)

- The runner against a fake command interface: all accepted; one refused in
  the middle; an empty table; a reply that is not `OK` but is not an error
  either (decide what counts and say so, from upstream's actual replies).
- Runs once: with settings present it does nothing and says `never`.
- `linkcli.py provision` against the session's loopback: stops at a refusal,
  never echoes a `password` line's value.

## Acceptance criteria

- [ ] `tools/verify.ps1` green.
- [ ] The report lists every `set` key of the pinned CLI with upstream's default, marking the ones the profile sets and the ones only the console may set.
- [ ] `docs/provisioning.md` exists and its USB session is what the console accepts, line for line (checked against the source, since no board is here).

## Not verified here (Thomas, with a board)

Add to `docs/bench.md`, "9. First boot and provisioning":

- Erase the node's settings (`erase` on the console) and reset: `sensor get
  firstboot.state` says `done`; `get path.hash.mode` says 1; the radio is on
  869.618.
- Provision it from `docs/provisioning.md`, once at the USB port and once with
  `linkcli.py provision`; a phone then logs in with the admin password that
  was set.

## Out of scope

MeshMaster's side of adopting a unit. Changing any default the documents do
not give a value for.
