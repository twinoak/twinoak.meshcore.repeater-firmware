# Phase 3 — follow-ups

Review findings, tagged with the task that must pick them up: `[NN]` for a task of this phase.
A session reads this file before it starts (RUNBOOK.md section 0) and treats every open item
tagged for its task as part of the task. Only the reviewer ticks an item.

## Review of 01–03 (18 October) — the phase, after it landed

Read against the three reports, the task files and the code at `main` (`ac7f2d4`, after 3.1 and
3.2 had also landed). 12 October, 00:58 → 04:09, no stop (01 54 min, 02 61, 03 75); every task
merged in its first CI round. The exit criterion holds: the release dry run makes two images, each
with exactly one descriptor that agrees with the manifest, and `manifestref.py descriptor` reads
both. The structure is the one the phase asked for — a role is a directory and an environment, a
board a target and a table row — and the scanner compiles not one line of MeshCore. Three things
matter; the rest is small.

- [ ] **[3.3] The band's legal duty cycle is not in the band, and a fresh unit starts at five
  times it.** Upstream's repeater starts at `airtime_factor` 1.0 — a 50 % budget
  (`docs/ARCHITECTURE.md`'s duty-cycle section, `MyMesh.cpp:890` at `d9296435`) — and the
  home channel, 869.618 MHz, is in the 869.40–869.65 MHz band whose limit is **10 %** at 500 mW
  e.r.p. (ERC Recommendation 70-03, Annex 1). That section already says *only upstream's budget
  can hold the regulatory limit*; at the default it holds a different one. 03's report lists
  `dutycycle` with the Danish standard's 10 and `docs/provisioning.md` files it under *a judgement
  about the neighbourhood* — the 10 is not a judgement, it is the band's ceiling (a lower value
  may be one). It belongs in `[band_868]` beside the frequency and the power, and the profile's
  second line is generated from it.
- [ ] **[3.3] `tools/linkloop/check.sh` is in no gate.** 01's move left its seventeen includes
  stale and nothing noticed until 03 needed it (03, *Deviations*). It needs a host compiler, which
  the native step's container already has: run it there.
- [ ] **[3.3] Nothing on this phase's bench list has been walked**, and three of its lines decide
  whether work already merged is right: `radio_ok` with the power-enable pin driven high (02,
  *Decisions* 1), the scanner → repeater round trip that leaves the identity in place (the promise
  both scanner rails exist for), and the power cut during the first boot (03). With a RAK4630 on
  Thomas's USB port these are mechanical; 3.3 makes them a script.
- [ ] **[Thomas] The guest password of a fresh unit.** Empty, so a stranger's blank login is a
  guest (`MyMesh::handleLoginReq`, 03's report); the Danish standard is `hello`. The profile cannot
  carry it — no line of it may carry a password (S-13, and a test) — so it is either a decision to
  leave it to provisioning (`docs/provisioning.md` §2, as today) or a change to S-13 in the
  hardware repository for this one public word. MeshMaster's guest-door pairing (meshmaster.dk
  D-44) works either way once the owner stores what was set.
- [ ] **[Thomas] The rest of the Danish standard.** `flood.advert.interval` 60–85 h chosen from the
  node's own key (upstream: 47), `agc.reset.interval` 4, `loop.detect moderate`,
  `flood.max.unscoped` 15 — none of them per site in the way the three delays are. 03 left them
  to the reviewer; the reviewer leaves them to you, because each changes what a repeater does in
  the mesh rather than what it is: say which go in, and the next pile adds them as the duty cycle
  is added in 3.3.
- [ ] **[Thomas] The hardware repository's old NoiseScope** (`software/firmware/noisescope-rak4630/`)
  is still there; replace it with a pointer to this repository's `docs/noisescope.md` in its
  `PROJECT-KNOWLEDGE.md` §8 (02's first paragraph). No session of this repository writes there.
- [ ] **[reviewer]** 02's RadioLib decision is right and its reason is the one that matters: the
  repeater cannot build against MeshCore's pinned commit, so pinning it in the scanner would have
  made the two *differ*. "RSSI comparable with the repeater's" is now true by construction.

### Fine as it is

- **The descriptor** is a `const` object kept by its readers (01 proved `used` alone does not keep
  it under `--gc-sections`), the CRC in `constexpr`, one generator for the board and the fixtures.
- **The seam:** six role hooks with no weak default; a role links only what it reaches (02).
- **The scanner's rails** (rules 10 and 11) each failed once on purpose; the image never names a
  transmit or a file system.
- **Once is exact** (03): one preferences write for both decisions, held across `firstBoot()`;
  no second marker. The console's reply buffer fix is a real upstream overflow, measured.
- **Sizes:** repeater +164 (01), 0 (02), +1408 (03) flash; every byte itemised from the map.

## What p3.4-02 answered, on the first real board (22 October 2026)

The third `[3.3]` item above — *nothing on this phase's bench list has been walked* — is what this
is about. A RAK4630 on the production adapter, its base written by a probe, `rf: dummy-load`,
`identity: disposable`; `bench walk 1`, `8`, `9` and `10` at `9fb721b`, result files under
`out/bench/` and the detail in `tasks/phase-3.4/reports/02.md`. The boxes are the reviewer's to
tick; what the board said is:

- **`radio_ok` with the power-enable pin high: yes.** The scanner came up with
  `"radio_ok": 1`, `"radio": "sx1262"`, `"rom": "SX1261 V2D 2D02"` and answered `sweep`, `hist`,
  `dwell`, `cad` and `scope` — so driving `SX126X_POWER_EN` (P1.05) high is right for this stamp
  and 02's decision 1 holds (`out/bench/walk-9-20261022-174804-9fb721b.md`).
- **The scanner → repeater round trip keeps the identity: yes.** `Repeater ID`
  `A76C9F71…F997DE` before the scanner and the same after the repeater went back, with the name
  and the version (§9's own last step, the same file). It took **two DFUs**, for the reason below.
- **The power cut during a first boot: still a person's.** The exact moment:
  `bench console "erase"` with `identity: disposable`, then pull the USB power **within the first
  second of the boot that follows** — before the two `First boot:` lines appear — and on the next
  boot read `sensor get firstboot.state`, `get path.hash.mode` and `get dutycycle`. What must
  never be seen is no `First boot:` line together with `path.hash.mode` `0` or `dutycycle`
  `> 50.0%`. Three or four attempts; `docs/bench.md` §10 has the line.

### Open, from the same walks

- [ ] **[reviewer] The scanner's watchdog ends the DFU that would replace it.** `wdtBegin()`
  (`src/roles/scanner/Wdt.h`) starts the nRF52840's WDT at 8 s with `CONFIG.SLEEP = Run`, and a
  **soft reset does not stop it** — so after the 1200-baud touch the bootloader has about eight
  seconds to take a 414 KB image, and it does not get there: `adafruit-nrfutil` dies with
  `WriteFile failed (PermissionError(13) … ERROR_BAD_COMMAND)` or `Write timeout`, the watchdog
  resets the part mid-DFU, and the board comes back in its full bootloader (`239A:0029`) with no
  application. The second attempt, from that bootloader, works — there is no WDT running there.
  p3.4-02 made `tools/bench` survive it (it waits for the port the DFU died on to go, finds the
  bootloader the board is now in, and runs nrfutil once more), so **a scanner can always be
  flashed back, in two DFUs**; what it did not do is decide whether the scanner should keep a
  watchdog it cannot be updated through. Three ways out, for whoever owns that decision: leave it
  as it is and let the bench tool do the two DFUs (what happens today, and a person with a UF2
  drive does the same thing twice); have the role feed the WDT from a `dfu`-like console line that
  resets *after* a WDT-free window (there is none - the WDT cannot be stopped); or drop the
  hardware WDT for a software one. `docs/bench.md` §9 and §0a now say what a person should expect.
  Evidence: `out/bench/flash.log` (every flash of the session, with `what` and `result`) and
  `tasks/phase-3.4/reports/02.md`.
