#!/usr/bin/env python3
"""The echoes of a wide `scope`, counted - and what they follow.

    python tools/noisescope/echoes.py <capture.ndjson> [--carrier 869.618]
    python tools/noisescope/echoes.py <capture.ndjson> --json
    python tools/noisescope/echoes.py --steps <download.ndjson> --carrier 869.618
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

**`--steps` is the same tool, read as a session.** The echoes have only ever been seen on the
unit at Bellevue, so they are measured in a NoiseScope session started from meshmaster.dk and not
on a bench (tasks/phase-3.16/README.md, *The garden instead of the bench*): up to six typed steps,
one file of lines an hour later, and a signal that is a neighbour's traffic and so is in some
frames and not in others. `--steps` reads such a download as what it is - one table across the
session's scopes in the order they were asked, with the regulator, the gain, the width, the settle,
the time a point cost and the chip's error word in it - and then **every two of its scopes that
differ in exactly one thing**, which is the only comparison a garden capture can carry. A pair of
one shape begun in two places is the one that answers this pile's question, and it says *stays with
the frequency*, *moves with the segments* or *cannot say*. `tools/noisescope/hunt.py --sessions`
is where the lists that make those pairs are checked and printed.

Two things `--steps` reads differently from a bench capture, and nothing else:

  * **the floor is the scope's own quiet frames** when it has MIN_QUIET_FRAMES of them or more -
    the same point with the carrier gone, which a bench with a steady generator never has - and
    each row says which rule it used;
  * a place's height in those quiet frames over its segment's floor **there** is printed beside
    it, because the frames of /noisescope/3 read the echoes' places 4 to 8 dB high even when
    quiet and that is part of what is hunted.

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
# The widest a segment may be is also the default, and `scope`'s sixth number asks for a narrower
# one; the seventh asks for a settle on the segment's first point (kScopeSettleMaxUs, phase 3.16's
# task 02). A settle changes nothing inside a segment, so it changes no distance this tool reads -
# it is carried because the ack names it and a reader of a table wants to know which line it was.
SETTLE_MAX_US = 20000
# And what a `scope` that names no settle gets (kScopeSettleDefaultUs, phase 3.17's task 01):
# 500 microseconds, which is what four NoiseScope sessions on the unit at Bellevue measured the
# block at a segment's first point away (tasks/phase-3.16/FOLLOWUPS.md, *What the garden said*).
SETTLE_DEFAULT_US = 500
# "No ninth number on the line", which `plan_scope()` turns into SETTLE_DEFAULT_US - `0` typed
# there is none at all. kScopeSettleAbsentUs, and above SETTLE_MAX_US for the same reason.
SETTLE_ABSENT_US = 0xFFFFFFFF
# An RSSI register update takes about 1.6 ms / RBW in kHz, and a point is one update
# (kScopeUpdateMsKhz, and the plugin's own comment in vendor/sx126x_spectrum/spectrum.asm).
UPDATE_MS_KHZ = 1.6
# And what the part's own maker says it is: 0.94 ms / RBW in kHz, read off the table of RSSI
# settling times Rev 2.2 of the SX1261/2 data sheet added to `GetRssiInst` (13.5.4, table 13-82;
# tasks/phase-3.16/FOLLOWUPS.md, *Rev 2.2 of the data sheet, read against Rev 1.2*). The two
# differ by a factor of 1.7 and nothing on a board has told them apart yet, so a frame's `us`
# over its points is printed with **both** beside it and neither is called the answer.
SEMTECH_UPDATE_MS_KHZ = 0.94
# The envelope of a `scope` line, which with the base64 of one byte a point is the most bytes a
# frame can be, newline included: `scopeFrameBytes()` of src/roles/scanner/Sweep.cpp, 246 since
# phase 3.16's task 03 put `us`, `usx` and `dev` on the line (its report: 762 bytes at 386
# points, 582 at 250). What a session's step costs the Walter is worked out from this.
FRAME_ENVELOPE_BYTES = 246
# A frame may take at most one part in this of its sink's time, which is what gives a `scope` on
# the Walter's 115200 baud UART its interval floor (kScopeSinkDutyDivisor).
SINK_DUTY_DIVISOR = 2
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


def point_us(rbw_khz, update_ms_khz=UPDATE_MS_KHZ):
    """One point, in microseconds: one RSSI update at that bandwidth."""
    return 1000.0 * update_ms_khz / rbw_khz


def base64_length(raw_bytes):
    """`scopeBase64Length()`: what `raw_bytes` bytes come to, base64-encoded."""
    return 4 * ((int(raw_bytes) + 2) // 3)


def frame_bytes(points):
    """`scopeFrameBytes()`: the most bytes a frame of `points` points is, newline included."""
    return FRAME_ENVELOPE_BYTES + base64_length(points)


def interval_floor_ms(points, baud):
    """`scopeIntervalFloorMs()`: the shortest interval a sink of `baud` bits a second carries it at.

    8N1, so ten bits a byte, rounded up, and then the frame may have only one part in
    SINK_DUTY_DIVISOR of the port's time. 0 for `baud` 0. This is why a `scope` started on the
    Walter's UART does not get the interval it asked for, and the `ack` says the one it got.
    """
    if baud <= 0:
        return 0
    bits = frame_bytes(points) * 10
    return ((bits * 1000 + baud - 1) // baud) * SINK_DUTY_DIVISOR


class Plan(object):
    """`ScopePlan`: what the firmware would run for a request. Fields are that struct's."""

    def __init__(self, f0, f1, first_mhz, step_khz, rbw_khz, points, segments,
                 seg_mhz=SEGMENT_MHZ, settle_us=0):
        self.f0 = f0
        self.f1 = f1
        self.first_mhz = first_mhz
        self.step_khz = step_khz
        self.rbw_khz = rbw_khz
        self.points = points
        self.segments = segments
        self.seg_mhz = seg_mhz
        self.settle_us = settle_us

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
                "segments": self.segments, "cov": self.cov, "point_us": self.point_us,
                "seg_mhz": self.seg_mhz, "settle_us": self.settle_us}


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


def plan_scope(f0, f1, points=0, rbw_khz=0.0, interval_ms=0, seg_mhz=0.0,
               settle_us=SETTLE_ABSENT_US):
    """`planScope()`, line for line. None for a request this firmware does not run.

    `settle_us` is resolved here as the firmware resolves it: SETTLE_ABSENT_US - a line with no
    ninth number - is SETTLE_DEFAULT_US, and only a `0` is none at all.
    """
    if f1 <= f0 or not (RADIO_LOW_MHZ <= f0 <= RADIO_HIGH_MHZ):
        return None
    if not RADIO_LOW_MHZ <= f1 <= RADIO_HIGH_MHZ:
        return None
    if rbw_khz < 0.0:
        return None
    if interval_ms > INTERVAL_MAX_MS:
        return None
    if settle_us == SETTLE_ABSENT_US:
        settle_us = SETTLE_DEFAULT_US
    if settle_us > SETTLE_MAX_US or settle_us < 0:
        return None
    if seg_mhz < 0.0:
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

    # A segment is at most PLUGIN_MAX_POINTS points and measures at most the width asked for -
    # SEGMENT_MHZ when none was: its first point's centre to its last one's, plus half a receiver
    # bandwidth at each end. One point always fits, however wide a step is.
    step_mhz = step_khz / 1000.0
    rbw_mhz = rbw / 1000.0
    width = SEGMENT_MHZ
    if seg_mhz > 0.0:
        width = math.floor(seg_mhz * 10.0 + 0.5) / 10.0   # a tenth of a MHz is taken
        if width > SEGMENT_MHZ or width < step_mhz - 1e-9:
            return None
    per_segment = math.floor((width - rbw_mhz) / step_mhz) + 1.0
    per_segment = min(per_segment, float(PLUGIN_MAX_POINTS))
    per_segment = max(per_segment, 1.0)
    max_points = int(per_segment)
    segments = (points + max_points - 1) // max_points

    return Plan(f0=f0, f1=f1, first_mhz=f0 + step_mhz / 2.0, step_khz=step_khz, rbw_khz=rbw,
                points=int(points), segments=int(segments), seg_mhz=width,
                settle_us=int(settle_us))


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


def plan_of_frame(f0_point_mhz, step_khz, points, rbw_khz, seg_mhz=0.0, settle_us=0):
    # `settle_us` defaults to 0 and not to SETTLE_ABSENT_US on purpose: a frame carries no settle,
    # and a capture with no `ack` to read one out of is a capture from a firmware that had no
    # default - every line typed before phase 3.17's task 01 stood for no time at all.
    """The plan a `scope` frame came from.

    A frame carries the **first point's centre** and the step, not the span that was asked for
    (docs/noisescope.md section 5), so the span is read back from them and the point count and
    bandwidth are handed to `planScope()` as the command's own. With the point count given the
    step is span / points again, so the plan that comes back is the plan the frame came from.

    A frame does **not** carry the segment width or the settle: those are the `ack`'s
    (`segw=`, `settle=`), which is why a capture whose `scope` was asked for with them has to be
    read with its ack - `read_capture()` does that, and a capture with no ack in it is read as a
    `scope` asked for with neither, which is what every line typed before phase 3.16's task 02 is.
    """
    step_mhz = step_khz / 1000.0
    f0 = f0_point_mhz - step_mhz / 2.0
    f1 = f0 + points * step_mhz
    return plan_scope(f0, f1, points=points, rbw_khz=rbw_khz, seg_mhz=seg_mhz,
                      settle_us=settle_us)


# --------------------------------------------------------------------------- a `scope` line

SCOPE_AGG_WORDS = ("max", "mean", "last")
SCOPE_REGULATOR_WORDS = ("ldo", "dcdc")
SCOPE_GAIN_WORDS = ("boost", "noboost")


def _as_double(text, fallback=0.0):
    """`asDouble()`: `atof`, which is the leading number of the word and 0 for no number at all."""
    out = ""
    for index, char in enumerate(text):
        if char in "+-" and index == 0:
            out += char
        elif char.isdigit() or char == ".":
            out += char
        else:
            break
    try:
        return float(out)
    except ValueError:
        return fallback


def _as_u32(text, fallback=0):
    """`asU32()`: `strtoul` base ten, which is 0 for a word that does not begin with a digit."""
    out = ""
    for char in text:
        if char.isdigit():
            out += char
        else:
            break
    return int(out) if out else fallback


class ScopeRequest(object):
    """What a `scope` line asks for, and the plan it would be run as.

    `parseScope()` (src/roles/scanner/Commands.cpp) in Python, which is what lets a tool price a
    typed line before anybody types it: `.plan` is `planScope()`'s answer and is None for numbers
    this firmware refuses. `.regulator` and `.gain` are the words of phase 3.16's task 03, taken
    out of the line wherever they stand after the two ends, and None when the line named neither -
    which is the build's regulator and the gain `boost on|off` left, not `dcdc` and not `boost`.
    """

    def __init__(self, f0, f1, points=0, rbw_khz=0.0, interval_ms=0, agg="max", seg_mhz=0.0,
                 settle_us=SETTLE_ABSENT_US, regulator=None, gain=None):
        self.f0 = f0
        self.f1 = f1
        self.points = points
        self.rbw_khz = rbw_khz
        self.interval_ms = interval_ms
        self.agg = agg
        self.seg_mhz = seg_mhz
        self.settle_us = settle_us
        self.regulator = regulator
        self.gain = gain
        self.plan = plan_scope(f0, f1, points=points, rbw_khz=rbw_khz, interval_ms=interval_ms,
                               seg_mhz=seg_mhz, settle_us=settle_us)


def parse_scope(line):
    """A `scope` line as the firmware reads it, or None for one it does not run.

    None for: a line whose first word is not `scope`, `scope off` (it asks for no sweep), one end
    where two are required, both words of a pair (`kScopeWordRefusal`) and an aggregate that is
    not one of three. A line whose *numbers* this firmware refuses is a request all the same and
    comes back with `.plan` None, because the refusal is `planScope()`'s and a caller wants to
    tell the two apart.
    """
    words = line.split()
    if not words or words[0] != "scope":
        return None
    if len(words) >= 2 and words[1] == "off":
        return None
    if len(words) < 3:
        return None

    # The four words first and wherever they stand **after the two ends**, exactly as
    # `parseScope()` takes them, so that what is left is read as it was read before they existed.
    regulator = gain = None
    rest = list(words[1:3])
    for word in words[3:]:
        if word in SCOPE_REGULATOR_WORDS:
            if regulator is not None:
                return None     # one word of each pair at most
            regulator = word
            continue
        if word in SCOPE_GAIN_WORDS:
            if gain is not None:
                return None
            gain = word
            continue
        rest.append(word)

    agg = "max"
    if len(rest) >= 6:
        if rest[5] not in SCOPE_AGG_WORDS:
            return None
        agg = rest[5]
    return ScopeRequest(
        f0=_as_double(rest[0]), f1=_as_double(rest[1]),
        points=(_as_u32(rest[2]) if len(rest) >= 3 else 0),
        rbw_khz=(_as_double(rest[3]) if len(rest) >= 4 else 0.0),
        interval_ms=(_as_u32(rest[4]) if len(rest) >= 5 else 0),
        agg=agg,
        seg_mhz=(_as_double(rest[6]) if len(rest) >= 7 else 0.0),
        settle_us=(_as_u32(rest[7]) if len(rest) >= 8 else SETTLE_ABSENT_US),
        regulator=regulator, gain=gain)


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
        self.ack_seg_mhz = None   # and `segw`/`settle`, which no frame carries
        self.ack_settle_us = None
        self.ack_regulator = None  # `reg=ldo|dcdc` of the ack, or None: the line named no word
        self.ack_gain = None       # `gain=boost|noboost`, or None for the same reason

    @property
    def us(self):
        """(shortest, longest) of the plugin's own calls over this scope's frames, in microseconds.

        `us` and `usx` of a frame are the shortest and the longest whole sweep in it counting the
        radio program's calls alone (docs/noisescope.md section 5), so the shortest over a scope
        is the least `us` of its frames and the longest the greatest `usx`. (None, None) for a
        capture made before phase 3.16's task 03, whose frames carry neither.
        """
        low = [frame["us"] for frame in self.frames if _is_number(frame.get("us"))]
        high = [frame["usx"] for frame in self.frames if _is_number(frame.get("usx"))]
        return (min(low) if low else None, max(high) if high else None)

    @property
    def device_errors(self):
        """`dev` OR-ed over this scope's frames, or None when no frame carries one.

        None is both of the two reasons a frame has no `dev`: a capture made before phase 3.16's
        task 03, and a `scope` of one segment, which arms once and so has no standby to read the
        word in. The table says which, from the plan.
        """
        word = None
        for frame in self.frames:
            one = device_errors_of(frame)
            if one is None:
                continue
            word = one if word is None else (word | one)
        return word

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
    return _ack_number(ack, "seg=", int)


def ack_seg_mhz(ack):
    """`segw=<n>MHz` of a `scope` ack's `msg`, or None when the line asked for no width.

    The ack names the two numbers of phase 3.16's task 02 only when one of them is not the
    default (`scopeAckMessage()`, src/roles/scanner/Sweep.cpp), so None here means 32 MHz - what
    a `scope` asked for without them takes, and what every capture made before that task is.
    """
    return _ack_number(ack, "segw=", float, "MHz")


def ack_settle_us(ack):
    """`settle=<n>us` of a `scope` ack's `msg`, or None."""
    return _ack_number(ack, "settle=", int, "us")


def ack_regulator(ack):
    """`reg=ldo` or `reg=dcdc` of a `scope` ack's `msg`, or None when the line named no word."""
    return _ack_word(ack, "reg=", SCOPE_REGULATOR_WORDS)


def ack_gain(ack):
    """`gain=boost` or `gain=noboost` of a `scope` ack's `msg`, or None for the same reason.

    None is *not* `boost`: the gain a scope with no word of its own runs at is whatever
    `boost on|off` left, which the ack does not say and no capture can recover
    (`scopeAckMessage()`, src/roles/scanner/Sweep.cpp; phase 3.16's task 03, decision 2).
    """
    return _ack_word(ack, "gain=", SCOPE_GAIN_WORDS)


def _ack_word(ack, prefix, allowed):
    if not ack:
        return None
    for word in str(ack.get("msg", "")).split():
        if word.startswith(prefix):
            text = word[len(prefix):]
            return text if text in allowed else None
    return None


def _is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


# The bits of the chip's `GetDeviceErrors` word this repository names, and nothing beyond them:
# the three constants of src/roles/scanner/Sweep.h and the image calibration's, which
# docs/noisescope.md section 5 reads off the SX1261/2 data sheet's table 13-86. A bit outside this
# list is printed as its own number rather than given a meaning no file here carries.
DEVICE_ERROR_BITS = (
    (0x0040, "the PLL did not lock"),
    (0x0020, "the crystal oscillator did not start"),
    (0x0010, "an image calibration failed"),
    (0x0004, "a PLL calibration failed"),
)


def device_errors_of(frame):
    """`dev` of a frame as a number, or None when the frame carries none.

    It is four lowercase hexadecimal digits in a JSON **string** (`Lines.cpp`: `,"dev":"%04x"`),
    so a frame whose `dev` is not that is read as no `dev` at all rather than as zero.
    """
    text = frame.get("dev")
    if not isinstance(text, str):
        return None
    try:
        return int(text, 16)
    except ValueError:
        return None


def device_errors_said(word):
    """A `dev` word in words: "0000, nothing" or the bits it holds, each named or numbered."""
    if word is None:
        return "-"
    if word == 0:
        return "0000, nothing"
    said = []
    left = word
    for bit, name in DEVICE_ERROR_BITS:
        if word & bit:
            said.append(name)
            left &= ~bit
    for shift in range(16):
        if left & (1 << shift):
            said.append("bit %d, which no file here names" % shift)
    return "%04x: %s" % (word, "; ".join(said))


def _ack_number(ack, prefix, cast, suffix=""):
    if not ack:
        return None
    for word in str(ack.get("msg", "")).split():
        if word.startswith(prefix):
            text = word[len(prefix):]
            if suffix and text.endswith(suffix):
                text = text[:-len(suffix)]
            try:
                return cast(text)
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
            current.ack_seg_mhz = ack_seg_mhz(pending_ack)
            current.ack_settle_us = ack_settle_us(pending_ack)
            current.ack_regulator = ack_regulator(pending_ack)
            current.ack_gain = ack_gain(pending_ack)
            pending_ack = None
            # The width is the ack's or it is the default: it is the one thing of the plan a
            # frame cannot say, and laying the segments out wrong puts every echo in the wrong
            # one (phase 3.16's task 02).
            current.plan = plan_of_frame(*config, seg_mhz=(current.ack_seg_mhz or 0.0),
                                         settle_us=(current.ack_settle_us or 0))
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
        # How high this place reads in the scope's own **quiet** frames, over its segment's floor
        # there. The frames of /noisescope/3 read the echoes' places 4 to 8 dB high even with no
        # carrier in them, which is part of what is hunted and not a thing the lit frames can
        # say. None unless the scope was read by the quiet-frame rule (ScopeReading).
        self.quiet_over_floor_db = None

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
            "quiet_over_floor_db": self.quiet_over_floor_db,
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


# How many quiet frames a scope needs before its own quiet frames are what its echoes are read
# over. The bench's carrier is in every frame and there are none; a garden capture's signal is a
# neighbour's traffic, there in some frames and not in others - ten of sixty in session 3 - and
# then the same place read with no carrier in it is a truer floor than any quantile of the
# frequency axis, because it is the same receiver at the same frequency. Five, because a median
# of fewer is one frame's noise (phase 3.16's task 04, deliverable 5).
MIN_QUIET_FRAMES = 5


class ScopeReading(object):
    """One `scope` of a capture, read: its carrier, its floor, its echoes and their interval.

    `quiet_rule` is the garden's: with MIN_QUIET_FRAMES frames or more that do **not** hold the
    carrier, a point counts as an echo when it stands `over_db` over *the same point's* median in
    those quiet frames, instead of over its segment's floor across the frequency axis. `.rule`
    says which was used, and `--floor` said outright keeps the floor whatever the frames are.
    """

    def __init__(self, scope, carrier_mhz=None, lit_dbm=-70.0, over_db=6.0, floor_dbm=None,
                 floor_quantile=FLOOR_QUANTILE, quiet_rule=False):
        self.scope = scope
        self.plan = scope.plan
        self.lit_dbm = lit_dbm
        self.over_db = over_db
        self.floor_dbm = floor_dbm
        self.floor_quantile = floor_quantile
        self.quiet_rule = quiet_rule
        self.notes = []
        self.carrier_point = None
        self.carrier_mhz_asked = carrier_mhz
        self.carrier_dbm = None
        self.lit = 0
        self.quiet = 0
        self.echoes = []
        self.floors = {}
        self.quiet_levels = []
        self.quiet_floors = {}
        self.rule = "floor"
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

        lit, quiet = [], []
        for frame in frames:
            if (self._carrier_level(frame["dbm"]) or -999.0) >= self.lit_dbm:
                lit.append(frame)
            else:
                quiet.append(frame)
        self.lit = len(lit)
        self.quiet = len(quiet)
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

        # And the garden's rule, when the frames allow it and no floor was said outright: the
        # same point's median in the frames that do **not** hold the carrier. A quiet frame is
        # the same receiver at the same frequency with the signal gone, so what it reads there is
        # the floor of that place and not of its segment.
        self._read_quiet(quiet)

        # Every run of points over that floor, a run never crossing a segment boundary - the
        # distance below is measured from a segment's first point, so a run in two segments has
        # two of them.
        runs = []
        for seg in self.scope.segments:
            start = None
            for i in range(seg.first, seg.last + 1):
                threshold = self._threshold(i, seg)
                over = (levels[i] is not None and threshold is not None
                        and levels[i] >= threshold)
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
        for run in runs:
            run.quiet_over_floor_db = self._quiet_over_floor(run)

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

    # ---- the two floors

    def _read_quiet(self, quiet):
        """The quiet frames' median per point, and a floor per segment read off that picture."""
        if self.floor_dbm is not None or not self.quiet_rule:
            return
        if len(quiet) < MIN_QUIET_FRAMES:
            self.notes.append("%d quiet frame(s), fewer than %d: the echoes are read over each "
                              "segment's own floor, as on a bench"
                              % (len(quiet), MIN_QUIET_FRAMES))
            return
        self.rule = "quiet"
        for i in range(self.plan.points):
            values = [frame["dbm"][i] for frame in quiet if frame["dbm"][i] is not None]
            self.quiet_levels.append(median(values))
        for seg in self.scope.segments:
            inside = [self.quiet_levels[i] for i in range(seg.first, seg.last + 1)
                      if self.quiet_levels[i] is not None]
            self.quiet_floors[seg.index] = quantile(inside, self.floor_quantile)

    def _threshold(self, index, seg):
        """What a point must read to count, in dBm: the quiet rule's or the floor's."""
        base = (self.quiet_levels[index] if self.rule == "quiet" else self.floors[seg.index])
        return None if base is None else base + self.over_db

    def _quiet_over_floor(self, run):
        """How high a place reads in the quiet frames over its segment's floor there."""
        if self.rule != "quiet":
            return None
        level = self.quiet_levels[run.first]
        floor = self.quiet_floors.get(run.segment.index)
        if level is None or floor is None:
            return None
        return level - floor

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
            "quiet_frames": self.quiet,
            "rule": self.rule,
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
            "quiet_floors_dbm": dict((str(k), v) for k, v in self.quiet_floors.items()),
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
                          rbw_khz=req.get("rbw_khz", 0.0), seg_mhz=req.get("seg_mhz", 0.0),
                          settle_us=req.get("settle_us", SETTLE_ABSENT_US))
        want = entry["plan"]
        if plan is None:
            out.write("FAIL %s: this firmware does not run that request\n" % name)
            bad += 1
            continue
        problems = []
        for key, tol in (("points", 0), ("segments", 0), ("step_khz", 1e-9),
                         ("rbw_khz", 1e-9), ("first_mhz", 1e-9), ("seg_mhz", 1e-9),
                         ("settle_us", 0)):
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


# ------------------------------------------------- a session's steps, as one table and its pairs
#
# A NoiseScope session is up to six typed steps and comes back as **one** file of lines
# (docs/noisescope.md section 7). So the thing to read is not a capture but a session: the scopes
# of the download in the order they were asked, one row each, and then every two of them that
# differ in exactly one thing - because a pair that differs in one thing is the only thing in a
# garden capture that answers anything, and the four lists of tools/noisescope/lists/ are written
# as such pairs.
#
# The rules below are these and nothing learned from the data: the thresholds are `--lit` and
# `--over`, as they are everywhere else in this file, and a place is the same place in two scopes
# when it is within **one step** of it.

# What the two scopes of a pair must agree on, and the one thing they may differ in. A width or a
# settle or either word changes no point and no step of a plan (phase 3.16's tasks 02 and 03), and
# a span begun elsewhere with the same step keeps its point count - so the signature below is
# equal in two scopes of a pair but for one field of PAIR_FIELDS.
PAIR_FIELDS = ("first_point", "width", "settle", "regulator", "gain")
PAIR_NAMES = {
    "first_point": "the first point",
    "width": "a segment's width",
    "settle": "the settle",
    "regulator": "the regulator",
    "gain": "the gain",
}

# How many echoes a scope needs before a shifted span is allowed to say anything. Two places and
# one shift fit either reading too easily; three is the fewest that can disagree.
MIN_ECHOES_FOR_A_VERDICT = 3

STAYS = "stays with the frequency"
MOVES = "moves with the segments"
CANNOT = "cannot say"


def _word(value):
    return "-" if value is None else str(value)


def _number(value, digits=2):
    return "-" if value is None else ("%.*f" % (digits, value))


def steps_rows(readings):
    """One row a scope, in the order they were asked: the plan, the frames, the places, the time."""
    rows = []
    for number, reading in enumerate(readings, 1):
        plan = reading.plan
        scope = reading.scope
        low, high = scope.us
        word = scope.device_errors
        carrier_mhz = (None if reading.carrier_point is None
                       else plan.centre_mhz(reading.carrier_point))
        places = []
        for run in reading.echoes:
            places.append({
                "first_mhz": run.first_mhz,
                "segment": run.segment.index,
                "from_segment_mhz": run.distance_mhz,
                "from_segment_points": run.distance_points,
                "from_carrier_mhz": (None if carrier_mhz is None
                                     else run.first_mhz - carrier_mhz),
                "peak_dbm": run.peak_dbm,
                "below_carrier_db": (None if run.peak_dbm is None or reading.carrier_dbm is None
                                     else reading.carrier_dbm - run.peak_dbm),
                "quiet_over_floor_db": run.quiet_over_floor_db,
                "length_points": run.points,
            })
        rows.append({
            "step": number,
            "f0": plan.f0,
            "f1": plan.f1,
            "rbw_khz": plan.rbw_khz,
            "points": plan.points,
            "segments": plan.segments,
            "seg_mhz": plan.seg_mhz,
            "settle_us": plan.settle_us,
            "regulator": scope.ack_regulator,
            "gain": scope.ack_gain,
            "agg": scope.agg,
            "frames": len(scope.frames),
            "lit_frames": reading.lit,
            "quiet_frames": reading.quiet,
            "rule": reading.rule,
            "has_ack": bool(scope.ack),
            "carrier_mhz": carrier_mhz,
            "carrier_dbm": reading.carrier_dbm,
            "places": places,
            "us_min": low,
            "us_max": high,
            # The one measurement the whole hunt wants a number for: how long a point really
            # costs, against the firmware's 1.6 ms / RBW and the part maker's 0.94.
            "us_per_point_min": (None if low is None else float(low) / plan.points),
            "us_per_point_max": (None if high is None else float(high) / plan.points),
            "point_us_at_1_6": point_us(plan.rbw_khz),
            "point_us_at_0_94": point_us(plan.rbw_khz, SEMTECH_UPDATE_MS_KHZ),
            "device_errors": word,
            "device_errors_said": device_errors_said(word),
            "summary": reading.summary(),
            "notes": list(reading.notes),
        })
    return rows


def _signature(reading):
    plan = reading.plan
    return {
        "step_khz": round(plan.step_khz, 6),
        "rbw_khz": plan.rbw_khz,
        "points": plan.points,
        "first_point": round(plan.first_mhz, 6),
        "width": round(plan.seg_mhz, 6),
        "settle": plan.settle_us,
        "regulator": reading.scope.ack_regulator,
        "gain": reading.scope.ack_gain,
    }


def one_difference(a, b):
    """Which single thing two scopes differ in, or None when it is none or more than one."""
    first, second = _signature(a), _signature(b)
    different = [key for key in sorted(first) if first[key] != second[key]]
    if len(different) != 1 or different[0] not in PAIR_FIELDS:
        return None
    return different[0]


def _same_place(one_mhz, others, tolerance):
    for other in others:
        if abs(other - one_mhz) <= tolerance:
            return True
    return False


def _multiple_of_an_interval(reading, shift_mhz, tolerance):
    """Whether a shift is a whole multiple of the interval this scope's echoes keep."""
    found = reading.interval["mhz"]
    if not found:
        return None
    interval = found[0]
    if interval <= 0.0:
        return None
    multiple = int(round(abs(shift_mhz) / interval))
    if multiple < 1:
        return None
    if abs(abs(shift_mhz) - multiple * interval) > tolerance:
        return None
    return (interval, multiple)


def first_point_pair(step_a, a, step_b, b):
    """Two scopes of one shape begun in different places: does an echo stay or move?

    For every echo of either, whether the other has one at the same frequency, at the same
    distance from its own segment's first point, at both or at neither - each within one step.
    Then the one line, with the counts.
    """
    tolerance = a.plan.step_mhz
    shift = b.plan.first_mhz - a.plan.first_mhz
    places = []
    for number, one, other in ((step_a, a, b), (step_b, b, a)):
        freqs = [run.first_mhz for run in other.echoes]
        distances = [run.distance_mhz for run in other.echoes]
        for run in one.echoes:
            places.append({
                "step": number,
                "first_mhz": run.first_mhz,
                "from_segment_mhz": run.distance_mhz,
                "same_frequency": _same_place(run.first_mhz, freqs, tolerance),
                "same_distance": _same_place(run.distance_mhz, distances, tolerance),
            })
    counted = {
        "frequency": sum(1 for one in places
                         if one["same_frequency"] and not one["same_distance"]),
        "distance": sum(1 for one in places
                        if one["same_distance"] and not one["same_frequency"]),
        "both": sum(1 for one in places if one["same_distance"] and one["same_frequency"]),
        "neither": sum(1 for one in places
                       if not one["same_distance"] and not one["same_frequency"]),
    }
    verdict, why = _first_point_verdict(step_a, a, step_b, b, shift, tolerance, counted)
    return {
        "kind": "first_point",
        "steps": [step_a, step_b],
        "differ": "%.4f MHz, step %d begun %s" % (abs(shift), step_b,
                                                  "lower" if shift < 0 else "higher"),
        "shift_mhz": shift,
        "echoes": [len(a.echoes), len(b.echoes)],
        "counted": counted,
        "places": places,
        "verdict": verdict,
        "why": why,
    }


def _first_point_verdict(step_a, a, step_b, b, shift, tolerance, counted):
    """The one line a shifted span is allowed to say, and why it is that one."""
    thin = [(number, len(one.echoes)) for number, one in ((step_a, a), (step_b, b))
            if len(one.echoes) < MIN_ECHOES_FOR_A_VERDICT]
    if thin:
        return CANNOT, ("fewer than %d echoes in %s, so neither reading can be told from the "
                        "other" % (MIN_ECHOES_FOR_A_VERDICT,
                                   " and ".join("step %d (%d)" % one for one in thin)))
    for number, one in ((step_a, a), (step_b, b)):
        found = _multiple_of_an_interval(one, shift, tolerance)
        if found:
            interval, multiple = found
            return CANNOT, ("the two first points differ by %.4f MHz, which is %d x the %.4f MHz "
                            "step %d's echoes keep: both readings predict the same places"
                            % (abs(shift), multiple, interval, number))
    counts = ("%d of %d places at the same frequency only, %d at the same distance only, %d at "
              "both, %d at neither"
              % (counted["frequency"], sum(counted.values()), counted["distance"],
                 counted["both"], counted["neither"]))
    if counted["frequency"] > counted["distance"]:
        return STAYS, counts
    if counted["distance"] > counted["frequency"]:
        return MOVES, counts
    return CANNOT, counts


def one_thing_pair(kind, step_a, a, step_b, b):
    """Two scopes that differ in a width, a settle, a regulator or a gain: who has what, and how
    far from the carrier. No verdict beyond the counts - what a word or a number *means* is the
    bench's and the garden's to say, not this tool's."""
    tolerance = a.plan.step_mhz
    mine = [run.first_mhz for run in a.echoes]
    theirs = [run.first_mhz for run in b.echoes]
    only_a = [run for run in a.echoes if not _same_place(run.first_mhz, theirs, tolerance)]
    only_b = [run for run in b.echoes if not _same_place(run.first_mhz, mine, tolerance)]
    shared = []
    for run in a.echoes:
        for other in b.echoes:
            if abs(other.first_mhz - run.first_mhz) <= tolerance:
                one = _below(a, run)
                two = _below(b, other)
                shared.append({"first_mhz": run.first_mhz,
                               "below_carrier_db": [one, two],
                               "difference_db": (None if one is None or two is None
                                                 else two - one)})
                break
    differences = [one["difference_db"] for one in shared if one["difference_db"] is not None]
    worst = None
    for value in differences:
        if worst is None or abs(value) > abs(worst):
            worst = value
    return {
        "kind": kind,
        "steps": [step_a, step_b],
        "differ": "%s -> %s" % (_pair_value(kind, a), _pair_value(kind, b)),
        "only_first": [run.first_mhz for run in only_a],
        "only_second": [run.first_mhz for run in only_b],
        "both": shared,
        "median_difference_db": (median(differences) if differences else None),
        "worst_difference_db": worst,
    }


def _below(reading, run):
    if run.peak_dbm is None or reading.carrier_dbm is None:
        return None
    return reading.carrier_dbm - run.peak_dbm


def _pair_value(kind, reading):
    if kind == "width":
        return "%.1f MHz" % reading.plan.seg_mhz
    if kind == "settle":
        return "%d us" % reading.plan.settle_us
    if kind == "regulator":
        return _word(reading.scope.ack_regulator)
    if kind == "gain":
        return _word(reading.scope.ack_gain)
    return "%.4f MHz" % reading.plan.first_mhz


def pairs_of(readings):
    """Every two scopes of a capture that differ in exactly one thing, in the order they were asked."""
    out = []
    for i in range(len(readings)):
        for j in range(i + 1, len(readings)):
            kind = one_difference(readings[i], readings[j])
            if kind is None:
                continue
            if kind == "first_point":
                out.append(first_point_pair(i + 1, readings[i], j + 1, readings[j]))
            else:
                out.append(one_thing_pair(kind, i + 1, readings[i], j + 1, readings[j]))
    return out


STEPS_HEADER = ("%4s %-17s %-7s %-5s %-4s %-6s %-7s %-5s %-8s %-7s %-4s %-6s %-11s %-7s "
                "%-13s %-13s %s")
STEPS_ROW = ("%4d %-17s %-7.1f %-5d %-4d %-6.1f %-7d %-5s %-8s %-7d %-4d %-6d %-11s %-7d "
             "%-13s %-13s %s")


def print_steps(path, readings, skipped, bad_frames, out=sys.stdout):
    """The table of a session's download, and under it what every pair of its scopes says."""
    rows = steps_rows(readings)
    out.write("%s\n" % path)
    out.write("  %d scope(s) in the order they were asked; %d line(s) that are not a `scope` "
              "frame, %d frame(s) that would not read\n" % (len(rows), skipped, bad_frames))
    out.write("\n")
    out.write(STEPS_HEADER % ("step", "span MHz", "rbw", "pts", "seg", "segw", "settle", "reg",
                              "gain", "frames", "lit", "quiet", "rule", "places", "us a point",
                              "1.6 | 0.94", "dev"))
    out.write("\n")
    for row in rows:
        out.write(STEPS_ROW % (
            row["step"], "%.3f-%.3f" % (row["f0"], row["f1"]), row["rbw_khz"], row["points"],
            row["segments"], row["seg_mhz"], row["settle_us"], _word(row["regulator"]),
            _word(row["gain"]), row["frames"], row["lit_frames"], row["quiet_frames"],
            _rule_said(row), len(row["places"]),
            _span_said(row["us_per_point_min"], row["us_per_point_max"]),
            "%.2f | %.2f" % (row["point_us_at_1_6"], row["point_us_at_0_94"]),
            "-" if row["device_errors"] is None else "%04x" % row["device_errors"]))
        out.write("\n")
    for row in rows:
        if not row["has_ack"]:
            # The width, the settle and the two words are the `ack`'s and are written nowhere
            # else, and a new scope begins at an ack or at a frame of a different shape - so two
            # steps of **one** span with no acks between them read as one scope. A download
            # carries every line the scanner sent, acks included; a capture that lost them is
            # worth saying out loud rather than reading wrong in silence.
            out.write("  step %d carries no `ack`: its segment width, its settle, its regulator "
                      "and its gain are unknown here and the default width was taken, and two "
                      "steps of one span would have been read as this one scope\n" % row["step"])
    for row in rows:
        for note in row["notes"]:
            out.write("  step %d: %s\n" % (row["step"], note))
        if row["device_errors"]:
            out.write("  step %d, dev %s\n" % (row["step"],
                                               row["device_errors_said"]))
    for row in rows:
        out.write("\n")
        out.write("step %d, %s: %s\n" % (row["step"], _asked_as(row), row["summary"]))
        if not row["places"]:
            continue
        out.write("  %-10s %-4s %-10s %-11s %-8s %-8s %s\n"
                  % ("first MHz", "seg", "from seg", "from carrier", "peak", "below", "quiet"))
        for place in row["places"]:
            out.write("  %-10.4f %-4d %-10.4f %-11s %-8s %-8s %s\n"
                      % (place["first_mhz"], place["segment"], place["from_segment_mhz"],
                         _number(place["from_carrier_mhz"], 4), _number(place["peak_dbm"], 1),
                         _number(place["below_carrier_db"], 1),
                         _number(place["quiet_over_floor_db"], 1)))
    print_pairs(pairs_of(readings), out=out)
    return rows


def _rule_said(row):
    if row["rule"] == "quiet":
        return "quiet(%d)" % row["quiet_frames"]
    return "floor"


def _asked_as(row):
    said = "%.3f to %.3f MHz, %d points, rbw %.1f kHz, %d segment(s)" % (
        row["f0"], row["f1"], row["points"], row["rbw_khz"], row["segments"])
    if row["seg_mhz"] != SEGMENT_MHZ:
        said += ", segw %.1f MHz" % row["seg_mhz"]
    said += ", settle %d us" % row["settle_us"]
    for name, value in (("reg", row["regulator"]), ("gain", row["gain"])):
        if value is not None:
            said += ", %s %s" % (name, value)
    return said


def _span_said(low, high):
    if low is None and high is None:
        return "-"
    if low is None or high is None:
        return _number(low if high is None else high)
    return "%.2f-%.2f" % (low, high)


def print_pairs(pairs, out=sys.stdout):
    out.write("\n")
    if not pairs:
        out.write("no two scopes of this capture differ in exactly one thing, so there is no "
                  "pair to read (the four lists of tools/noisescope/lists/ are written as such "
                  "pairs)\n")
        return
    out.write("the pairs of this capture - two scopes differing in exactly one thing:\n")
    for pair in pairs:
        out.write("\n")
        out.write("  %s, steps %d and %d (%s)\n"
                  % (PAIR_NAMES[pair["kind"]], pair["steps"][0], pair["steps"][1],
                     pair["differ"]))
        if pair["kind"] == "first_point":
            out.write("    %d echo(es) in step %d and %d in step %d; %s\n"
                      % (pair["echoes"][0], pair["steps"][0], pair["echoes"][1],
                         pair["steps"][1], pair["why"]))
            out.write("    **%s**\n" % pair["verdict"])
            continue
        out.write("    %d echo(es) only in step %d, %d only in step %d, %d in both\n"
                  % (len(pair["only_first"]), pair["steps"][0], len(pair["only_second"]),
                     pair["steps"][1], len(pair["both"])))
        if pair["median_difference_db"] is None:
            out.write("    no place both have carries a level against the carrier to compare\n")
        else:
            out.write("    of those in both, step %d stands %+.1f dB further below the carrier "
                      "than step %d (median; worst %+.1f)\n"
                      % (pair["steps"][1], pair["median_difference_db"], pair["steps"][0],
                         pair["worst_difference_db"]))


def steps_as_dict(path, readings, skipped, bad_frames, carrier_mhz, lit_dbm, over_db):
    return {
        "capture": path,
        "skipped_lines": skipped,
        "unreadable_frames": bad_frames,
        "carrier_mhz": carrier_mhz,
        "lit_dbm": lit_dbm,
        "over_db": over_db,
        "steps": steps_rows(readings),
        "pairs": pairs_of(readings),
    }


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
    parser.add_argument("--steps", action="store_true",
                        help="read the capture as a NoiseScope session: one table across its "
                             "scopes in the order they were asked, and what every two of them "
                             "that differ in exactly one thing say (docs/noisescope.md section 7)")
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
                             floor_dbm=args.floor, floor_quantile=args.floor_quantile,
                             quiet_rule=args.steps)
                for scope in scopes]
    if args.steps:
        if args.json:
            out.write(json.dumps(steps_as_dict(args.capture, readings, skipped, bad_frames,
                                               args.carrier, args.lit, args.over),
                                 indent=2, sort_keys=True) + "\n")
        else:
            print_steps(args.capture, readings, skipped, bad_frames, out=out)
        return 0
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
