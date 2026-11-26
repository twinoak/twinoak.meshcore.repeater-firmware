# Phase 3.18 — a scope beside a loud neighbour

Written on 26 November.

**What happened.** Phase 3.17 is proven on the unit at Bellevue (sessions 12 to 15,
`tasks/phase-3.17/FOLLOWUPS.md`): the default settle takes the block off each segment's
first point, and `stepped` takes the far echoes out of the wide picture. Thomas on session
15's 180 MHz picture: *very happy*. On its 2 MHz and 13 MHz pictures: the neighbour's
packets are *smeared out* over the whole picture.

**What that is.** While the neighbour sends (-34 dBm here), every reading within about 4 MHz
of it rises from the floor to about -70 dBm: all 250 points of the 2 MHz picture, 195 of 250
of the 13 MHz picture, 17 of 386 of the 180 MHz picture. A station at -65 dBm on the same
channel raises nothing beside itself. Both ways of sweeping show it, at every bandwidth, the
same width in MHz. **My reading, not proven:** the chip's gain control turning down for a
signal that strong inside its analogue filter.

**What this pile builds.** One task: `scope` takes a word, `gain<N>`, that holds the
receiver at one gain for that scope. The unit then says whether the rise is the gain
control, and which gain a site with a loud neighbour is measured at.

**Chosen by me, his to change before the run:**

- **A word on the line, and nobody's default** (his: *as a parameter such that it can be
  done if one has a loud repeater nearby*).
- **The chip's own numbers for the gains**, not decibels: the data sheet names thirteen
  gains and no figure for each.
- **Semtech's word or nothing.** The data sheet says a manual gain mode exists and not how
  it is entered. The session finds it in what Semtech has published, or merges its report
  alone: no register is written on a guess.
- **Levels are not corrected for the gain.** There is no table for this board; the garden's
  session measures what a held gain's levels are off by.
- **`held=<N>` in the `ack`**, because `gain=` is already the word for `boost`/`noboost`.

## Order and dependencies

| # | Task | Depends on | What it leaves behind |
|---|---|---|---|
| 01 | [a-scope-at-a-fixed-gain](01-a-scope-at-a-fixed-gain.md) | — | `scope … gain<N>`: the receiver held at one gain for that scope, both ways of sweeping; the `ack` says `held=`; `echoes.py --steps` counts the rise; the list `garden-f` |

```powershell
.\tasks\run-all.ps1 -Phase '3.18'
```

No board is needed.

## Models

- **Opus**: 01.

## Afterwards (Thomas)

- **Release.** I import it, mark it canary for Risskov LTE, fetch the scanner image and
  run `garden-f` when you say so. The repeater's own image does not change in this pile.
- **Then meshmaster.dk:** if a held gain takes the rise away, a choice on the session form
  for a site with a loud neighbour (a task there, after the session).

## Follow-ups

Review findings go into [FOLLOWUPS.md](FOLLOWUPS.md).
