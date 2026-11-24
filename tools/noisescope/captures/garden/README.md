# The garden's downloads

A NoiseScope session's lines, as they came off its page. The echoes of a wide `scope` have only
ever been seen on the unit at Bellevue — with its antenna, its cavity filter and the neighbour
200 m away — and the bench has never shown one, so this is where they are measured
(`tasks/phase-3.16/README.md`, *The garden instead of the bench*).

**Where a download goes.** On the session's page, *Download the lines*. Save the file **here,
under its own name, unedited**: `noisescope-<repeater>-<yyyyMMdd-HHmmss>.ndjson`, every line as
the scanner sent it. The name says which repeater and when, which is the only thing that tells
two sessions of the same program apart; renaming it or trimming it throws that away. Nothing is
edited and nothing is merged — one session, one file.

**The one command over it:**

```bash
python tools/noisescope/echoes.py --steps <download.ndjson> --carrier 869.618
```

One table across the session's steps in the order they were asked — the span, the bandwidth, the
points and segments, the width and settle, the regulator and the gain, the frames and the frames
that hold the signal, the echoes with their distance from their segment's first point and from the
carrier, the time a point cost against both figures for an RSSI update, and the chip's error word —
and under it **every two of its scopes that differ in exactly one thing**, each with what that
pair says. A pair of one shape begun in two places answers this pile's question outright: *stays
with the frequency*, *moves with the segments* or *cannot say* (`docs/noisescope.md` §7).

`--carrier 869.618` is the neighbour's own frequency, which is the signal that lights the echoes.
Add `--json` for the same table as data, and `--lit` / `--over` to move the two thresholds.

**What the sessions are.** `tools/noisescope/lists/garden-a.txt` to `garden-d.txt`, and
`python tools/noisescope/hunt.py --sessions <list>` prints each as the lines to paste into the
form, with what it will bring back and how long Bellevue is off the mesh for it.

| File | What it is |
|---|---|
| — | **Empty.** No session had been run when phase 3.16's task 04 was written: `garden-a` needs nothing new and is Thomas's to start whenever he likes, and `garden-b`, `-c` and `-d` need a release carrying that pile's tasks 02 and 03 and *Fetch a scanner image* on the unit (`tasks/phase-3.16/reports/04.md`, *Not verified here*). |

A download is the unit's own bytes. The `ack` of each step is in it, as the scanner sent it, and
`--steps` needs it: two steps of the same span told apart — one on the DC-DC converter and one on
the LDO, say — are two scopes only because their `ack`s differ, and the segment width and the
settle are named nowhere else at all.
