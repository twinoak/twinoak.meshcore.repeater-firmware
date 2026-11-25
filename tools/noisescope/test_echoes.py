"""tools/noisescope/echoes.py, held to the firmware's arithmetic and to planted echoes.

Run where tools/bench's own tests are: the release step of tools/verify.ps1
(tools/release/selfcheck.py adds this module to its suite), because it needs no board, no
compiler and nothing but the standard library.

Four things:

  * **the plan** - every row of tools/noisescope/scope_plans.json, which is the table
    test/test_scanner/test_scanner.cpp reads for `planScope()` and `scopeSegment()` themselves.
    A plan that moves fails here and there at once (`Scope.ThePlanTableIsPlanScopes`);
  * **the planting** - captures this file makes, with echoes put in by one of three rules: every
    5.6 MHz from a segment's first point, every 12 points, every 41 us. Each capture holds four
    scopes - **two bandwidths, two steps each** - because two scopes of different step and
    bandwidth are the only thing that can tell the three rules apart (echoes.py's own docstring
    says why), and the test asserts the tool names the rule it was planted with;
  * **a capture with a carrier and no echoes**, which must say so rather than find something;
  * **what a capture has in it besides frames**: lines that are not `scope` lines, a frame with
    `255` in it, a `scope` of one segment, a frame that will not decode.

The captures are the firmware's own format (docs/noisescope.md section 5): the `ack` with its
`seg=`, then frames whose `f0` is the first point's centre to six decimals and whose `step` is
kHz to four, because that is how src/roles/scanner/Lines.cpp writes them and a tool that only
works on unrounded numbers would not work on a capture.
"""

import base64
import io
import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import echoes  # noqa: E402
import hunt  # noqa: E402  (its `read_list`, for this pile's own list of lines)


# ----------------------------------------------------------------------------- a capture, made

FLOOR_DBM = -110.0
CARRIER_DBM = -32.0
# How far over the floor a planted block's first point stands, and the ripple the floor itself is
# given so that a floor is not a row of identical numbers (a real one never is).
BLOCK_DB = 24.0
RIPPLE_DB = 1.5


def _byte(dbm):
    """A level as the frame carries it: -2 x dBm, one byte, `255` for no reading."""
    if dbm is None:
        return echoes.NO_READING
    value = int(round(-2.0 * dbm))
    return max(0, min(254, value))


def frame_line(plan, dbm, seq=1, sps=476, agg="last", sweeps=1):
    """One `scope` line, with the numbers rounded as src/roles/scanner/Lines.cpp rounds them."""
    raw = bytes(bytearray(_byte(one) for one in dbm))
    return json.dumps({
        "t": "scope", "seq": seq, "ts": 0, "ms": 1000 * seq,
        "f0": round(plan.first_mhz, 6), "step": round(plan.step_khz, 4),
        "rbw": plan.rbw_khz, "n": plan.points, "cov": round(plan.cov, 3),
        "agg": agg, "sweeps": sweeps, "sps": sps,
        "lv": base64.b64encode(raw).decode("ascii"),
    })


def ack_line(plan):
    """`scopeAckMessage()` (src/roles/scanner/Sweep.cpp): `segw=` only when the width is not the
    default, and `settle=` on **every** `scope` since phase 3.17's task 01 - every one of them has
    a settle now, and a capture has to say which."""
    msg = ("n=%d step=%.4fkHz rbw=%.1fkHz seg=%d cov=%.3f interval=0ms sweep=%dms"
           % (plan.points, plan.step_khz, plan.rbw_khz, plan.segments, plan.cov, 1))
    if plan.seg_mhz != echoes.SEGMENT_MHZ:
        msg += " segw=%.1fMHz" % plan.seg_mhz
    msg += " settle=%dus" % plan.settle_us
    return json.dumps({"t": "ack", "cmd": "scope", "ok": True, "msg": msg})


def old_ack_line(plan):
    """The ack of a firmware from before phase 3.17's task 01: no `settle=` at all.

    A capture downloaded from a unit running 0.1.8 or earlier is read every day, so the reader has
    to keep taking an ack that names no settle - and that is a `scope` that stood on nothing.
    """
    msg = ("n=%d step=%.4fkHz rbw=%.1fkHz seg=%d cov=%.3f interval=0ms sweep=%dms"
           % (plan.points, plan.step_khz, plan.rbw_khz, plan.segments, plan.cov, 1))
    return json.dumps({"t": "ack", "cmd": "scope", "ok": True, "msg": msg})


def plant(plan, rule, value, carrier_mhz=None, no_reading=()):
    """One sweep's levels: the floor, the carrier, and a block every `value` of `rule`.

    `rule` is "mhz", "points" or "us" and `value` is the interval in that unit. The interval is
    turned into points at *this* plan's step and bandwidth, which is the whole trick: one rule
    gives a different number of points in two scopes of different steps, and that is what
    echoes.py has to read back.

    A block begins at its point and falls back to the floor over about half the interval, in the
    sweep's own direction - the shape of 23 November (tasks/phase-3.16/README.md: *each block
    fades over five to fifteen points in the sweep's direction*). Half, and not all of it, so
    that a tenth of each segment is still floor and the floor can be found at all.
    """
    if rule == "points":
        interval = int(round(value))
    elif rule == "mhz":
        interval = int(round(value / plan.step_mhz))
    elif rule == "us":
        interval = int(round(value / plan.point_us))
    else:
        raise AssertionError(rule)
    assert interval >= 2, "an interval of %r %s is %d points here" % (value, rule, interval)

    levels = []
    for i in range(plan.points):
        # A floor that ripples, so that a quantile of it is not one repeated number.
        levels.append(FLOOR_DBM + RIPPLE_DB * ((i % 3) - 1))

    tail = max(1, interval // 2)
    for seg in echoes.segments_of(plan):
        # Three blocks in a segment is the fewest that say an interval: with two, the distances
        # are 0 and d and d/2 explains them as well as d does, which is what the narrow capture of
        # 23 November ran into (its middle mark lay under the carrier's own skirt). A plan that
        # cannot hold three is not a plan to plant this rule in.
        assert seg.points >= 3 * interval, (
            "segment %d of this plan holds %d points and an interval of %r %s is %d of them: "
            "fewer than three blocks say nothing" % (seg.index, seg.points, value, rule, interval))
        at = seg.first
        while at <= seg.last:
            for k in range(tail + 1):
                i = at + k
                if i > seg.last:
                    break
                over = BLOCK_DB * (1.0 - float(k) / float(tail + 1))
                levels[i] = max(levels[i], FLOOR_DBM + over)
            at += interval

    if carrier_mhz is not None:
        index = int(round((carrier_mhz - plan.first_mhz) / plan.step_mhz))
        if 0 <= index < plan.points:
            levels[index] = CARRIER_DBM
    for index in no_reading:
        levels[index] = None
    return levels


def capture(plans, rule=None, value=None, carrier_mhz=869.618, frames=3, chatter=True,
            no_reading=()):
    """A capture of one scope per plan, each planted by the same rule. A list of lines."""
    lines = []
    if chatter:
        # What a real capture has in it besides frames, and what tools/bench writes into one.
        lines.append("# bench console --scanner, 10 s")
        lines.append(json.dumps({"t": "boot", "fw": "noisescope", "ver": "0.1.7",
                                 "role": "scanner", "board": "rak4630-single", "reset": "soft",
                                 "wdt": "", "radio_ok": True, "rtc": 1, "ina": 1, "ts": 0}))
    seq = 0
    for plan in plans:
        lines.append(ack_line(plan))
        if rule is None:
            levels = [FLOOR_DBM + RIPPLE_DB * ((i % 3) - 1) for i in range(plan.points)]
            index = int(round((carrier_mhz - plan.first_mhz) / plan.step_mhz))
            if 0 <= index < plan.points:
                levels[index] = CARRIER_DBM
            for one in no_reading:
                levels[one] = None
        else:
            levels = plant(plan, rule, value, carrier_mhz=carrier_mhz, no_reading=no_reading)
        for _frame in range(frames):
            seq += 1
            lines.append(frame_line(plan, levels, seq=seq))
        if chatter:
            lines.append(json.dumps({"t": "stat", "ts": 0, "ms": 1000 * seq, "up": 1,
                                     "mode": "scope", "home": 869.618}))
    return lines


# The four scopes a planted capture holds: **two bandwidths, two steps each**, which is what the
# task asks for and what it takes to name a rule. All four are the wide span of 23 November
# (`scope 779.618 959.618`, the carrier's own frequency in the middle of it), because the two
# bandwidths have to be wide ones: 41 us is twelve points at 467 kHz but a point and a half at
# 58.6, and a rule whose interval is one point is not a rule anything can read back.
#
#   name              points   step kHz   rbw kHz   segments   12 points is
WIDE_467 = dict(f0=779.618, f1=959.618, points=0, rbw_khz=467.0)      # 386, 466.32, 467, 6
WIDE_234 = dict(f0=779.618, f1=959.618, points=0, rbw_khz=234.3)      # 769, 234.07, 234.3, 6
WIDE_467_FINE = dict(f0=779.618, f1=959.618, points=772, rbw_khz=467.0)   # 772, 233.16, 467, 6
WIDE_234_FINE = dict(f0=779.618, f1=959.618, points=1538, rbw_khz=234.3)  # 1538, 117.04, 234.3, 7
ALL_FOUR = (WIDE_467, WIDE_234, WIDE_467_FINE, WIDE_234_FINE)
# And the narrow one, for a `scope` of a single segment: thirteen megahertz around the carrier, as
# near as 23 November's second capture is known (tools/noisescope/scope_plans.json).
NARROW = dict(f0=863.1, f1=876.1, points=250, rbw_khz=0.0)            # 250, 52.00, 58.6, 1


def plan_of(spec):
    """The plan a spec asks for. `seg_mhz` and `settle_us` are phase 3.16's task 02's and are
    optional, so that a spec written before it is a spec still."""
    plan = echoes.plan_scope(spec["f0"], spec["f1"], points=spec["points"],
                             rbw_khz=spec["rbw_khz"], seg_mhz=spec.get("seg_mhz", 0.0),
                             settle_us=spec.get("settle_us", echoes.SETTLE_ABSENT_US))
    assert plan is not None, spec
    return plan


def read(lines, **kwargs):
    """The readings of a capture given as lines, and the verdict over them."""
    scopes, skipped, bad = echoes.read_capture(lines)
    scopes = [scope for scope in scopes if scope.frames]
    readings = [echoes.ScopeReading(scope, **kwargs) for scope in scopes]
    return readings, echoes.verdict(readings), skipped, bad


# ----------------------------------------------------------------------------- the plan

class ThePlanTable(unittest.TestCase):
    """tools/noisescope/scope_plans.json, which the firmware's own test reads too."""

    def test_the_table_is_this_tools_arithmetic_for_every_plan(self):
        out = io.StringIO()
        self.assertTrue(echoes.check_plans(out=out), out.getvalue())

    def test_every_scope_this_file_plants_in_is_a_row_of_the_table(self):
        """A capture this test makes is a span a unit was really asked for, and not an invention."""
        with open(echoes.PLAN_TABLE, "r", encoding="utf-8") as handle:
            table = json.load(handle)
        rows = [(one["request"]["f0"], one["request"]["f1"], one["request"]["points"],
                 one["request"]["rbw_khz"]) for one in table["plans"]]
        for spec in ALL_FOUR + (NARROW,):
            key = (spec["f0"], spec["f1"], spec["points"], spec["rbw_khz"])
            self.assertIn(key, rows, "%r is in no row of %s"
                          % (key, os.path.basename(echoes.PLAN_TABLE)))

    def test_the_table_has_the_spans_this_pile_is_about(self):
        with open(echoes.PLAN_TABLE, "r", encoding="utf-8") as handle:
            table = json.load(handle)
        self.assertGreaterEqual(len(table["plans"]), 10)
        # The wide scope of 23 November: six segments, 386 points, and the segment starts the
        # pile's README worked out by hand (779.9, 810.2, 840.5, 870.3, 900.2, 930.0 MHz).
        wide = [one for one in table["plans"] if "23 November" in one["name"]
                and one["request"]["points"] == 0 and one["request"]["rbw_khz"] == 467.0
                and not one["request"].get("seg_mhz", 0.0)]
        self.assertEqual(1, len(wide))
        plan, segments = wide[0]["plan"], wide[0]["segments"]
        self.assertEqual(386, plan["points"])
        self.assertEqual(6, plan["segments"])
        starts = [round(one["first_mhz"], 1) for one in segments]
        self.assertEqual([779.9, 810.2, 840.5, 870.3, 900.2, 930.0], starts)
        # And twelve points of it is the 5.6 MHz the blocks were read to repeat at.
        self.assertAlmostEqual(5.596, 12 * plan["step_khz"] / 1000.0, places=2)

    def test_a_frames_rounded_numbers_still_give_its_own_plan(self):
        """A frame carries six decimals of MHz and four of kHz, not the unrounded plan."""
        for spec in ALL_FOUR + (NARROW,):
            plan = plan_of(spec)
            line = json.loads(frame_line(plan, [FLOOR_DBM] * plan.points))
            back = echoes.plan_of_frame(line["f0"], line["step"], line["n"], line["rbw"])
            self.assertIsNotNone(back, spec)
            self.assertEqual(plan.points, back.points, spec)
            self.assertEqual(plan.segments, back.segments, spec)
            self.assertAlmostEqual(plan.step_khz, back.step_khz, places=3)
            self.assertAlmostEqual(plan.first_mhz, back.first_mhz, places=5)
            for mine, theirs in zip(echoes.segments_of(plan), echoes.segments_of(back)):
                self.assertEqual(mine.first, theirs.first, spec)
                self.assertEqual(mine.points, theirs.points, spec)


# ----------------------------------------------------------------------------- the three rules

class TheRuleIsNamed(unittest.TestCase):
    """An echo planted by one of three rules, and the tool asked which rule it was.

    Each capture holds the four scopes of ALL_FOUR - two bandwidths, two steps each - so that one
    unit's interval holds across them and the other two do not. A pair would do for one rule; all
    four is the task's *at two bandwidths and two steps each*, and it leaves no pair of scopes in
    which two units happen to agree.
    """

    def _planted(self, rule, value, specs=ALL_FOUR):
        readings, (unit, line), _skipped, _bad = read(
            capture([plan_of(one) for one in specs], rule=rule, value=value),
            carrier_mhz=869.618)
        self.assertEqual(len(specs), len(readings), line)
        for reading in readings:
            self.assertTrue(reading.echoes, "no echo found in %s: %s"
                            % (reading.plan.as_dict(), reading.summary()))
            self.assertTrue(reading.interval["points"], reading.summary())
        return unit, line, readings

    def _intervals(self, readings, unit):
        return [reading.interval[unit][0] for reading in readings]

    def test_every_5_6_mhz_from_a_segments_first_point_is_read_as_mhz(self):
        unit, line, readings = self._planted("mhz", 5.596)
        self.assertEqual("mhz", unit, line)
        for value in self._intervals(readings, "mhz"):
            self.assertAlmostEqual(5.596, value, delta=0.03)
        # The same 5.6 MHz, and *not* the same number of points: twelve of the 467 kHz scope's,
        # twenty-four of each halved step's, forty-eight of the finest.
        self.assertEqual([12.0, 24.0, 24.0, 48.0], self._intervals(readings, "points"))

    def test_every_12_points_from_a_segments_first_point_is_read_as_points(self):
        unit, line, readings = self._planted("points", 12)
        self.assertEqual("points", unit, line)
        self.assertEqual([12.0] * 4, self._intervals(readings, "points"))
        # And the MHz interval is not the same in the four: 5.60 down to 1.40.
        mhz = [round(value, 2) for value in self._intervals(readings, "mhz")]
        self.assertEqual([5.6, 2.81, 2.8, 1.4], mhz)

    def test_every_41_us_from_a_segments_first_point_is_read_as_us(self):
        unit, line, readings = self._planted("us", 41.1)
        self.assertEqual("us", unit, line)
        for value in self._intervals(readings, "us"):
            self.assertAlmostEqual(41.1, value, delta=0.5)
        # Twelve points at 467 kHz and six at 234.3: the same time, not the same points.
        self.assertEqual([12.0, 6.0, 12.0, 6.0], self._intervals(readings, "points"))

    def test_each_rule_is_named_from_two_scopes_as_well_as_four(self):
        """One bandwidth each and one step each is already enough to tell the three apart."""
        for rule, value in (("mhz", 5.596), ("points", 12), ("us", 41.1)):
            unit, line, _readings = self._planted(rule, value, (WIDE_467, WIDE_234))
            self.assertEqual(rule, unit, line)
            unit, line, _readings = self._planted(rule, value, (WIDE_467_FINE, WIDE_234_FINE))
            self.assertEqual(rule, unit, line)

    def test_one_scope_cannot_tell_the_three_apart_and_says_so(self):
        """The honest answer to a capture of one span: a point is one number in all three units."""
        readings, (unit, line), _skipped, _bad = read(
            capture([plan_of(WIDE_467)], rule="points", value=12), carrier_mhz=869.618)
        self.assertEqual(1, len(readings))
        self.assertTrue(readings[0].echoes)
        self.assertIsNone(unit)
        self.assertIn("one step and one bandwidth", line)
        self.assertIn("cannot tell them apart", readings[0].summary())

    def test_two_scopes_of_one_step_and_bandwidth_cannot_either(self):
        readings, (unit, line), _skipped, _bad = read(
            capture([plan_of(WIDE_467), plan_of(WIDE_467)], rule="points", value=12),
            carrier_mhz=869.618)
        self.assertEqual(2, len(readings))
        self.assertIsNone(unit)
        self.assertIn("one step and one bandwidth", line)

    def test_the_distances_are_from_the_segments_own_first_point(self):
        readings, _verdict, _skipped, _bad = read(
            capture([plan_of(WIDE_467)], rule="points", value=12), carrier_mhz=869.618)
        reading = readings[0]
        self.assertEqual(6, reading.plan.segments)
        seen = set()
        for run in reading.echoes:
            self.assertEqual(0, run.distance_points % 12,
                             "%.4f MHz is %d points into segment %d"
                             % (run.first_mhz, run.distance_points, run.segment.index))
            self.assertAlmostEqual(run.distance_points * reading.plan.step_mhz, run.distance_mhz)
            self.assertAlmostEqual(run.distance_points * reading.plan.point_us, run.distance_us)
            self.assertGreaterEqual(run.first, run.segment.first)
            self.assertLessEqual(run.last, run.segment.last)
            seen.add(run.segment.index)
        self.assertEqual(set(range(6)), seen, "every segment has its own blocks")
        # A block at a segment's own first point is one of them, which is what was seen at
        # Bellevue in three of the six segments.
        self.assertIn(0, [run.distance_points for run in reading.echoes])

    def test_a_tail_is_measured_in_the_same_three_units(self):
        readings, _verdict, _skipped, _bad = read(
            capture([plan_of(WIDE_467)], rule="points", value=12), carrier_mhz=869.618)
        reading = readings[0]
        for run in reading.echoes:
            self.assertGreaterEqual(run.points, 1)
            self.assertLess(run.points, 12, "a block cannot be longer than the interval here")
            self.assertAlmostEqual(run.points * reading.plan.step_mhz, run.length_mhz)
            self.assertAlmostEqual(run.points * reading.plan.point_us, run.length_us)

    def test_an_echo_is_said_to_be_under_the_carrier(self):
        readings, _verdict, _skipped, _bad = read(
            capture([plan_of(WIDE_467)], rule="points", value=12), carrier_mhz=869.618)
        reading = readings[0]
        self.assertAlmostEqual(CARRIER_DBM, reading.carrier_dbm, places=1)
        for run in reading.echoes:
            under = reading.carrier_dbm - run.peak_dbm
            self.assertGreater(under, 0.0)
            self.assertAlmostEqual(under, run.as_dict(reading.carrier_dbm)["below_carrier_db"])


# ----------------------------------------------------------------------------- no echo

class NoEcho(unittest.TestCase):

    def test_a_carrier_and_no_echoes_says_so(self):
        readings, (unit, line), _skipped, _bad = read(
            capture([plan_of(WIDE_467), plan_of(NARROW)], rule=None), carrier_mhz=869.618)
        self.assertEqual(2, len(readings))
        for reading in readings:
            self.assertEqual([], reading.echoes, reading.summary())
            self.assertTrue(reading.summary().startswith("no echo"), reading.summary())
            self.assertGreater(reading.lit, 0, "the carrier is there in every frame")
        self.assertIsNone(unit)
        self.assertIn("nothing to follow", line)

    def test_peaks_that_do_not_repeat_are_not_called_an_interval(self):
        """The nine spurs of a real dummy-load baseline, as distances from a segment's start.

        `scope 150 960 2048` on a 50 ohm load (the bench of 22 October, `out/bench/`) has nine
        peaks of the radio's own, 15 to 78 points into whichever segment they fall in. Before
        MIN_MULTIPLE_COVERAGE the tool called them *every one and a half points* - which fits
        them, on the tenth, fourteenth, seventeenth, thirtieth, thirty-third, forty-first and
        fifty-second multiple of itself, and says nothing at all.
        """
        plan = plan_of(WIDE_467)
        levels = [FLOOR_DBM + RIPPLE_DB * ((i % 3) - 1) for i in range(plan.points)]
        seg = echoes.segments_of(plan)[0]
        for into in (15, 21, 25, 32, 33, 45, 49, 61, 78):
            if seg.first + into <= seg.last:
                levels[seg.first + into] = FLOOR_DBM + BLOCK_DB
        index = int(round((869.618 - plan.first_mhz) / plan.step_mhz))
        levels[index] = CARRIER_DBM
        lines = [ack_line(plan)] + [frame_line(plan, levels, seq=i) for i in range(1, 4)]
        readings, (unit, line), _skipped, _bad = read(lines, carrier_mhz=869.618)
        reading = readings[0]
        self.assertGreaterEqual(len(reading.echoes), 6, reading.summary())
        for one in echoes.UNITS:
            self.assertIsNone(reading.interval[one], "%s: %s" % (one, reading.summary()))
        self.assertIn("do not repeat", reading.summary())
        self.assertIsNone(unit)
        self.assertIn("nothing to follow", line)

    def test_a_capture_whose_carrier_never_lights_reads_nothing_and_says_why(self):
        lines = capture([plan_of(WIDE_467)], rule="points", value=12, carrier_mhz=None)
        readings, _verdict, _skipped, _bad = read(lines, carrier_mhz=869.618, lit_dbm=-40.0)
        self.assertEqual([], readings[0].echoes)
        self.assertIn("holds the carrier", readings[0].summary())


# ----------------------------------------------------------------------------- what else is in
#                                                                               a capture

class WhatElseIsInACapture(unittest.TestCase):

    def test_lines_that_are_not_scope_lines_are_skipped_without_complaint(self):
        lines = capture([plan_of(WIDE_467)], rule="points", value=12)
        lines.insert(0, "not JSON at all")
        lines.insert(1, "{broken json")
        lines.insert(2, json.dumps({"t": "err", "msg": "something went wrong", "code": -707}))
        lines.insert(3, json.dumps(["a JSON array, which is not an object"]))
        readings, _verdict, skipped, bad = read(lines, carrier_mhz=869.618)
        self.assertEqual(1, len(readings))
        self.assertTrue(readings[0].echoes)
        self.assertGreaterEqual(skipped, 4)
        self.assertEqual(0, bad)

    def test_a_frame_with_255_in_it_is_read(self):
        plan = plan_of(WIDE_467)
        blind = (0, 1, 2, plan.points - 1)
        lines = capture([plan], rule="points", value=12, no_reading=blind)
        readings, _verdict, _skipped, bad = read(lines, carrier_mhz=869.618)
        self.assertEqual(0, bad)
        reading = readings[0]
        self.assertTrue(reading.echoes)
        for run in reading.echoes:
            for index in blind:
                self.assertFalse(run.first <= index <= run.last,
                                 "point %d has no reading and cannot be over anything" % index)

    def test_a_scope_of_one_segment_is_read(self):
        readings, _verdict, _skipped, _bad = read(
            capture([plan_of(NARROW)], rule="points", value=12), carrier_mhz=869.618)
        self.assertEqual(1, readings[0].plan.segments)
        self.assertTrue(readings[0].echoes)
        for run in readings[0].echoes:
            self.assertEqual(0, run.segment.index)

    def test_a_frame_that_will_not_decode_is_counted_and_the_rest_is_read(self):
        plan = plan_of(WIDE_467)
        lines = capture([plan], rule="points", value=12)
        lines.append(json.dumps({"t": "scope", "seq": 99, "f0": round(plan.first_mhz, 6),
                                 "step": round(plan.step_khz, 4), "rbw": plan.rbw_khz,
                                 "n": plan.points, "lv": "not base64 at all!!"}))
        readings, _verdict, _skipped, bad = read(lines, carrier_mhz=869.618)
        self.assertEqual(1, bad)
        self.assertEqual(1, len(readings))
        self.assertTrue(readings[0].echoes)

    def test_the_ack_and_the_frames_agree_on_how_many_segments_there_are(self):
        for spec in ALL_FOUR + (NARROW,):
            plan = plan_of(spec)
            scopes, _skipped, _bad = echoes.read_capture(capture([plan], rule="points", value=12))
            self.assertEqual(1, len(scopes))
            self.assertEqual(plan.segments, scopes[0].ack_segments, spec)
            self.assertEqual(plan.segments, scopes[0].plan.segments, spec)

    def test_two_scopes_in_one_capture_with_no_ack_between_them_are_still_two(self):
        lines = [line for line in capture([plan_of(WIDE_467), plan_of(NARROW)], rule="points",
                                          value=12, chatter=False)
                 if '"t": "ack"' not in line]
        scopes, _skipped, _bad = echoes.read_capture(lines)
        self.assertEqual(2, len(scopes))
        self.assertEqual([386, 250], [scope.plan.points for scope in scopes])
        self.assertEqual([None, None], [scope.ack_segments for scope in scopes])

    def test_what_a_sweep_cost_and_where_the_carrier_landed_are_reported(self):
        readings, _verdict, _skipped, _bad = read(
            capture([plan_of(WIDE_467)], rule="points", value=12), carrier_mhz=869.618)
        reading = readings[0]
        self.assertAlmostEqual(47.6, reading.scope.sps, places=1)
        self.assertEqual(0, reading.carrier_offset_points,
                         "the carrier was planted on its own point")
        as_json = reading.as_dict()
        self.assertEqual(47.6, as_json["sps"])
        self.assertEqual(0, as_json["carrier"]["offset_points"])
        self.assertEqual("last", as_json["agg"])


# ----------------------------------------------------------------------------- the command line

class TheCommandLine(unittest.TestCase):

    def _file(self, lines, name="capture.ndjson"):
        import tempfile
        directory = tempfile.mkdtemp(prefix="echoes-test-")
        path = os.path.join(directory, name)
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("".join(line + "\n" for line in lines))
        self.addCleanup(_rmtree, directory)
        return path

    def test_a_capture_with_echoes_exits_0_and_prints_a_table(self):
        path = self._file(capture([plan_of(WIDE_467), plan_of(WIDE_234)], rule="mhz", value=5.596))
        out = io.StringIO()
        self.assertEqual(0, echoes.main([path, "--carrier", "869.618"], out=out))
        text = out.getvalue()
        self.assertIn("scope 1 of 2", text)
        self.assertIn("follow the frequency", text)
        self.assertIn("first MHz", text)

    def test_a_capture_with_no_echoes_exits_0_too(self):
        path = self._file(capture([plan_of(WIDE_467)], rule=None))
        out = io.StringIO()
        self.assertEqual(0, echoes.main([path, "--carrier", "869.618"], out=out))
        self.assertIn("no echo", out.getvalue())

    def test_the_same_as_json(self):
        path = self._file(capture([plan_of(WIDE_467), plan_of(WIDE_234)], rule="points", value=12))
        out = io.StringIO()
        self.assertEqual(0, echoes.main([path, "--carrier", "869.618", "--json"], out=out))
        parsed = json.loads(out.getvalue())
        self.assertEqual("points", parsed["follows"])
        self.assertEqual(2, len(parsed["scopes"]))
        first = parsed["scopes"][0]
        self.assertEqual(386, first["plan"]["points"])
        self.assertEqual(6, len(first["segments"]))
        self.assertTrue(first["echoes"])
        for key in ("distance_mhz", "distance_points", "distance_us", "peak_dbm",
                    "below_carrier_db", "length_points", "length_mhz", "length_us", "segment"):
            self.assertIn(key, first["echoes"][0])

    def test_a_capture_it_cannot_read_is_the_error(self):
        out = io.StringIO()
        self.assertEqual(1, echoes.main([os.path.join(HERE, "no-such-capture.ndjson")], out=out))
        self.assertIn("no such file", out.getvalue())

        empty = self._file(["# nothing but chatter", "not JSON"])
        out = io.StringIO()
        self.assertEqual(1, echoes.main([empty], out=out))
        self.assertIn("no `scope` frame", out.getvalue())

    def test_check_plans_from_the_command_line(self):
        out = io.StringIO()
        self.assertEqual(0, echoes.main(["--check-plans"], out=out))
        self.assertIn("0 disagreeing", out.getvalue())

    def test_a_carrier_outside_the_span_is_said_and_the_strongest_point_taken(self):
        path = self._file(capture([plan_of(NARROW)], rule="points", value=12))
        out = io.StringIO()
        self.assertEqual(0, echoes.main([path, "--carrier", "433.0"], out=out))
        self.assertIn("outside this scope's span", out.getvalue())


# ----------------------------------------------------------------------------- the capture of
#                                                                               23 November

BELLEVUE = os.path.join(HERE, "captures", "bellevue-2026-11-23.ndjson")


class TheCaptureOf23November(unittest.TestCase):
    """The lines of meshmaster.dk `/noisescope/3`, when Thomas has saved them there.

    The pile's README worked its third `scope` out by hand: six segments beginning at 779.9,
    810.2, 840.5, 870.3, 900.2 and 930.0 MHz, blocks at a segment's first point and at points 12,
    24, 36 and 48 of one, strongest -47 to -70 dBm. With the file here this holds the tool to
    that; without it there is nothing to hold and the task's report says so (deliverable 3).
    """

    @unittest.skipUnless(os.path.isfile(BELLEVUE), "%s is not in this checkout" % BELLEVUE)
    def test_the_wide_scope_has_the_readmes_rows(self):
        scopes, _skipped, _bad = echoes.read_capture(BELLEVUE)
        wide = [scope for scope in scopes if scope.plan and scope.plan.points == 386
                and scope.plan.segments == 6]
        self.assertTrue(wide, "no 386-point six-segment `scope` in %s" % BELLEVUE)
        reading = echoes.ScopeReading(wide[0], carrier_mhz=869.618)
        starts = [round(seg.first_mhz, 1) for seg in reading.scope.segments]
        self.assertEqual([779.9, 810.2, 840.5, 870.3, 900.2, 930.0], starts)
        self.assertTrue(reading.echoes, reading.summary())
        distances = sorted(set(run.distance_points for run in reading.echoes))
        for expected in (0, 12, 24, 36, 48):
            self.assertIn(expected, distances,
                          "the README read a block %d points into a segment; this found %r"
                          % (expected, distances))
        self.assertTrue(reading.interval["points"], reading.summary())
        self.assertAlmostEqual(12.0, reading.interval["points"][0], delta=0.5)


# ----------------------------------------------------------------------------- the two numbers

class TheTwoNumbers(unittest.TestCase):
    """A segment's width and a settle (phase 3.16's task 02).

    The width is the one thing of a plan a frame cannot say - `f0`, `step`, `n` and `rbw` are all
    a frame carries - so a capture of a `scope` asked for with one has to be read **with its
    ack**, and getting it wrong puts every echo in the wrong segment. These are that.
    """

    WIDE = dict(f0=779.618, f1=959.618, points=0, rbw_khz=467.0)

    def _plan(self, seg_mhz=0.0, settle_us=echoes.SETTLE_ABSENT_US, **spec):
        full = dict(self.WIDE)
        full.update(spec)
        plan = echoes.plan_scope(full["f0"], full["f1"], points=full["points"],
                                 rbw_khz=full["rbw_khz"], seg_mhz=seg_mhz, settle_us=settle_us)
        self.assertIsNotNone(plan, (full, seg_mhz, settle_us))
        return plan

    def test_a_width_cuts_the_plan_as_the_firmwares_own_test_says_it_does(self):
        """The widths of deliverable 5, over both spans of 23 November."""
        wants = [
            (0.0, 6, 65), (32.0, 6, 65), (11.0, 17, 23), (5.6, 33, 12), (5.5, 36, 11),
            (2.0, 97, 4),
        ]
        for seg_mhz, segments, longest in wants:
            plan = self._plan(seg_mhz=seg_mhz)
            self.assertEqual(segments, plan.segments, seg_mhz)
            self.assertEqual(32.0 if seg_mhz == 0.0 else seg_mhz, plan.seg_mhz)
            segs = echoes.segments_of(plan)
            self.assertEqual(plan.points, sum(one.points for one in segs), seg_mhz)
            self.assertEqual(longest, max(one.points for one in segs), seg_mhz)
            for one in segs:
                self.assertLessEqual(one.cal_hi_mhz - one.cal_lo_mhz, plan.seg_mhz + 1e-9)
        narrow = dict(f0=863.118, f1=876.118, points=250, rbw_khz=58.6)
        for seg_mhz, segments in ((0.0, 1), (32.0, 1), (11.0, 2), (5.5, 3), (2.0, 7)):
            plan = self._plan(seg_mhz=seg_mhz, **narrow)
            self.assertEqual(segments, plan.segments, seg_mhz)

    def test_a_width_below_one_step_or_above_32_and_a_settle_too_long_are_refused(self):
        """planScope()'s own bounds: the tool must not read a plan the firmware would refuse."""
        self.assertIsNotNone(echoes.plan_scope(779.618, 959.618, 0, 467.0, seg_mhz=0.5))
        for bad in (0.4, 32.1, -1.0):
            self.assertIsNone(echoes.plan_scope(779.618, 959.618, 0, 467.0, seg_mhz=bad), bad)
        # A tenth of a MHz is taken, as the firmware takes it.
        self.assertEqual(5.6, echoes.plan_scope(779.618, 959.618, 0, 467.0, seg_mhz=5.64).seg_mhz)
        self.assertIsNotNone(echoes.plan_scope(779.618, 959.618, 0, 467.0,
                                               settle_us=echoes.SETTLE_MAX_US))
        self.assertIsNone(echoes.plan_scope(779.618, 959.618, 0, 467.0,
                                           settle_us=echoes.SETTLE_MAX_US + 1))

    def test_the_ack_is_what_says_which_width_a_capture_was_taken_with(self):
        """`segw=` and `settle=` of the ack: no `segw=` when the width is the default, and a
        `settle=` on every `scope` (phase 3.17's task 01)."""
        plan = self._plan(seg_mhz=5.6, settle_us=500)
        ack = json.loads(ack_line(plan))
        self.assertEqual(5.6, echoes.ack_seg_mhz(ack))
        self.assertEqual(500, echoes.ack_settle_us(ack))
        self.assertEqual(33, echoes.ack_segments(ack))
        today = json.loads(ack_line(self._plan()))
        self.assertIsNone(echoes.ack_seg_mhz(today), "a line asked for as it always was")
        self.assertEqual(echoes.SETTLE_DEFAULT_US, echoes.ack_settle_us(today),
                         "and it stood 500 us on each segment's first point all the same")
        self.assertEqual(6, echoes.ack_segments(today))
        # A line that asked for none says so, which is the whole reason the ack names it always.
        none = json.loads(ack_line(self._plan(settle_us=0)))
        self.assertEqual(0, echoes.ack_settle_us(none))
        # And an ack from a firmware older than this task names none, which is also none.
        old = json.loads(old_ack_line(self._plan()))
        self.assertIsNone(echoes.ack_settle_us(old))

    def test_a_line_with_no_ninth_number_is_planned_with_the_default_and_a_zero_is_none(self):
        """`plan_scope()`'s half of kScopeSettleDefaultUs, and `parse_scope()`'s."""
        self.assertEqual(echoes.SETTLE_DEFAULT_US, self._plan().settle_us)
        self.assertEqual(echoes.SETTLE_DEFAULT_US, self._plan(settle_us=echoes.SETTLE_ABSENT_US)
                         .settle_us)
        self.assertEqual(0, self._plan(settle_us=0).settle_us)
        self.assertEqual(2000, self._plan(settle_us=2000).settle_us)
        # The line, as a session types it: no ninth number, and then `max 32 0`.
        default = echoes.parse_scope("scope 840 900 0 467 1000")
        self.assertEqual(echoes.SETTLE_ABSENT_US, default.settle_us)
        self.assertEqual(echoes.SETTLE_DEFAULT_US, default.plan.settle_us)
        asked_none = echoes.parse_scope("scope 840 900 0 467 1000 max 32 0")
        self.assertEqual(0, asked_none.settle_us)
        self.assertEqual(0, asked_none.plan.settle_us)
        self.assertEqual(32.0, asked_none.plan.seg_mhz)
        # And the two are the same plan but for that: the row of scope_plans.json this pairs with.
        self.assertEqual(default.plan.points, asked_none.plan.points)
        self.assertEqual(default.plan.segments, asked_none.plan.segments)

    def test_a_capture_of_a_narrow_width_is_read_with_its_own_segments(self):
        """The whole point: seventeen segments of 23 points, not six of 65.

        11 MHz and not the 5.6 MHz the pile's README points at, because a planted capture needs
        three blocks in a segment to say anything and a twelve-point segment does not hold three;
        the firmware's own test is where every width is checked.
        """
        plan = self._plan(seg_mhz=11.0, settle_us=500)
        self.assertEqual(17, plan.segments)
        lines = capture([plan], rule="points", value=6)
        readings, (unit, _line), _skipped, bad = read(lines)
        self.assertEqual(0, bad)
        self.assertEqual(1, len(readings))
        reading = readings[0]
        self.assertEqual(17, reading.plan.segments)
        self.assertEqual(11.0, reading.plan.seg_mhz)
        self.assertEqual(500, reading.plan.settle_us)
        # A block every six points from each segment's first, and a segment is 22 or 23 points
        # here: so every echo is 0, 6, 12 or 18 points into its own segment and none is further.
        self.assertTrue(reading.echoes)
        for one in reading.echoes:
            self.assertIn(one.distance_points, (0, 6, 12, 18), one.as_dict(None))
        self.assertIn(6.0, reading.interval["points"], "the interval is six points of this plan")
        self.assertIsNone(unit, "one scope still cannot say which of the three units it keeps")

    def test_the_same_capture_read_without_its_ack_is_read_wrong_and_that_is_the_reason(self):
        """Why the ack is read at all: the segments are the one thing a frame cannot say."""
        plan = self._plan(seg_mhz=11.0)
        lines = [one for one in capture([plan], rule="points", value=6)
                 if '"t": "ack"' not in one and '"t":"ack"' not in one]
        readings, _verdict, _skipped, _bad = read(lines)
        self.assertEqual(1, len(readings))
        self.assertEqual(6, readings[0].plan.segments,
                         "with no ack a capture is read as a `scope` asked for without a width")
        far = [one.distance_points for one in readings[0].echoes if one.distance_points >= 23]
        self.assertTrue(far, "and the distances then run the length of a 65-point segment")

    def test_a_settle_changes_no_distance_inside_a_segment(self):
        """It is time spent before a segment's first reading, so no echo of one moves."""
        planted = {}
        for settle in (0, 500, echoes.SETTLE_MAX_US):
            plan = self._plan(seg_mhz=11.0, settle_us=settle)
            readings, _verdict, _skipped, _bad = read(capture([plan], rule="points", value=6))
            self.assertEqual(1, len(readings))
            planted[settle] = sorted(one.distance_points for one in readings[0].echoes)
            self.assertEqual(settle, readings[0].plan.settle_us)
        self.assertEqual(planted[0], planted[500])
        self.assertEqual(planted[0], planted[echoes.SETTLE_MAX_US])

    def test_every_scope_line_of_this_pile_s_list_is_one_this_firmware_runs(self):
        """tools/noisescope/lists/p3.16-02.txt, read by hunt.py's own reader."""
        path = os.path.join(os.path.dirname(os.path.abspath(echoes.__file__)), "lists",
                            "p3.16-02.txt")
        self.assertTrue(os.path.isfile(path), path)
        rows = hunt.read_list(path)
        self.assertEqual(32, len(rows), "two spans, sixteen lines each")
        for name, line, _about in rows:
            words = line.split()
            self.assertEqual("scope", words[0], name)
            self.assertLessEqual(len(words), 9, "%s: nine words is the longest line there is" % name)
            self.assertEqual("last", words[6], "%s: a frame is one sweep" % name)
            self.assertEqual("0", words[5], "%s: a frame per sweep" % name)
            plan = echoes.plan_scope(float(words[1]), float(words[2]), points=int(words[3]),
                                     rbw_khz=float(words[4]),
                                     seg_mhz=(float(words[7]) if len(words) > 7 else 0.0),
                                     settle_us=(int(words[8]) if len(words) > 8 else 0))
            self.assertIsNotNone(plan, "%s: %s is not a plan" % (name, line))
            self.assertLessEqual(plan.segments, echoes.MAX_POINTS, name)



# ----------------------------------------------------------- phase 3.16's words and new members

def grow_frame(text, us=1193, usx=1207, dev="0044"):
    """One `scope` line as a scanner carrying phase 3.16's task 03 would have sent it.

    `us`, `usx` and `dev` after `sps` and before `lv`, which is where src/roles/scanner/Lines.cpp
    writes them; `dev` None for the one plan that carries none, a plan of a single segment. Each
    of the three is left out when it is None, so one function makes a frame of a scanner before
    that task, a frame of one segment after it and a frame of six.
    """
    obj = json.loads(text)
    grown = {}
    for key, value in obj.items():
        if key == "lv":
            for name, number in (("us", us), ("usx", usx), ("dev", dev)):
                if number is not None:
                    grown[name] = number
        grown[key] = value
    return json.dumps(grown)


def with_new_members(lines, dev="0044", us=1193, usx=1207):
    """The same capture as a scanner carrying phase 3.16's task 03 would have sent it.

    Every `scope` frame gains `us`, `usx` and - for a plan of more than one segment - `dev`,
    after `sps` and before `lv`; every `scope` ack gains ` reg=ldo gain=noboost` after what task
    02 added. Nothing else of either line moves, which is the point of the test below.
    """
    out = []
    for text in lines:
        if not text.startswith("{"):
            out.append(text)
            continue
        obj = json.loads(text)
        if obj.get("t") == "scope":
            out.append(grow_frame(text, us=us, usx=usx, dev=dev))
        elif obj.get("t") == "ack" and obj.get("cmd") == "scope":
            obj["msg"] = str(obj.get("msg", "")) + " reg=ldo gain=noboost"
            out.append(json.dumps(obj))
        else:
            out.append(text)
    return out


# ------------------------------------------------------- a session's download, made (task 04)

# How far over the floor a planted echo's place stands in the frames that do **not** hold the
# carrier. The frames of /noisescope/3 read the echoes' places 4 to 8 dB high even when quiet
# (tasks/phase-3.16/README.md), which is the thing the quiet-frame rule has to see past and the
# thing the table prints beside each place.
QUIET_OVER_DB = 5.0

# The garden's own spans, which are whole megahertz because a session's step is forty characters
# (tools/noisescope/lists/garden-a.txt). All four are rows of scope_plans.json.
GARDEN_WIDE = dict(f0=780.0, f1=960.0, points=0, rbw_khz=467.0)          # 386, 466.32, 6 segments
GARDEN_WIDE_LOWER = dict(f0=773.0, f1=953.0, points=0, rbw_khz=467.0)    # the same, 7 MHz lower
GARDEN_SIXTY = dict(f0=840.0, f1=900.0, points=0, rbw_khz=467.0)         # 129, 465.12, 2 segments
GARDEN_NARROW = dict(f0=863.0, f1=876.0, points=250, rbw_khz=58.6)       # 250, 52.00, 1 segment
# And the same wide span begun one whole interval lower, which is the one shift that cannot
# answer anything: both readings then predict the same places.
GARDEN_WIDE_ONE_INTERVAL = dict(f0=774.404, f1=954.404, points=0, rbw_khz=467.0)

GARDEN_PLACES_MHZ = (800.0, 820.0, 840.0, 860.0, 880.0, 900.0, 920.0, 940.0)


def plant_at_mhz(plan, places_mhz, carrier_mhz=None, tail=6):
    """One sweep's levels with a block at each **frequency**, wherever this plan's segments fall.

    The other reading of the echoes of 23 November: a block that belongs to the signal or to the
    site sits at a frequency and does not care where a segment began. `plant()` is the first
    reading; a pair of scopes planted with one of the two and read by `echoes.py --steps` is how
    the pair's line is held to both.
    """
    levels = [FLOOR_DBM + RIPPLE_DB * ((i % 3) - 1) for i in range(plan.points)]
    for mhz in places_mhz:
        at = int(round((mhz - plan.first_mhz) / plan.step_mhz))
        if not 0 <= at < plan.points:
            continue
        for k in range(tail + 1):
            index = at + k
            if index >= plan.points:
                break
            over = BLOCK_DB * (1.0 - float(k) / float(tail + 1))
            levels[index] = max(levels[index], FLOOR_DBM + over)
    if carrier_mhz is not None:
        index = int(round((carrier_mhz - plan.first_mhz) / plan.step_mhz))
        if 0 <= index < plan.points:
            levels[index] = CARRIER_DBM
    return levels


def garden_step(spec, rule=None, value=None, places_mhz=None, carrier_mhz=869.618, lit=10,
                quiet=50, reg=None, gain=None, us=1193, usx=1207, dev="0000", seq=0):
    """The lines of one step of a session: its `ack`, its lit frames and its quiet ones.

    A garden step is not a bench capture: the signal that lights the echoes is a neighbour's
    traffic, so it is in some frames and not in others - ten of sixty in session 3, which is this
    function's default. A quiet frame carries the floor and the echoes' own places QUIET_OVER_DB
    over it, and no carrier at all. Returns (lines, the next seq).
    """
    plan = plan_of(spec)
    ack = json.loads(ack_line(plan))
    for name, word in (("reg", reg), ("gain", gain)):
        if word is not None:
            ack["msg"] = "%s %s=%s" % (ack["msg"], name, word)
    lines = [json.dumps(ack)]
    if rule is not None:
        bright = plant(plan, rule, value, carrier_mhz=carrier_mhz)
    else:
        bright = plant_at_mhz(plan, places_mhz or (), carrier_mhz=carrier_mhz)
    floor = [FLOOR_DBM + RIPPLE_DB * ((i % 3) - 1) for i in range(plan.points)]
    dim = [floor[i] + (bright[i] - floor[i]) * (QUIET_OVER_DB / BLOCK_DB)
           for i in range(plan.points)]
    carrier_index = int(round((carrier_mhz - plan.first_mhz) / plan.step_mhz))
    if 0 <= carrier_index < plan.points:
        dim[carrier_index] = floor[carrier_index]       # the signal is what a quiet frame lacks
    for number in range(lit + quiet):
        seq += 1
        levels = bright if number < lit else dim
        lines.append(grow_frame(frame_line(plan, levels, seq=seq), us=us, usx=usx, dev=dev))
    return lines, seq


def garden_capture(steps, chatter=True):
    """A session's download: the boot line, then one step of lines after another."""
    lines = []
    if chatter:
        lines.append(json.dumps({"t": "boot", "fw": "noisescope", "ver": "0.1.8",
                                 "role": "scanner", "board": "rak4630-single", "reset": "soft",
                                 "wdt": "", "radio_ok": True, "rtc": 1, "ina": 1, "ts": 0}))
    seq = 0
    for step in steps:
        more, seq = garden_step(seq=seq, **step)
        lines.extend(more)
        if chatter:
            lines.append(json.dumps({"t": "stat", "ts": 0, "ms": 1000 * seq, "up": 1,
                                     "mode": "scope", "home": 869.618}))
    return lines


def read_steps(lines, carrier_mhz=869.618, **kwargs):
    """What `--steps` reads: the rows, the pairs, and the readings behind them."""
    scopes, skipped, bad = echoes.read_capture(lines)
    scopes = [scope for scope in scopes if scope.frames]
    readings = [echoes.ScopeReading(scope, carrier_mhz=carrier_mhz, quiet_rule=True, **kwargs)
                for scope in scopes]
    return readings, echoes.steps_rows(readings), echoes.pairs_of(readings), skipped, bad


class TheFourWordsAndTheNewMembers(unittest.TestCase):
    """What phase 3.16's task 03 adds to a capture, read by a tool that has no use for it yet.

    The four words (`ldo`, `dcdc`, `boost`, `noboost`) are named in the `ack` and the frame says
    `us`, `usx` and `dev`. What this tool *does* with them is task 04's; what it must do today is
    read such a capture exactly as it reads one without them.
    """

    def test_a_capture_with_the_new_members_reads_exactly_as_one_without(self):
        lines = capture(list(map(plan_of, ALL_FOUR)), rule="mhz", value=5.6)
        grown = with_new_members(lines)
        self.assertNotEqual(lines, grown, "the fixture must actually differ")

        plain_readings, plain_verdict, plain_skipped, plain_bad = read(lines)
        new_readings, new_verdict, new_skipped, new_bad = read(grown)
        self.assertEqual(plain_skipped, new_skipped)
        self.assertEqual(plain_bad, new_bad)
        self.assertEqual(0, new_bad, "a frame with three more members is still a frame")
        self.assertEqual(plain_verdict, new_verdict, "the unit and the sentence both")
        self.assertEqual(len(plain_readings), len(new_readings))
        for before, after in zip(plain_readings, new_readings):
            self.assertEqual(before.plan.points, after.plan.points)
            self.assertEqual(before.plan.segments, after.plan.segments)
            self.assertEqual(len(before.echoes), len(after.echoes))
            for one, two in zip(before.echoes, after.echoes):
                self.assertAlmostEqual(one.distance_mhz, two.distance_mhz, places=9)
                self.assertEqual(one.distance_points, two.distance_points)
                self.assertEqual(one.segment.first, two.segment.first)

        # And the report, which is what a person reads, is the same text.
        plain_out = io.StringIO()
        echoes.print_report("(plain)", plain_readings, plain_skipped, plain_bad, out=plain_out)
        new_out = io.StringIO()
        echoes.print_report("(plain)", new_readings, new_skipped, new_bad, out=new_out)
        self.assertEqual(plain_out.getvalue(), new_out.getvalue())

    def test_the_acks_two_numbers_are_still_read_when_the_words_stand_after_them(self):
        # The width is the one thing of a plan a frame cannot carry, so the ack is where it comes
        # from (p3.16-02): the two words are appended after it and must not hide it.
        plan = echoes.plan_scope(779.618, 959.618, rbw_khz=467.0, seg_mhz=5.6, settle_us=500)
        self.assertIsNotNone(plan)
        ack = json.loads(ack_line(plan))
        ack["msg"] = str(ack["msg"]) + " reg=ldo gain=noboost"
        self.assertEqual(33, echoes.ack_segments(ack))
        self.assertAlmostEqual(5.6, echoes.ack_seg_mhz(ack))
        self.assertEqual(500, echoes.ack_settle_us(ack))
        # A line that named a word and neither number says neither number, and the tool then
        # reads the default width, as it does for every capture made before task 02.
        plain = echoes.plan_scope(779.618, 959.618, rbw_khz=467.0)
        bare = json.loads(ack_line(plain))
        bare["msg"] = str(bare["msg"]) + " reg=ldo"
        self.assertIsNone(echoes.ack_seg_mhz(bare))
        self.assertEqual(echoes.SETTLE_DEFAULT_US, echoes.ack_settle_us(bare),
                         "the settle is named on every scope since phase 3.17's task 01")
        self.assertEqual(6, echoes.ack_segments(bare))

    def test_a_capture_laid_out_by_the_ack_is_laid_out_the_same_with_the_words_on_it(self):
        # The narrow span of 23 November is one segment, so its frames carry no `dev` at all -
        # and a capture of it with `us` and `usx` alone reads as one with neither.
        plan = plan_of(NARROW)
        self.assertEqual(1, plan.segments)
        lines = capture([plan], rule="points", value=60)
        grown = with_new_members(lines, dev=None)
        plain_readings, _, _, plain_bad = read(lines)
        new_readings, _, _, new_bad = read(grown)
        self.assertEqual(0, plain_bad)
        self.assertEqual(0, new_bad)
        self.assertEqual(len(plain_readings[0].echoes), len(new_readings[0].echoes))

    def test_waterfall_plot_reads_a_frames_members_by_name_and_requires_none_of_the_new_ones(self):
        # waterfall_plot.py cannot be imported here - it needs numpy and matplotlib and this
        # harness is the standard library only (tools/release/selfcheck.py) - so it is read
        # instead: every member of a frame it takes, it takes by name out of the parsed object,
        # and the three new names are not among them. A frame with three more members is
        # therefore the same picture, and nothing in the file enumerates a frame's members or
        # compares them against a set, which is the only way a new one could break it.
        import ast

        path = os.path.join(os.path.dirname(os.path.abspath(echoes.__file__)),
                            "waterfall_plot.py")
        with open(path, "r", encoding="utf-8") as handle:
            source = handle.read()
        tree = ast.parse(source)

        keys = set()
        for node in ast.walk(tree):
            # obj.get("key") and obj.get("key", default)
            if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                    and node.func.attr == "get" and node.args
                    and isinstance(node.args[0], ast.Constant)
                    and isinstance(node.args[0].value, str)):
                keys.add(node.args[0].value)
            # obj["key"]
            if (isinstance(node, ast.Subscript) and isinstance(node.slice, ast.Constant)
                    and isinstance(node.slice.value, str)):
                keys.add(node.slice.value)

        for needed in ("f0", "step", "n", "lv", "t"):
            self.assertIn(needed, keys, "it does read the members it needs")
        for added in ("us", "usx", "dev"):
            self.assertNotIn(added, keys,
                             "%s is task 04's to use; today it must be ignored" % added)
        for enumerating in (".keys()", "set(obj)", "sorted(obj)"):
            self.assertNotIn(enumerating, source,
                             "nothing may enumerate a frame's members: %s" % enumerating)

def _rmtree(path):
    import shutil
    shutil.rmtree(path, ignore_errors=True)

# ------------------------------------------------- phase 3.16's task 04: a session, as one table

class TheGardensTable(unittest.TestCase):
    """`echoes.py --steps`: a session's download as one table, a row a scope.

    A garden capture is not a bench capture. The signal is a neighbour's traffic, so some frames
    hold it and some do not; the scanner carries `us`, `usx` and `dev`, or - if the session ran
    before phase 3.16's task 03 - it does not; and the words `ldo` and `noboost` are in the `ack`
    and nowhere else. Each of those is a column, and each is held here.
    """

    def test_a_row_a_scope_carries_the_plan_the_words_the_time_and_the_error_word(self):
        rows = read_steps(garden_capture([
            dict(spec=GARDEN_WIDE, rule="mhz", value=5.596, dev="0000"),
            dict(spec=GARDEN_WIDE, rule="mhz", value=5.596, reg="ldo", gain="noboost",
                 dev="0040", us=1193, usx=1207),
            dict(spec=GARDEN_NARROW, rule="points", value=12, dev=None, us=6912, usx=7104),
        ]))[1]
        self.assertEqual([1, 2, 3], [row["step"] for row in rows])

        first, second, third = rows
        self.assertEqual(386, first["points"])
        self.assertEqual(6, first["segments"])
        self.assertEqual(32.0, first["seg_mhz"])
        self.assertEqual(echoes.SETTLE_DEFAULT_US, first["settle_us"],
                         "a step that named no settle stood 500 us on each first point")
        self.assertIsNone(first["regulator"])
        self.assertIsNone(first["gain"], "no word on the line is not `boost`")
        self.assertEqual(60, first["frames"])
        self.assertEqual(10, first["lit_frames"], "ten of sixty, as session 3 had it")
        self.assertEqual(50, first["quiet_frames"])
        self.assertTrue(first["places"])

        # The two words come from the ack and from nowhere else: no frame carries them.
        self.assertEqual("ldo", second["regulator"])
        self.assertEqual("noboost", second["gain"])

        # The time a point cost, with both figures beside it. 1193 us over 386 points is 3.09 us,
        # against 3.43 for an update of 1.6 ms / RBW and 2.01 for Semtech's 0.94.
        self.assertAlmostEqual(1193 / 386.0, second["us_per_point_min"], places=6)
        self.assertAlmostEqual(1207 / 386.0, second["us_per_point_max"], places=6)
        self.assertAlmostEqual(3.4261, second["point_us_at_1_6"], places=3)
        self.assertAlmostEqual(2.0128, second["point_us_at_0_94"], places=3)

        # The error word, OR-ed over the scope and said in words when it is not 0000.
        self.assertEqual(0, first["device_errors"])
        self.assertEqual("0000, nothing", first["device_errors_said"])
        self.assertEqual(0x0040, second["device_errors"])
        self.assertIn("the PLL did not lock", second["device_errors_said"])

        # A `scope` of one segment carries no `dev` at all, and that is the only way to know.
        self.assertEqual(1, third["segments"])
        self.assertIsNone(third["device_errors"])
        self.assertEqual("-", third["device_errors_said"])
        self.assertAlmostEqual(6912 / 250.0, third["us_per_point_min"], places=6)

    def test_a_capture_made_before_03_is_read_with_the_two_columns_empty(self):
        """Every line typed before that task is such a capture, and there is a year of them."""
        rows = read_steps(capture([plan_of(GARDEN_WIDE)], rule="mhz", value=5.596, frames=60))[1]
        self.assertEqual(1, len(rows))
        row = rows[0]
        self.assertIsNone(row["us_min"])
        self.assertIsNone(row["us_max"])
        self.assertIsNone(row["us_per_point_min"])
        self.assertIsNone(row["us_per_point_max"])
        self.assertIsNone(row["device_errors"])
        self.assertTrue(row["places"], "and everything else of the row is read as always")
        out = io.StringIO()
        echoes.print_steps("(before 03)", read_steps(
            capture([plan_of(GARDEN_WIDE)], rule="mhz", value=5.596, frames=60))[0], 0, 0,
            out=out)
        printed = [line for line in out.getvalue().splitlines() if line.startswith("   1 ")]
        self.assertEqual(1, len(printed), out.getvalue())
        self.assertEqual(4, len([word for word in printed[0].split() if word == "-"]),
                         "the regulator, the gain, the time a point cost and `dev` are all "
                         "empty in such a capture: %r" % printed[0])

    def test_the_quiet_frame_rule_is_used_with_five_quiet_frames_and_not_with_four(self):
        for quiet, rule, said in ((4, "floor", "floor"), (5, "quiet", "quiet(5)")):
            readings, rows, _pairs, _skipped, _bad = read_steps(garden_capture([
                dict(spec=GARDEN_SIXTY, rule="points", value=12, lit=10, quiet=quiet)]))
            self.assertEqual(rule, readings[0].rule, "%d quiet frame(s)" % quiet)
            self.assertEqual(rule, rows[0]["rule"])
            self.assertEqual(quiet, rows[0]["quiet_frames"])
            out = io.StringIO()
            echoes.print_steps("(planted)", readings, 0, 0, out=out)
            self.assertIn(said, out.getvalue(), "the row must say which rule it used")
            if rule == "floor":
                self.assertIn("fewer than %d" % echoes.MIN_QUIET_FRAMES, out.getvalue())
                for place in rows[0]["places"]:
                    self.assertIsNone(place["quiet_over_floor_db"])
            else:
                for place in rows[0]["places"]:
                    self.assertIsNotNone(place["quiet_over_floor_db"])

    def test_a_places_height_in_the_quiet_frames_is_printed_beside_it(self):
        """The 4 to 8 dB /noisescope/3 reads at the same places with no carrier in the frame."""
        _readings, rows, _pairs, _skipped, _bad = read_steps(garden_capture([
            dict(spec=GARDEN_SIXTY, rule="points", value=12)]))
        self.assertTrue(rows[0]["places"])
        for place in rows[0]["places"]:
            self.assertGreater(place["quiet_over_floor_db"], QUIET_OVER_DB - 2.0)
            self.assertLess(place["quiet_over_floor_db"], QUIET_OVER_DB + 4.0)

    def test_a_frame_is_never_longer_than_the_bytes_a_step_is_priced_with(self):
        """The arithmetic a session's step is sized by, against real lines.

        `scopeFrameBytes()` is a **bound** and a session's budget is worked out from it
        (tools/noisescope/hunt.py, `step_lines()`). The two frames below are real: printed by
        phase 3.16's task 03 over the real src/roles/scanner/Lines.cpp and quoted in its report
        (*What a frame says now*), levels and all. Every `scope` line of every capture kept under
        tools/noisescope/captures/ is held to the same bound, and the report says so when there is
        none to hold.
        """
        for points, emitted in ((386, 705), (250, 513)):
            bound = echoes.frame_bytes(points)
            self.assertGreaterEqual(bound, emitted,
                                    "%d points: a real line was %d bytes" % (points, emitted))
        self.assertEqual(762, echoes.frame_bytes(386))
        self.assertEqual(582, echoes.frame_bytes(250))
        self.assertEqual(2978, echoes.frame_bytes(2048))
        # And the port's floor, which is what an interval of 0 becomes on the Walter's UART.
        self.assertEqual(134, echoes.interval_floor_ms(386, 115200))
        self.assertEqual(102, echoes.interval_floor_ms(250, 115200))

        directory = os.path.join(os.path.dirname(os.path.abspath(echoes.__file__)), "captures")
        real = []
        for root, _dirs, files in os.walk(directory):
            for name in files:
                if name.endswith(".ndjson"):
                    real.append(os.path.join(root, name))
        for path in real:
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                for text in handle:
                    if '"t":"scope"' not in text.replace(" ", ""):
                        continue
                    obj = json.loads(text)
                    self.assertLessEqual(
                        len(text.rstrip("\r\n")) + 1, echoes.frame_bytes(int(obj["n"])),
                        "%s: a real frame of %d points is longer than the bound"
                        % (os.path.basename(path), obj["n"]))

    def test_every_step_of_the_four_garden_lists_is_a_scope_this_firmware_runs(self):
        """The lines of tasks/phase-3.16/README.md's four lists, read by hunt.py's own reader.

        Six steps each but garden-d, which lost one to make the room for `max 32 0` on its
        reference line when phase 3.17's task 01 made 500 us the default.
        """
        directory = os.path.join(os.path.dirname(os.path.abspath(echoes.__file__)), "lists")
        for letter, steps in (("a", 6), ("b", 6), ("c", 6), ("d", 5)):
            path = os.path.join(directory, "garden-%s.txt" % letter)
            self.assertTrue(os.path.isfile(path), path)
            sessions = hunt.read_sessions(path)
            self.assertEqual(1, len(sessions), path)
            self.assertEqual(steps, len(sessions[0].steps), path)
            for step in sessions[0].steps:
                request = echoes.parse_scope(step.command)
                self.assertIsNotNone(request, "%s: %r" % (path, step.command))
                self.assertIsNotNone(request.plan, "%s: %r" % (path, step.command))
                self.assertGreater(request.interval_ms, 0,
                                   "%s: a garden step keeps its own interval" % path)

    def test_the_table_and_the_pairs_from_the_command_line(self):
        import tempfile
        lines = garden_capture([
            dict(spec=GARDEN_WIDE, rule="mhz", value=5.596),
            dict(spec=GARDEN_WIDE_LOWER, rule="mhz", value=5.596),
        ])
        directory = tempfile.mkdtemp(prefix="echoes-steps-")
        self.addCleanup(_rmtree, directory)
        path = os.path.join(directory, "noisescope-bellevue-20261125-193000.ndjson")
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("".join(line + "\n" for line in lines))

        out = io.StringIO()
        self.assertEqual(0, echoes.main(["--steps", path, "--carrier", "869.618"], out=out))
        text = out.getvalue()
        self.assertIn("step span MHz", text)
        self.assertIn("the pairs of this capture", text)
        self.assertIn(echoes.MOVES, text)

        out = io.StringIO()
        self.assertEqual(0, echoes.main(["--steps", path, "--carrier", "869.618", "--json"],
                                        out=out))
        parsed = json.loads(out.getvalue())
        self.assertEqual(2, len(parsed["steps"]))
        self.assertEqual(1, len(parsed["pairs"]))
        self.assertEqual("first_point", parsed["pairs"][0]["kind"])
        self.assertEqual(echoes.MOVES, parsed["pairs"][0]["verdict"])

    def test_a_step_that_lost_its_ack_is_said_to_have_lost_it(self):
        """Two steps of one span are two scopes only because their `ack`s differ.

        A download carries every line the scanner sent, this one included; a file that lost them
        would read garden-b's first three steps as one scope and say nothing about it, which is
        the one way `--steps` could be quietly wrong.
        """
        lines = [text for text in garden_capture([
            dict(spec=GARDEN_SIXTY, rule="points", value=12)])
            if '"t": "ack"' not in text and '"t":"ack"' not in text]
        readings, rows, _pairs, _skipped, _bad = read_steps(lines)
        self.assertEqual(1, len(rows))
        self.assertFalse(rows[0]["has_ack"])
        out = io.StringIO()
        echoes.print_steps("(no ack)", readings, 0, 0, out=out)
        self.assertIn("carries no `ack`", out.getvalue())
        # And with the acks it says nothing of the kind.
        rows = read_steps(garden_capture([
            dict(spec=GARDEN_SIXTY, rule="points", value=12)]))[1]
        self.assertTrue(rows[0]["has_ack"])

    def test_the_old_table_is_untouched_by_the_new_one(self):
        """`echoes.py` without `--steps` reads what it always read: no quiet rule, no columns."""
        lines = garden_capture([dict(spec=GARDEN_SIXTY, rule="points", value=12)])
        scopes, _skipped, _bad = echoes.read_capture(lines)
        reading = echoes.ScopeReading(scopes[0], carrier_mhz=869.618)
        self.assertEqual("floor", reading.rule, "the quiet rule is --steps' and nothing else's")
        self.assertEqual({}, reading.quiet_floors)
        self.assertEqual([], reading.quiet_levels)
        out = io.StringIO()
        echoes.print_report("(planted)", [reading], 0, 0, out=out)
        self.assertNotIn("quiet", out.getvalue())


class ThePairsSay(unittest.TestCase):
    """Two scopes of a session that differ in exactly one thing, and what they are allowed to say.

    The pile's whole question is one pair: the same span begun somewhere else. An echo that
    follows the sweep's segments moves with them; one that follows the signal or the site stays
    where it is; and there are two shapes of capture that cannot tell the two apart and have to
    say so rather than guess.
    """

    def _pairs(self, steps):
        return read_steps(garden_capture(steps))[2]

    def test_echoes_planted_on_the_segments_move_with_the_segments(self):
        pairs = self._pairs([dict(spec=GARDEN_WIDE, rule="mhz", value=5.596),
                             dict(spec=GARDEN_WIDE_LOWER, rule="mhz", value=5.596)])
        self.assertEqual(1, len(pairs))
        pair = pairs[0]
        self.assertEqual("first_point", pair["kind"])
        self.assertEqual(echoes.MOVES, pair["verdict"], pair["why"])
        self.assertEqual(0, pair["counted"]["frequency"])
        self.assertGreater(pair["counted"]["distance"], 2)
        self.assertAlmostEqual(-7.0, pair["shift_mhz"], places=3)

    def test_echoes_planted_on_the_frequency_stay_with_the_frequency(self):
        pairs = self._pairs([dict(spec=GARDEN_WIDE, places_mhz=GARDEN_PLACES_MHZ),
                             dict(spec=GARDEN_WIDE_LOWER, places_mhz=GARDEN_PLACES_MHZ)])
        self.assertEqual(1, len(pairs))
        pair = pairs[0]
        self.assertEqual(echoes.STAYS, pair["verdict"], pair["why"])
        self.assertEqual(0, pair["counted"]["distance"])
        self.assertGreater(pair["counted"]["frequency"], 2)

    def test_a_span_begun_one_whole_interval_lower_cannot_say_and_says_why(self):
        """Both readings then predict the same places, whichever one is true."""
        pairs = self._pairs([dict(spec=GARDEN_WIDE, rule="mhz", value=5.596),
                             dict(spec=GARDEN_WIDE_ONE_INTERVAL, rule="mhz", value=5.596)])
        self.assertEqual(1, len(pairs))
        pair = pairs[0]
        self.assertEqual(echoes.CANNOT, pair["verdict"])
        self.assertIn("both readings predict the same places", pair["why"])
        self.assertIn("1 x the", pair["why"])

    def test_fewer_than_three_echoes_cannot_say_and_says_which(self):
        pairs = self._pairs([dict(spec=GARDEN_WIDE, places_mhz=(820.0, 900.0)),
                             dict(spec=GARDEN_WIDE_LOWER, places_mhz=(820.0, 900.0))])
        self.assertEqual(1, len(pairs))
        pair = pairs[0]
        self.assertEqual(echoes.CANNOT, pair["verdict"])
        self.assertIn("fewer than %d echoes" % echoes.MIN_ECHOES_FOR_A_VERDICT, pair["why"])
        self.assertIn("step 2 (2)", pair["why"], "it names which scope was too thin, and how thin")

    def test_the_regulator_the_gain_the_width_and_the_settle_are_each_one_pair(self):
        """One line each and no verdict beyond the counts: what a word means is not read here."""
        steps = [
            dict(spec=GARDEN_SIXTY, rule="points", value=12),
            dict(spec=GARDEN_SIXTY, rule="points", value=12, reg="ldo"),
            dict(spec=GARDEN_SIXTY, rule="points", value=12, gain="noboost"),
        ]
        pairs = self._pairs(steps)
        kinds = sorted(pair["kind"] for pair in pairs)
        self.assertEqual(["gain", "regulator"], kinds,
                         "steps 2 and 3 differ in two things and are no pair")
        for pair in pairs:
            self.assertNotIn("verdict", pair, "a word is not given a verdict here")
            self.assertEqual([], pair["only_first"])
            self.assertEqual([], pair["only_second"])
            self.assertEqual(10, len(pair["both"]))
            self.assertAlmostEqual(0.0, pair["median_difference_db"], places=6)

        # A width and a settle come from the ack, which is the only place they are written.
        plan = echoes.plan_scope(840.0, 900.0, rbw_khz=467.0, seg_mhz=11.0)
        self.assertIsNotNone(plan)
        wide = dict(f0=840.0, f1=900.0, points=0, rbw_khz=467.0)
        # Six points a block here and not twelve: a segment of 11 MHz holds 22 points and three
        # blocks is the fewest that say an interval (`plant()`).
        pairs = read_steps(garden_capture([
            dict(spec=wide, rule="points", value=6),
            dict(spec=dict(wide, seg_mhz=11.0), rule="points", value=6),
            # `0` and not 500: since phase 3.17's task 01 the first step's own settle *is* 500,
            # so the pair that differs in the settle alone is the default against none at all.
            dict(spec=dict(wide, settle_us=0), rule="points", value=6),
        ]))[2]
        kinds = sorted(pair["kind"] for pair in pairs)
        self.assertEqual(["settle", "width"], kinds)
        for pair in pairs:
            if pair["kind"] == "width":
                self.assertEqual("32.0 MHz -> 11.0 MHz", pair["differ"])
            else:
                self.assertEqual("500 us -> 0 us", pair["differ"])

    def test_a_pair_that_differs_in_two_things_is_no_pair_at_all(self):
        pairs = self._pairs([dict(spec=GARDEN_WIDE, rule="mhz", value=5.596),
                             dict(spec=GARDEN_WIDE_LOWER, rule="mhz", value=5.596, reg="ldo")])
        self.assertEqual([], pairs)

    def test_a_capture_with_no_pair_in_it_says_so(self):
        readings = read_steps(garden_capture([
            dict(spec=GARDEN_WIDE, rule="mhz", value=5.596)]))[0]
        out = io.StringIO()
        echoes.print_steps("(one scope)", readings, 0, 0, out=out)
        self.assertIn("no two scopes of this capture differ in exactly one thing",
                      out.getvalue())

if __name__ == "__main__":
    unittest.main()
