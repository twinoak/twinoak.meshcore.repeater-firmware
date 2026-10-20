# Proposal 04 — the Walter provisions a new radio

**For:** `SYSTEM.md` (S-5, §5, §10.2), `FIRMWARE-MANIFEST.md`, the Walter firmware, the repeater
firmware's release, meshmaster.dk's catalogue. **Status:** accepted 2026-10-20 (Thomas) **as a
command, not as an automatic step** — now `SYSTEM.md` S-5a and `FIRMWARE-MANIFEST.md` §4a. Built as
the Walter's phase 5 (and walked in its phase 6), the repeater firmware's pile 3.4 (task 01) and
meshmaster.dk's pile 4.16 (task 133).

## What was found

RAK4630 stamps ship with RAK's RUI3, not the MBR, SoftDevice S140 6.1.1 and bootloader the repeater
is linked against (2026-10-20, the first article). Every new radio needs that base once, before the
Walter's application-region flasher can do anything. Done with a debug probe it is a per-unit bench
step, and a radio swapped on a mast would need a probe on the mast (README goal 4: swap just the
radio).

## What was proposed, and what was decided

The proposal had the Walter classify a radio at every boot (*provisioned / damaged / unprovisioned /
locked*) from a lookup and a journal, and provision an unprovisioned one by itself. Thomas's answer
was simpler: build it into the Walter's firmware **as a command**. That drops the classification
and the journal entirely:

- **No classifying.** A person decides that this radio gets a base, and says which radio by its
  `DEVICEID` (or `locked`, when the port is closed and nothing can be read).
- **No journal.** Every step either completes or leaves the radio erased, and the command starts by
  erasing; after a failure or a power cut it is run again.
- **S-5 unchanged.** Nothing automatic erases a chip. S-5a adds the one person-made exception.

## Where each part lives

| Part | Where |
|---|---|
| the rule | `SYSTEM.md` S-5a |
| the file and what it may hold | `FIRMWARE-MANIFEST.md` `use: provision`, §4a; `tools/manifestref.py validate_provision`; `fixtures/manifest/base-*.hex` |
| the base in a release (the RAK4630 bootloader + S140 6.1.1, vendored and pinned) | repeater firmware, pile 3.4 task 01 |
| the base in the Walter's store; the CTRL-AP and UICR behind one door; `rak provision` | Walter firmware, phase 5 |
| on silicon: a factory RUI3 radio provisioned and given its first image by the Walter | Walter firmware, phase 6 task 03 |
| the catalogue takes a release that carries a base | meshmaster.dk, pile 4.16 task 133 |
| the bench tool that did it first, before the firmware had it | `software/firmware/walter-swd-bench/` in the hardware repository |

## Later, not built

`rak provision` as a MeshMaster job: an audited control action behind a second factor, naming the
radio's device ID, for a radio swapped in the field. The command is written so that the job is a
caller of the same sequence, not a second one.
