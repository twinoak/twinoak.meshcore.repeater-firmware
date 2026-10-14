# Proposal 02 — a firmware catalogue, and updates as jobs

**For:** `twinoak/meshmaster.dk`. **Status:** accepted 2026-10-14 — built in the core, not as a pack (meshmaster.dk D-45), as pile 4.14, tasks 122–127; the job types are `twinoak.fw.offer` / `.apply` / `.state` and `twinoak.power.cycle`. Written 2026-10-11. Read against
`spec/PLUGINS.md` §5 and §6 (`twinoak.ota.*`, `twinoak_firmware_images`, `twinoak_ota_runs`) and
[`../FIRMWARE-MANIFEST.md`](../FIRMWARE-MANIFEST.md).

## The idea in one paragraph

A release of either firmware is a signed manifest and its files. MeshMaster keeps those in a
catalogue, knows which unit runs what, and lets a person start an update — one unit first, then
the rest. The unit downloads the bytes from MeshMaster, checks the signature itself, flashes,
checks its own health, and keeps or rolls back; MeshMaster records every step. **The server never
decides that an image is good.** It cannot sign one, and a unit does not take its word.

## Two halves, and which is generic

**Generic, for every repeater in the register** — worth having without any TwinOak hardware:

- *Which firmware runs where.* The register already records `firmware` from `status` and
  `config.read`. A fleet page: version, how many, which are behind the newest upstream tag (the
  `upstream-firmware` watcher already knows the tag).

**The TwinOak pack** — only for units that can be flashed remotely:

- the catalogue of *our* releases, the update jobs, the history.

## The catalogue

- **Import.** A person gives a release (a GitHub Release URL, or uploads `manifest.json`, its
  `.sig` and the files). The server checks the signature against the release public key it has
  been configured with — not because units trust the server, but so that a bad upload is refused
  at the door — validates the manifest, checks each file's size and SHA-256, and stores them.
- **Rows.** One per manifest (`product`, `version`, `commit`, `link` version, imported by, when),
  one per image (`role`, `board`, `band`, `requires`), one per file (`use`, hash, size, where the
  bytes are). Files are content-addressed by hash.
- **Serving.** `GET /agent/v1/packs/twinoak/firmware/{manifestId}/manifest` (+ `.sig`) and
  `GET …/files/{sha256}`, with `Range` support. The unit is on LTE-M at a few kilobytes a second
  with a data plan measured in megabytes: downloads must resume, and the server should send
  nothing the unit did not ask for.
- **State.** A release is `imported`, `canary` (allowed on named units), `released` (offered to
  all compatible units) or `withdrawn` (never offered again; units that run it are listed).

## The update, as jobs

PLUGINS §6 names `twinoak.ota.stage` / `.trigger` / `.status` / `.confirm`. With the unit doing
its own health check and rollback (`SYSTEM.md` S-5, S-6), the server's part is smaller:

| Job | What the unit does | Result |
|---|---|---|
| `twinoak.fw.offer` | fetch the manifest and signature; verify; pick the image for the named slot; download and verify the file; store it as the candidate. Lease-extended, progress reported in bytes | `stored` with the image id and hash, or the check that failed, by number (`FIRMWARE-MANIFEST.md` §5) |
| `twinoak.fw.apply` | if the preconditions hold, flash the candidate, health-check, commit or roll back | `committed` or `rolledBack` with the journal: each step, its time, the reason for a rollback |
| `twinoak.fw.state` | nothing on the air: report what each slot runs, what golden and candidate images are held, the last update's journal | the inventory |

Two jobs rather than one because the download and the flash want different moments: the download
whenever there is signal and energy, the flash when a person has said go and the battery is good.

**Preconditions** the unit enforces and reports when they stop it: battery above a threshold and
not falling; the link to the RAK up; the clock set; no other update in progress; not both
firmwares in one wake (S-7); and, for a Walter update, room to roll back.

**Order across products.** When a release changes the link's major version, the catalogue knows
(`link.major` in both manifests) and the UI offers the Walter's update first; the repeater's
`requires.walter` makes the unit refuse the wrong order anyway.

**Canary.** A release is first offered to one unit a person names. The fleet page shows the canary
as such, and *release to all* is a second, gated action available once the canary has reported
`committed` and has been heard from again after it.

**Audit.** Importing, offering, applying, withdrawing: each is an audited control action with who,
when and why. A rollback is not an error to hide: it is the system working, and the journal is
shown as it is.

## What the unit needs from the agent protocol

Nothing new in the core: pack routes under `/agent/v1/packs/twinoak/…`, lease extension and
progress, which exist for exactly this. The one thing to check is the result size of a journal
(bounded at the unit; a few hundred bytes).

## Not proposed

Automatic rollout. An update is started by a person, every time. Scheduling one is fine; a server
that decides by itself to flash a mast is not.
