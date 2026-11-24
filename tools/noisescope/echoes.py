#!/usr/bin/env python3
"""The echoes of a wide `scope`, counted - and what they follow.

    python tools/noisescope/echoes.py <capture.ndjson> [--carrier 869.618]
    python tools/noisescope/echoes.py <capture.ndjson> --json
    python tools/noisescope/echoes.py --check-plans

A wide `scope` of the unit at Bellevue (meshmaster.dk `/noisescope/3`, 23 November 2026) showed
bright blocks at frequencies where nothing can be, behind a cavity filter that passes 863 to
870 MHz, in every frame that held a neighbour's carrier on 869.618 MHz. Read by hand, those
blocks began **at the first point of a segment of the sweep and again about every 5.6 MHz after
it** (tasks/phase-3.16/README.md). This counts them instead: every run of points that stands over
the floor while the carrier is there, with its distance from its own segment's first point in
**MHz, in points and in microseconds**, and - across the scopes of one capture - which of those
three the distances keep.

Why three units. Inside one `scope` they are the same number three times: a point is one RSSI
update, so a distance of k points is k x step MHz and k x 1.6/rbw ms, whatever k is. One capture
of one span can therefore never tell them apart, and this tool says so rather than picking one.
What tells them apart is **two scopes with different steps or bandwidths**: an echo that follows
the frequency keeps its interval in MHz and changes it in points, one that follows the sweep's own
bookkeeping keeps its interval in points, one that follows the clock keeps it in microseconds.
That is what the verdict line reads off a capture that holds several.

What *over the floor* means here. The floor is read **across the frequency axis and not across
time**: with a steady carrier every frame holds it, so there is no quiet frame to subtract, and
the floor jumps where a segment does. So each segment gets its own, the tenth percentile of its
points - not the median, because blocks twelve points apart that fade over ten cover most of a
segment and a median would sit inside them. `--floor` says it outright for a segment its echoes
fill, and `--over` is how far over it a point must stand to be counted.

The segments are laid out by the same arithmetic as the firmware's `planScope()` and
`scopeSegment()` (src/roles/scanner/Sweep.cpp), which is where it lives; the copy below is held to
it by **tools/noisescope/scope_plans.json**, a table of plans that both this tool's tests
(tools/noisescope/test_echoes.py) and the firmware's own (test/test_scanner/test_scanner.cpp,
`Scope.ThePlanTableIsPlanScopes`) read. `--check-plans` is that check by hand.

Standard library only, like tools/bench: the tests run in the release step of tools/verify.ps1
(tools/release/selfcheck.py), which has no numpy and no matplotlib - so this does not import
tools/noisescope/waterfall_plot.py, and the level arithmetic below is that file's
`scope_levels_to_dbm()` written out again.

Exit codes: 0 whether there were echoes or not; 1 for a capture that cannot be read (no file, no
`scope` frame in it, or a frame whose levels do not decode). A line that is not a `scope` line is
skipped without complaint, and so is a point the radio read nothing for (`255`).
"""

import argparse
import base64
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PLAN_TABLE = os.path.join(HERE, "scope_plans.json")

# ----------------------------------------------------------------------------- the firmware's
#                                                                               own numbers
#
# src/roles/scanner/Sweep.h and Sweep.cpp at this commit. Each one is a constant of the firmware
# and not a choice of this tool's; scope_plans.json is what holds the arithmetic below to theirs.

RBW_KHZ = (4.8, 5.8, 7.3, 9.7, 11.7, 14.6, 19.5, 23.4, 29.3, 39.0, 46.9, 58.6,
           78.2, 93.8, 117.3, 156.2, 187.2, 234.3, 312.0, 373.6, 467.0)
RADIO_LOW_MHZ = 150.0
RADIO_HIGH_MHZ = 960.0
MIN_POINTS = 16
MAX_POINTS = 2048
DEFAULT_POINTS = 250
PLUGIN_MAX_POINTS = 250
SEGMENT_MHZ = 32.0
MIN_STEP_KHZ = 0.1
RBW_MAX_KHZ = 467.0
INTERVAL_MAX_MS = 60000
# An RSSI register update takes about 1.6 ms / RBW in kHz, and a point is one update
# (kScopeUpdateMsKhz, and the plugin's own comment in vendor/sx126x_spectrum/spectrum.asm).
UPDATE_MS_KHZ = 1.6
# The level byte the radio never reports (kScopeNoReading), and the scale of the ones it does:
# -2 x dBm as GetRssiInst reports it, 0.5 dB a step (docs/noisescope.md section 5).
NO_READING = 255


def narrowest_rbw_khz(khz):
    """`narrowestRbwKhz()`: the narrowest receiver bandwidth at least `khz` wide."""
    for one in RBW_KHZ:
        if one >= khz:
            return one
    return RBW_KHZ[-1]


def pick_rbw_khz(step_khz):
    """`pickRbwKhz()`: the bandwidth a step gets when none was asked for."""
    return narrowest_rbw_khz(step_khz * 1.1)


def point_us(rbw_khz):
    """One point, in microseconds: one RSSI update at that bandwidth."""
    return 1000.0 * UPDATE_MS_KHZ / rbw_khz


class Plan(object):
    """`ScopePlan`: what the firmware would run for a request. Fields are that struct's."""

    def __init__(self, f0, f1, first_mhz, step_khz, rbw_khz, points, segments):
        self.f0 = f0
        self.f1 = f1
        self.first_mhz = first_mhz
        self.step_khz = step_khz
        self.rbw_khz = rbw_khz
        self.points = points
        self.segments = segments

    @property
    def step_mhz(self):
        return self.step_khz / 1000.0

    @property
    def cov(self):
        return self.rbw_khz / self.step_khz

    @property
    def point_us(self):
        return point_us(self.rbw_khz)

    def centre_mhz(self, index):
        """The centre of point `index` of the frame, in MHz."""
        return self.first_mhz + index * self.step_mhz

    def as_dict(self):
        return {"f0": self.f0, "f1": self.f1, "first_mhz": self.first_mhz,
                "step_khz": self.step_khz, "rbw_khz": self.rbw_khz, "points": self.points,
                "segments": self.segments, "cov": self.cov, "point_us": self.point_us}


class Segment(object):
    """`ScopeSegment`: one plugin call. `first` is the index of its first point in the frame."""

    def __init__(self, index, first, points, first_mhz, cal_lo_mhz, cal_hi_mhz):
        self.index = index
        self.first = first
        self.points = points
        self.first_mhz = first_mhz
        self.cal_lo_mhz = cal_lo_mhz
        self.cal_hi_mhz = cal_hi_mhz

    @property
    def last(self):
        return self.first + self.points - 1

    def as_dict(self):
        return {"index": self.index, "first": self.first, "points": self.points,
                "first_mhz": self.first_mhz, "cal_lo_mhz": self.cal_lo_mhz,
                "cal_hi_mhz": self.cal_hi_mhz}


def plan_scope(f0, f1, points=0, rbw_khz=0.0, interval_ms=0):
    """`planScope()`, line for line. None for a request this firmware does not run."""
    if f1 <= f0 or not (RADIO_LOW_MHZ <= f0 <= RADIO_HIGH_MHZ):
        return None
    if not RADIO_LOW_MHZ <= f1 <= RADIO_HIGH_MHZ:
        return None
    if rbw_khz < 0.0:
        return None
    if interval_ms > INTERVAL_MAX_MS:
        return None
    if points != 0 and not MIN_POINTS <= points <= MAX_POINTS:
        return None

    span_khz = (f1 - f0) * 1000.0

    # A bandwidth asked for is a resolution asked for, so the point count follows it; with
    # neither given the default is one plugin call and the bandwidth follows the step.
    rbw = narrowest_rbw_khz(rbw_khz) if rbw_khz > 0.0 else 0.0
    if points == 0:
        if rbw > 0.0:
            needed = math.ceil(span_khz / rbw)
            points = int(min(max(needed, MIN_POINTS), MAX_POINTS))
        else:
            points = DEFAULT_POINTS

    step_khz = span_khz / float(points)
    if step_khz < MIN_STEP_KHZ:
        return None
    if rbw <= 0.0:
        rbw = pick_rbw_khz(step_khz)

    # A segment is at most PLUGIN_MAX_POINTS points and measures at most SEGMENT_MHZ: its first
    # point's centre to its last one's, plus half a receiver bandwidth at each end. One point
    # always fits, however wide a step is.
    step_mhz = step_khz / 1000.0
    rbw_mhz = rbw / 1000.0
    per_segment = math.floor((SEGMENT_MHZ - rbw_mhz) / step_mhz) + 1.0
    per_segment = min(per_segment, float(PLUGIN_MAX_POINTS))
    per_segment = max(per_segment, 1.0)
    max_points = int(per_segment)
    segments = (points + max_points - 1) // max_points

    return Plan(f0=f0, f1=f1, first_mhz=f0 + step_mhz / 2.0, step_khz=step_khz, rbw_khz=rbw,
                points=int(points), segments=int(segments))


def scope_segment(plan, index):
    """`scopeSegment()`: segment `index` of a plan, or None for one it does not have."""
    if plan.points == 0 or plan.segments == 0 or index >= plan.segments or index < 0:
        return None
    base = plan.points // plan.segments
    extra = plan.points % plan.segments
    count = base + (1 if index < extra else 0)
    first = index * base + (index if index < extra else extra)
    step_mhz = plan.step_khz / 1000.0
    half_rbw_mhz = plan.rbw_khz / 2000.0
    first_mhz = plan.f0 + (first + 0.5) * step_mhz
    return Segment(index=index, first=first, points=count, first_mhz=first_mhz,
                   cal_lo_mhz=first_mhz - half_rbw_mhz,
                   cal_hi_mhz=first_mhz + (count - 1) * step_mhz + half_rbw_mhz)


def segments_of(plan):
    return [scope_segment(plan, i) for i in range(plan.segments)]


def plan_of_frame(f0_point_mhz, step_khz, points, rbw_khz):
    """The plan a `scope` frame came from.

    A frame carries the **first point's centre** and the step, not the span that was asked for
    (docs/noisescope.md section 5), so the span is read back from them and the point count and
    bandwidth are handed to `planScope()` as the command's own. With the point count given the
    step is span / points again, so the plan that comes back is the plan the frame came from.
    """
    step_mhz = step_khz / 1000.0
    f0 = f0_point_mhz - step_mhz / 2.0
    f1 = f0 + points * step_mhz
    return plan_scope(f0, f1, points=points, rbw_khz=rbw_khz)


# ----------------------------------------------------------------------------- the levels

def levels_to_dbm(encoded, n):
    """One `lv` field -> a list of dBm, with None where the radio read nothing.

    tools/noisescope/waterfall_plot.py's `scope_levels_to_dbm()` without numpy: one byte a point,
    base64, each -2 x dBm as the SX126x reports it; `255` is no reading.
    """
    try:
        raw = base64.b64decode(encoded, validate=True)
    except Exception:
        return None
    if len(raw) < n:
        return None
    out = []
    for byte in raw[:n]:
        out.append(None if byte == NO_READING else -0.5 * byte)
    return out


def median(values):
    return quantile(values, 0.5)


def quantile(values, q):
    """The q-th quantile of `values`, interpolated between the two ranks it falls between."""
    ordered = sorted(values)
    if not ordered:
        return None
    position = q * (len(ordered) - 1)
    low = int(math.floor(position))
    high = int(math.ceil(position))
    if low == high:
        return ordered[low]
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


# ----------------------------------------------------------------------------- the capture

class Scope(object):
    """One `scope` of a capture: its ack, its frames, and the plan they came from."""

    def __init__(self, ack, config):
        self.ack = ack
        self.config = config      # (f0_point_mhz, step_khz, points, rbw_khz)
        self.frames = []
        self.plan = None
        self.segments = []
        self.ack_segments = None  # `seg` of the ack, when there was one

    @property
    def sps(self):
        """Sweeps a second, as the frames carry it (`sps` is x10). The median of the frames."""
        values = [frame["sps"] / 10.0 for frame in self.frames if "sps" in frame]
        return median(values)

    @property
    def agg(self):
        for frame in self.frames:
            if frame.get("agg"):
                return str(frame["agg"])
        return ""


def ack_segments(ack):
    """`seg=<n>` of a `scope` ack's `msg`, or None."""
    if not ack:
        return None
    for word in str(ack.get("msg", "")).split():
        if word.startswith("seg="):
            try:
                return int(word[4:])
            except ValueError:
                return None
    return None


def read_capture(path_or_lines):
    """The `scope`s of a capture, in the order they were asked for.

    A new `scope` begins at an `ack` for one, and at any frame whose span, step, point count or
    bandwidth differs from the frames before it - which is what a second command looks like in a
    capture with no ack in it. Everything else in the file is skipped: a `boot`, a `stat`, an
    `err`, a comment of tools/bench's own, a line that is not JSON at all.
    """
    if isinstance(path_or_lines, str):
        with open(path_or_lines, "r", encoding="utf-8", errors="replace") as handle:
            lines = handle.readlines()
    else:
        lines = list(path_or_lines)

    scopes = []
    pending_ack = None
    current = None
    skipped = 0
    bad_frames = 0
    for text in lines:
        text = text.strip()
        if not text.startswith("{"):
            if text:
                skipped += 1
            continue
        try:
            obj = json.loads(text)
        except ValueError:
            skipped += 1
            continue
        if not isinstance(obj, dict):
            skipped += 1
            continue
        kind = obj.get("t")
        if kind == "ack" and str(obj.get("cmd", "")).lower() == "scope":
            pending_ack = obj if obj.get("ok", True) else None
            current = None
            continue
        if kind != "scope":
            skipped += 1
            continue
        try:
            config = (float(obj["f0"]), float(obj["step"]), int(obj["n"]),
                      float(obj.get("rbw", 0.0)))
        except (KeyError, TypeError, ValueError):
            bad_frames += 1
            continue
        if config[2] <= 0 or config[1] <= 0.0 or config[3] <= 0.0:
            bad_frames += 1
            continue
        if current is None or current.config != config:
            current = Scope(pending_ack, config)
            current.ack_segments = ack_segments(pending_ack)
            pending_ack = None
            current.plan = plan_of_frame(*config)
            if current.plan is None:
                bad_frames += 1
                current = None
                continue
            current.segments = segments_of(current.plan)
            scopes.append(current)
        dbm = levels_to_dbm(obj.get("lv", ""), config[2])
        if dbm is None:
            bad_frames += 1
            continue
        frame = dict(obj)
        frame["dbm"] = dbm
        current.frames.append(frame)
    return scopes, skipped, bad_frames


# ----------------------------------------------------------------------------- the echoes

# How far either side of the carrier's own point its energy is looked for when deciding whether a
# frame holds it: the plugin files the reading of a point under the point it retuned to `latency`
# updates ago, so a carrier can land a point along (spectrum.asm, and deliverable 4 of
# tasks/phase-3.16/01-the-echoes-are-measured.md).
CARRIER_WINDOW_POINTS = 2

# Which quantile of a segment's points is taken for its floor. **Not the median**: the blocks of
# 23 November are twelve points apart and fade over five to fifteen, so they cover most of a
# segment and a median sits inside them rather than under them. A low quantile is under them as
# long as a tenth of a segment's points are still floor; a segment its echoes fill has no floor
# to find and `--floor` is then the way to say what it is.
FLOOR_QUANTILE = 0.10

UNITS = ("mhz", "points", "us")
UNIT_NAMES = {"mhz": "MHz", "points": "points", "us": "us"}


class Run(object):
    """A run of consecutive points standing over the floor, inside one segment."""

    def __init__(self, scope, first, last, levels):
        plan = scope.plan
        self.first = first
        self.last = last
        self.points = last - first + 1
        self.segment = segment_of(scope, first)
        self.first_mhz = plan.centre_mhz(first)
        self.distance_points = first - self.segment.first
        self.distance_mhz = self.distance_points * plan.step_mhz
        self.distance_us = self.distance_points * plan.point_us
        self.length_mhz = self.points * plan.step_mhz
        self.length_us = self.points * plan.point_us
        inside = [levels[i] for i in range(first, last + 1) if levels[i] is not None]
        self.peak_dbm = max(inside) if inside else None
        self.peak_point = None
        for i in range(first, last + 1):
            if levels[i] is not None and levels[i] == self.peak_dbm:
                self.peak_point = i
                break

    def distance(self, unit):
        return {"mhz": self.distance_mhz, "points": float(self.distance_points),
                "us": self.distance_us}[unit]

    def as_dict(self, carrier_dbm):
        return {
            "first_point": self.first,
            "first_mhz": self.first_mhz,
            "segment": self.segment.index,
            "segment_first_point": self.segment.first,
            "segment_first_mhz": self.segment.first_mhz,
            "distance_mhz": self.distance_mhz,
            "distance_points": self.distance_points,
            "distance_us": self.distance_us,
            "peak_dbm": self.peak_dbm,
            "peak_point": self.peak_point,
            "below_carrier_db": (None if self.peak_dbm is None or carrier_dbm is None
                                 else carrier_dbm - self.peak_dbm),
            "length_points": self.points,
            "length_mhz": self.length_mhz,
            "length_us": self.length_us,
        }


def segment_of(scope, index):
    for seg in scope.segments:
        if seg.first <= index <= seg.last:
            return seg
    return scope.segments[-1]


# An interval small enough explains any set of numbers, so two things bound one.
#
# **It is at least three points.** The blocks of 23 November are five to fifteen points long, so a
# spacing below three is nothing a reader of a picture could see, and it is at this tool's own
# resolution.
#
# **And the multiples it explains the distances with are not a sparse set.** A comb has a block at
# very nearly *every* multiple of its interval from where it starts - 0, 12, 24, 36, 48, 60 - while
# an unrelated handful of peaks fits some small interval only by accident and lands on an arbitrary
# few of its multiples. The nine spurs of a real dummy-load baseline (`scope 150 960 2048`, the
# bench of 22 October) were otherwise "every one and a half points", on multiples 10, 14, 17, 30,
# 33, 41 and 52 of it: nothing anybody would call an interval. So at least this fraction of the
# multiples from the first to the last have to be there. Not a cap on the multiple itself: a
# segment of 220 points really does hold thirty-seven blocks six points apart.
MIN_INTERVAL_POINTS = 3
MIN_MULTIPLE_COVERAGE = 0.5
MAX_MULTIPLE = 64        # how far the candidates are generated, not a bound on an answer


def repeats(distances, tolerance, max_multiple=MAX_MULTIPLE):
    """The largest interval every distance is a whole multiple of, to within `tolerance`.

    (interval, worst miss) or None. The candidates are each non-zero distance over each whole
    number up to `max_multiple`, largest first; `tolerance` is half a point in whichever unit the
    distances are in, and the two constants above say what is not an interval.
    """
    if len(distances) < 2:
        return None
    nonzero = [d for d in distances if d > tolerance]
    if not nonzero:
        return None
    least = 2.0 * tolerance * MIN_INTERVAL_POINTS      # tolerance is half a point
    candidates = set()
    for d in nonzero:
        for k in range(1, max_multiple + 1):
            value = d / k
            if value >= least:
                candidates.add(value)
    for interval in sorted(candidates, reverse=True):
        worst = 0.0
        multiples = set()
        ok = True
        for d in distances:
            multiple = int(round(d / interval))
            if d > tolerance and multiple < 1:
                ok = False
                break
            multiples.add(multiple)
            worst = max(worst, abs(d - multiple * interval))
        if not ok or worst > tolerance:
            continue
        top = max(multiples)
        if top > 0 and len(multiples) < MIN_MULTIPLE_COVERAGE * (top + 1):
            continue    # a sparse set of multiples is not a comb
        return interval, worst
    return None


class ScopeReading(object):
    """One `scope` of a capture, read: its carrier, its floor, its echoes and their interval."""

    def __init__(self, scope, carrier_mhz=None, lit_dbm=-70.0, over_db=6.0, floor_dbm=None,
                 floor_quantile=FLOOR_QUANTILE):
        self.scope = scope
        self.plan = scope.plan
        self.lit_dbm = lit_dbm
        self.over_db = over_db
        self.floor_dbm = floor_dbm
        self.floor_quantile = floor_quantile
        self.notes = []
        self.carrier_point = None
        self.carrier_mhz_asked = carrier_mhz
        self.carrier_dbm = None
        self.lit = 0
        self.echoes = []
        self.floors = {}
        self.interval = dict((unit, None) for unit in UNITS)
        self.levels = []
        self.carrier_run = None
        self._read()

    # ---- the carrier

    def _carrier_level(self, levels):
        """A frame's carrier level: its own point and the two either side, the strongest of them."""
        lo = max(0, self.carrier_point - CARRIER_WINDOW_POINTS)
        hi = min(self.plan.points - 1, self.carrier_point + CARRIER_WINDOW_POINTS)
        inside = [levels[i] for i in range(lo, hi + 1) if levels[i] is not None]
        return max(inside) if inside else None

    def _pick_carrier(self):
        plan = self.plan
        if self.carrier_mhz_asked is not None:
            index = int(round((self.carrier_mhz_asked - plan.first_mhz) / plan.step_mhz))
            if 0 <= index < plan.points:
                return index
            self.notes.append("the carrier at %.4f MHz is outside this scope's span (%.4f to "
                              "%.4f MHz): the strongest point was taken instead"
                              % (self.carrier_mhz_asked, plan.f0, plan.f1))
        best, best_dbm = None, None
        for frame in self.scope.frames:
            for i, value in enumerate(frame["dbm"]):
                if value is None:
                    continue
                if best_dbm is None or value > best_dbm:
                    best, best_dbm = i, value
        return best

    # ---- the reading

    def _read(self):
        plan = self.plan
        frames = self.scope.frames
        if not frames:
            self.notes.append("no frame")
            return
        self.carrier_point = self._pick_carrier()
        if self.carrier_point is None:
            self.notes.append("no point of any frame carries a reading")
            return

        lit = [frame for frame in frames
               if (self._carrier_level(frame["dbm"]) or -999.0) >= self.lit_dbm]
        self.lit = len(lit)
        if not lit:
            self.notes.append("no frame of %d holds the carrier at %.4f MHz above %.1f dBm "
                              "(--lit): nothing is read"
                              % (len(frames), plan.centre_mhz(self.carrier_point), self.lit_dbm))
            return

        # A point's level is the median over the frames that hold the carrier: an echo made by
        # the carrier is in every one of them, and a median is not moved by one noisy frame.
        levels = []
        for i in range(plan.points):
            values = [frame["dbm"][i] for frame in lit if frame["dbm"][i] is not None]
            levels.append(median(values))
        self.levels = levels
        self.carrier_dbm = self._carrier_level(levels)

        # The floor is **spatial and not temporal**: a low quantile of a segment's own points.
        # With a steady carrier every frame is lit, so a quiet frame to subtract does not exist;
        # and the floor jumps at a segment boundary, which is why it is per segment and not per
        # capture (FLOOR_QUANTILE says why it is not the median).
        for seg in self.scope.segments:
            inside = [levels[i] for i in range(seg.first, seg.last + 1) if levels[i] is not None]
            self.floors[seg.index] = (self.floor_dbm if self.floor_dbm is not None
                                      else quantile(inside, self.floor_quantile))

        # Every run of points over that floor, a run never crossing a segment boundary - the
        # distance below is measured from a segment's first point, so a run in two segments has
        # two of them.
        runs = []
        for seg in self.scope.segments:
            start = None
            for i in range(seg.first, seg.last + 1):
                floor = self.floors[seg.index]
                over = (levels[i] is not None and floor is not None
                        and levels[i] >= floor + self.over_db)
                if over and start is None:
                    start = i
                elif not over and start is not None:
                    runs.append(Run(self.scope, start, i - 1, levels))
                    start = None
            if start is not None:
                runs.append(Run(self.scope, start, seg.last, levels))

        # The carrier's own run is not an echo.
        carrier_runs = [run for run in runs
                        if run.first - CARRIER_WINDOW_POINTS <= self.carrier_point
                        <= run.last + CARRIER_WINDOW_POINTS]
        if not carrier_runs:
            self.notes.append("the carrier's own point does not stand %.1f dB over its segment's "
                              "floor: nothing was taken for it" % self.over_db)
        self.carrier_run = carrier_runs[0] if carrier_runs else None
        self.echoes = [run for run in runs if run not in carrier_runs]

        # The interval, in each of the three units. Inside one scope the three are the same
        # number three times (the module's docstring): the tolerance is half a point in each, so
        # the worst miss is the same fraction of the interval in all three and a capture of one
        # scope cannot tell them apart.
        distances = {
            "mhz": [run.distance_mhz for run in self.echoes],
            "points": [float(run.distance_points) for run in self.echoes],
            "us": [run.distance_us for run in self.echoes],
        }
        tolerance = {"mhz": plan.step_mhz / 2.0, "points": 0.5, "us": plan.point_us / 2.0}
        for unit in UNITS:
            self.interval[unit] = repeats(distances[unit], tolerance[unit])

    # ---- what it says

    @property
    def carrier_offset_points(self):
        """Where the carrier's own peak sits against the point it was asked for.

        The plugin files a reading under the point it retuned to `latency` updates ago
        (`latencyFor()` of vendor/sx126x_spectrum/SX126xSpectrumScanner.h: 2, 3 or 4). A latency
        that is right for this bandwidth puts the peak on the carrier's own point; one that is
        not shifts it, and this is that shift.
        """
        if self.carrier_run is None or self.carrier_run.peak_point is None:
            return None
        return self.carrier_run.peak_point - self.carrier_point

    def summary(self):
        """The one line: whether the distances repeat, in which unit, the interval, the miss."""
        plan = self.plan
        if not self.echoes:
            if self.lit:
                return ("no echo: no run of points outside the carrier's own stands %.1f dB over "
                        "its segment's floor in the %d frame(s) that hold the carrier"
                        % (self.over_db, self.lit))
            return "no echo: " + (self.notes[-1] if self.notes else "nothing was read")
        found = [unit for unit in UNITS if self.interval[unit]]
        if not found:
            return ("%d echo(es), and their distances do not repeat: %s from their segments' "
                    "first points" % (len(self.echoes), self._distance_list()))
        parts = []
        for unit in UNITS:
            if not self.interval[unit]:
                continue
            interval, worst = self.interval[unit]
            parts.append("%s %s (worst miss %s)"
                         % (number_in(interval, unit), UNIT_NAMES[unit], number_in(worst, unit)))
        return ("%d echo(es) in %d segment(s): the distances repeat every %s - one point is "
                "%.4f MHz and %.2f us here, so these three are one number and this scope alone "
                "cannot tell them apart"
                % (len(self.echoes), len(set(run.segment.index for run in self.echoes)),
                   ", ".join(parts), plan.step_mhz, plan.point_us))

    def _distance_list(self):
        return ", ".join("%.4f MHz (%d points)" % (run.distance_mhz, run.distance_points)
                         for run in self.echoes)

    def as_dict(self):
        return {
            "plan": self.plan.as_dict(),
            "segments": [seg.as_dict() for seg in self.scope.segments],
            "ack": self.scope.ack,
            "ack_segments": self.scope.ack_segments,
            "frames": len(self.scope.frames),
            "lit_frames": self.lit,
            "sps": self.scope.sps,
            "agg": self.scope.agg,
            "carrier": {
                "asked_mhz": self.carrier_mhz_asked,
                "point": self.carrier_point,
                "mhz": (None if self.carrier_point is None
                        else self.plan.centre_mhz(self.carrier_point)),
                "dbm": self.carrier_dbm,
                "offset_points": self.carrier_offset_points,
            },
            "floors_dbm": dict((str(k), v) for k, v in self.floors.items()),
            "echoes": [run.as_dict(self.carrier_dbm) for run in self.echoes],
            "interval": dict((unit, (None if not self.interval.get(unit)
                                     else {"interval": self.interval[unit][0],
                                           "worst_miss": self.interval[unit][1]}))
                             for unit in UNITS),
            "summary": self.summary(),
            "notes": list(self.notes),
        }


def number_in(value, unit):
    """A value in one of the three units, as many digits as that unit is worth."""
    if unit == "points":
        return "%.1f" % value
    if unit == "mhz":
        return "%.4f" % value
    return "%.2f" % value


# ----------------------------------------------------------------------------- the verdict

# How far two scopes' intervals in one unit may differ and still be called the same: a step and a
# bandwidth differ between the scopes being compared, so the intervals are not expected to agree
# to the digit. Five per cent is a point or two of a twelve-point interval.
SAME_INTERVAL_FRACTION = 0.05


def verdict(readings):
    """Which of the three units the interval keeps across the scopes of one capture.

    Returns (unit or None, one line). A capture whose scopes all have the same step and bandwidth
    cannot answer it, and says so: inside one step the three units are one number.
    """
    with_interval = [r for r in readings if any(r.interval.values())]
    if not with_interval:
        return None, "no scope of this capture has an interval: there is nothing to follow"
    shapes = set((round(r.plan.step_khz, 6), r.plan.rbw_khz) for r in with_interval)
    if len(with_interval) < 2 or len(shapes) < 2:
        return None, ("one step and one bandwidth: a point is %.4f MHz and %.2f us throughout, so "
                      "MHz, points and us are the same number and nothing here tells them apart. "
                      "Two scopes of different step or bandwidth do (--help)."
                      % (with_interval[0].plan.step_mhz, with_interval[0].plan.point_us))

    kept = []
    spread = {}
    for unit in UNITS:
        values = [r.interval[unit][0] for r in with_interval if r.interval[unit]]
        if len(values) < len(with_interval):
            spread[unit] = None
            continue
        lo, hi = min(values), max(values)
        spread[unit] = (lo, hi, values)
        if lo > 0.0 and (hi - lo) / lo <= SAME_INTERVAL_FRACTION:
            kept.append(unit)

    def shown(unit):
        if spread[unit] is None:
            return "%s: not in every scope" % UNIT_NAMES[unit]
        return "%s: %s" % (UNIT_NAMES[unit],
                           ", ".join(number_in(v, unit) for v in spread[unit][2]))

    detail = "; ".join(shown(unit) for unit in UNITS)
    if len(kept) == 1:
        return kept[0], ("the interval is the same in %s across %d scopes of different step or "
                         "bandwidth, and not in the other two - the echoes follow the %s (%s)"
                         % (UNIT_NAMES[kept[0]], len(with_interval),
                            {"mhz": "frequency", "points": "sweep's own points",
                             "us": "clock"}[kept[0]], detail))
    if not kept:
        return None, ("no unit's interval holds across the %d scopes: the echoes follow none of "
                      "the three as this capture reads (%s)" % (len(with_interval), detail))
    return None, ("the interval holds in more than one unit (%s), so these scopes do not tell "
                  "them apart - two with a different step *and* a different bandwidth do (%s)"
                  % (", ".join(UNIT_NAMES[u] for u in kept), detail))


# ----------------------------------------------------------------------------- the table

def check_plans(path=PLAN_TABLE, out=sys.stdout):
    """Every plan of scope_plans.json against the arithmetic above. True when all of them agree.

    The same file test/test_scanner/test_scanner.cpp reads for `planScope()` itself, which is
    what holds this copy to the firmware's (the module's docstring).
    """
    with open(path, "r", encoding="utf-8") as handle:
        table = json.load(handle)
    bad = 0
    for entry in table["plans"]:
        name = entry["name"]
        req = entry["request"]
        plan = plan_scope(req["f0"], req["f1"], points=req.get("points", 0),
                          rbw_khz=req.get("rbw_khz", 0.0))
        want = entry["plan"]
        if plan is None:
            out.write("FAIL %s: this firmware does not run that request\n" % name)
            bad += 1
            continue
        problems = []
        for key, tol in (("points", 0), ("segments", 0), ("step_khz", 1e-9),
                         ("rbw_khz", 1e-9), ("first_mhz", 1e-9)):
            got = getattr(plan, key)
            if abs(got - want[key]) > tol:
                problems.append("%s %r, not %r" % (key, got, want[key]))
        got_segments = segments_of(plan)
        if len(got_segments) != len(entry["segments"]):
            problems.append("%d segments, not %d" % (len(got_segments), len(entry["segments"])))
        else:
            for seg, expect in zip(got_segments, entry["segments"]):
                for key, tol in (("first", 0), ("points", 0), ("first_mhz", 1e-9),
                                 ("cal_lo_mhz", 1e-9), ("cal_hi_mhz", 1e-9)):
                    got = getattr(seg, key)
                    if abs(got - expect[key]) > tol:
                        problems.append("segment %d: %s %r, not %r"
                                        % (seg.index, key, got, expect[key]))
        if problems:
            bad += 1
            out.write("FAIL %s: %s\n" % (name, "; ".join(problems)))
        else:
            out.write("ok   %s: %d points, %d segment(s), step %.6f kHz, rbw %.1f kHz\n"
                      % (name, plan.points, plan.segments, plan.step_khz, plan.rbw_khz))
    out.write("%d plan(s), %d disagreeing with %s\n"
              % (len(table["plans"]), bad, os.path.basename(path)))
    return bad == 0


# ----------------------------------------------------------------------------- the table printed

def print_report(path, readings, skipped, bad_frames, out=sys.stdout):
    out.write("%s\n" % path)
    if skipped or bad_frames:
        out.write("  %d line(s) that are not a `scope` frame, %d frame(s) that would not read\n"
                  % (skipped, bad_frames))
    for number, reading in enumerate(readings, 1):
        plan = reading.plan
        scope = reading.scope
        out.write("\nscope %d of %d: %.4f to %.4f MHz, %d points, step %.4f kHz, rbw %.1f kHz, "
                  "%d segment(s)\n"
                  % (number, len(readings), plan.f0, plan.f1, plan.points, plan.step_khz,
                     plan.rbw_khz, plan.segments))
        if scope.ack_segments is not None and scope.ack_segments != plan.segments:
            out.write("  the ack says seg=%d and this plan has %d: the frame and the ack "
                      "disagree\n" % (scope.ack_segments, plan.segments))
        sps = scope.sps
        out.write("  %d frame(s), %d holding the carrier; agg %s; sps %s; one point is %.4f MHz "
                  "and %.2f us\n"
                  % (len(scope.frames), reading.lit, scope.agg or "?",
                     "?" if sps is None else "%.1f a second" % sps,
                     plan.step_mhz, plan.point_us))
        if reading.carrier_point is not None:
            out.write("  carrier: point %d, %.4f MHz, %s dBm%s\n"
                      % (reading.carrier_point, plan.centre_mhz(reading.carrier_point),
                         "?" if reading.carrier_dbm is None else "%.1f" % reading.carrier_dbm,
                         "" if reading.carrier_offset_points is None
                         else ", its peak %+d point(s) from where it was asked for"
                              % reading.carrier_offset_points))
        for note in reading.notes:
            out.write("  note: %s\n" % note)
        if reading.echoes:
            out.write("  %-10s %-4s %-9s %-7s %-8s %-7s %-7s %-6s %-8s %s\n"
                      % ("first MHz", "seg", "from MHz", "points", "us", "peak", "below",
                         "len pt", "len MHz", "len us"))
            for run in reading.echoes:
                out.write("  %-10.4f %-4d %-9.4f %-7d %-8.1f %-7s %-7s %-6d %-8.4f %.1f\n"
                          % (run.first_mhz, run.segment.index, run.distance_mhz,
                             run.distance_points, run.distance_us,
                             "?" if run.peak_dbm is None else "%.1f" % run.peak_dbm,
                             "?" if run.peak_dbm is None or reading.carrier_dbm is None
                             else "%.1f" % (reading.carrier_dbm - run.peak_dbm),
                             run.points, run.length_mhz, run.length_us))
        out.write("  %s\n" % reading.summary())
    unit, line = verdict(readings)
    out.write("\nwhat they follow: %s\n" % line)
    return unit


def main(argv=None, out=sys.stdout):
    parser = argparse.ArgumentParser(
        prog="echoes", description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture", nargs="?", help="a NoiseScope NDJSON capture")
    parser.add_argument("--carrier", type=float, default=None, metavar="MHZ",
                        help="the frequency the carrier is on (default: the strongest point)")
    parser.add_argument("--lit", type=float, default=-70.0, metavar="DBM",
                        help="a frame holds the carrier when it reads at least this (default -70)")
    parser.add_argument("--over", type=float, default=6.0, metavar="DB",
                        help="a point is over the floor when it stands this far over it "
                             "(default 6)")
    parser.add_argument("--floor", type=float, default=None, metavar="DBM",
                        help="the floor, said outright, for a segment its echoes fill (default: "
                             "the %.0fth percentile of each segment's own points)"
                             % (100 * FLOOR_QUANTILE))
    parser.add_argument("--floor-quantile", type=float, default=FLOOR_QUANTILE,
                        dest="floor_quantile", metavar="Q",
                        help="which quantile of a segment's points its floor is (default %.2f)"
                             % FLOOR_QUANTILE)
    parser.add_argument("--json", action="store_true", help="the same, as JSON")
    parser.add_argument("--check-plans", action="store_true", dest="check_plans",
                        help="hold this tool's segments to %s and exit"
                             % os.path.basename(PLAN_TABLE))
    args = parser.parse_args(argv)

    if args.check_plans:
        return 0 if check_plans(out=out) else 1
    if not args.capture:
        parser.error("a capture, or --check-plans")
    if not os.path.isfile(args.capture):
        out.write("cannot read %s: no such file\n" % args.capture)
        return 1

    scopes, skipped, bad_frames = read_capture(args.capture)
    scopes = [scope for scope in scopes if scope.frames]
    if not scopes:
        out.write("cannot read %s: no `scope` frame in it (%d line(s) skipped, %d frame(s) that "
                  "would not read)\n" % (args.capture, skipped, bad_frames))
        return 1
    readings = [ScopeReading(scope, carrier_mhz=args.carrier, lit_dbm=args.lit, over_db=args.over,
                             floor_dbm=args.floor, floor_quantile=args.floor_quantile)
                for scope in scopes]
    if args.json:
        unit, line = verdict(readings)
        out.write(json.dumps({
            "capture": args.capture,
            "skipped_lines": skipped,
            "unreadable_frames": bad_frames,
            "carrier_mhz": args.carrier,
            "lit_dbm": args.lit,
            "over_db": args.over,
            "floor_dbm": args.floor,
            "floor_quantile": args.floor_quantile,
            "scopes": [reading.as_dict() for reading in readings],
            "follows": unit,
            "verdict": line,
        }, indent=2, sort_keys=True) + "\n")
    else:
        print_report(args.capture, readings, skipped, bad_frames, out=out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
