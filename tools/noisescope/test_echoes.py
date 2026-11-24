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
    return json.dumps({
        "t": "ack", "cmd": "scope", "ok": True,
        "msg": ("n=%d step=%.4fkHz rbw=%.1fkHz seg=%d cov=%.3f interval=0ms sweep=%dms"
                % (plan.points, plan.step_khz, plan.rbw_khz, plan.segments, plan.cov, 1)),
    })


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
    plan = echoes.plan_scope(spec["f0"], spec["f1"], points=spec["points"],
                             rbw_khz=spec["rbw_khz"])
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
                and one["request"]["points"] == 0 and one["request"]["rbw_khz"] == 467.0]
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


def _rmtree(path):
    import shutil
    shutil.rmtree(path, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
