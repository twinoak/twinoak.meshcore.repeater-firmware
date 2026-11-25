"""tools/noisescope/hunt.py against tools/bench/fakes.py's scanner - no board, and none needed.

Run where tools/bench's own tests are (tools/release/selfcheck.py). Three kinds of thing:

  * **the rails**: no `bench.local.json` or no port with its serial number is exit 3 and *no
    bench*, a board in its bootloader is refused, a board another process holds is exit 4, and
    nothing is flashed, erased or restored by this tool whatever its list says;
  * **the run**: the list sent line for line on one open port, a capture per `scope` under
    `out/bench/echoes/<label>/`, `run.json` beside them, and tools/noisescope/echoes.py's answer
    for each - including the verdict over a capture whose scopes were planted with one rule.

  * **the garden** (phase 3.16's task 04): a list as the session programs meshmaster.dk and the
    Walter would take, every rule of theirs broken once and named in the refusal, the four lists
    of `tools/noisescope/lists/` taken as they are written, and a bench that explodes on being
    looked at - because `--sessions` must ask tools/bench for nothing at all.

The fake scanner is given the one thing it did not have: frames that belong to the span that was
asked for. It takes them from a hook (`scope_frames_for`), and the arithmetic behind the hook is
`planScope()`'s copy in tools/noisescope/echoes.py - so tools/bench stays free of it and this file
carries it, which is where a test of a noisescope tool belongs.
"""

import io
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
BENCH = os.path.join(REPO, "tools", "bench")
for path in (HERE, BENCH):
    if path not in sys.path:
        sys.path.insert(0, path)

import config as cfg  # noqa: E402
from fakes import FakeBoard, FakeClock, FakePorts, FakeProbe, FakeScannerFirmware  # noqa: E402
from flash import Timeouts  # noqa: E402
from lock import BoardLock  # noqa: E402

import bench as bench_tool  # noqa: E402
import echoes  # noqa: E402
import hunt  # noqa: E402
import test_echoes  # noqa: E402  (the capture maker: one file makes every synthetic capture)

SERIAL = "E926F6BEB407A412"
BOARD = "rak4630-single"


def scope_plan_of(words):
    """The plan a `scope` line asks for, as the firmware would make it. None for a refusal."""
    try:
        f0, f1 = float(words[1]), float(words[2])
    except (IndexError, ValueError):
        return None
    points = int(float(words[3])) if len(words) > 3 else 0
    rbw = float(words[4]) if len(words) > 4 else 0.0
    return echoes.plan_scope(f0, f1, points=points, rbw_khz=rbw)


def planted_scanner(rule=None, value=None, frames=2, carrier_mhz=869.618):
    """A FakeScannerFirmware whose `scope` answers the span it was asked for.

    With a rule, each frame carries blocks planted by it - the same planting
    tools/noisescope/test_echoes.py uses - so that a run of hunt.py over the fake ends in the
    same verdict a run over a board would.
    """

    def ack_for(words):
        plan = scope_plan_of(words)
        if plan is None:
            return "started"
        return ("n=%d step=%.4fkHz rbw=%.1fkHz seg=%d cov=%.3f interval=0ms sweep=%dms"
                % (plan.points, plan.step_khz, plan.rbw_khz, plan.segments, plan.cov, 1))

    def frames_for(words):
        plan = scope_plan_of(words)
        if plan is None:
            return []
        if rule is None:
            levels = [test_echoes.FLOOR_DBM] * plan.points
            index = int(round((carrier_mhz - plan.first_mhz) / plan.step_mhz))
            if 0 <= index < plan.points:
                levels[index] = test_echoes.CARRIER_DBM
        else:
            levels = test_echoes.plant(plan, rule, value, carrier_mhz=carrier_mhz)
        line = json.loads(test_echoes.frame_line(plan, levels))
        line.pop("seq", None)
        return [dict(line) for _ in range(frames)]

    return FakeScannerFirmware(version="0.1.7", scope_frames_for=frames_for,
                               scope_ack_for=ack_for)


# The lines a test sends: one `scope` of each of the four shapes test_echoes plants in, so that
# one run of hunt.py over the fake can be asked which unit the interval keeps.
FOUR_LINES = [
    ("wide-467", "scope 779.618 959.618 0 467 0 last", "467 kHz, 386 points"),
    ("wide-234", "scope 779.618 959.618 0 234.3 0 last", "234.3 kHz, 769 points"),
    ("wide-467-halved", "scope 779.618 959.618 772 467 0 last", "467 kHz, 772 points"),
    ("wide-234-halved", "scope 779.618 959.618 1538 234.3 0 last", "234.3 kHz, 1538 points"),
]


class HuntCase(unittest.TestCase):
    """A temporary repository with a bench.local.json, a temporary data directory, fakes."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="twinoak-hunt-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.repo = os.path.join(self.tmp, "repo")
        os.makedirs(self.repo)
        self.home = os.path.join(self.tmp, "home")
        self.clock = FakeClock()
        self.out = io.StringIO()

    def config(self, rf="antenna", identity="disposable", serial=SERIAL, **extra):
        data = {"board": BOARD, "serial": serial, "rf": rf, "identity": identity}
        data.update(extra)
        with open(os.path.join(self.repo, cfg.BENCH_FILE), "w", encoding="utf-8") as handle:
            json.dump(data, handle)
        return data

    def bench(self, firmware=None, mode="application", probe=None, boards=None):
        if boards is None:
            board = FakeBoard(serial=SERIAL, firmware=firmware or planted_scanner())
            board.mode = mode
            boards = [board]
        self.boards = boards
        self.ports = FakePorts(boards=boards, clock=self.clock)
        self.probe = probe if probe is not None else FakeProbe({os.getpid(): 1.0})
        return bench_tool.Bench(repo=self.repo, ports=self.ports, home=self.home, out=self.out,
                                probe=self.probe, python="python",
                                timeouts=Timeouts(settle_s=0.0, dfu_settle_s=0.0))

    def run_hunt(self, argv=None, **kwargs):
        bench = self.bench(**kwargs)
        argv = list(argv or [])
        if not any(one == "--repo" for one in argv):
            argv += ["--repo", self.repo]
        code = hunt.main(argv, bench=bench, out=self.out)
        return code, self.out.getvalue()

    def directory(self, label):
        return os.path.join(self.repo, "out", "bench", "echoes", label)


# ----------------------------------------------------------------------------- the rails

class TheRails(HuntCase):

    def test_no_config_is_no_bench(self):
        code, text = self.run_hunt()
        self.assertEqual(cfg.EXIT_NO_BENCH, code, text)
        self.assertIn("no bench", text)
        self.assertEqual([], self.ports.opened, "a port was opened with no bench")

    def test_no_port_with_that_serial_number_is_no_bench(self):
        self.config()
        code, text = self.run_hunt(boards=[])
        self.assertEqual(cfg.EXIT_NO_BENCH, code, text)
        self.assertIn("no port with serial number", text)

    def test_a_board_in_its_bootloader_is_refused(self):
        self.config()
        code, text = self.run_hunt(mode="bootloader")
        self.assertEqual(cfg.EXIT_REFUSED, code, text)
        self.assertIn("no console to talk to", text)

    def test_a_board_another_process_holds_is_exit_4(self):
        self.config()
        probe = FakeProbe({os.getpid(): 1.0, 4242: 500.0})
        os.makedirs(os.path.join(self.home, "locks"))
        with open(BoardLock(SERIAL, "x", home=self.home).path, "w", encoding="utf-8") as handle:
            json.dump({"pid": 4242, "process_started": 500.0, "taken": "t",
                       "command": "console", "host": "h"}, handle)
        code, text = self.run_hunt(probe=probe,
                                   argv=["--label", "held", "--seconds", "1"])
        self.assertEqual(cfg.EXIT_LOCKED, code, text)
        self.assertIn("locked", text)
        self.assertEqual([], self.ports.opened, "a port was opened while the board was held")

    def test_nothing_is_flashed_and_nothing_is_restored(self):
        self.config()
        code, text = self.run_hunt(argv=["--label", "rails", "--seconds", "1"])
        self.assertEqual(cfg.EXIT_OK, code, text)
        self.assertEqual([], self.boards[0].flashes, "this tool flashes nothing")
        self.assertEqual(0, self.boards[0].touches, "and touches no port at 1200 baud")
        for line in self.boards[0].firmware.received:
            self.assertNotIn(line.split()[0].lower(), ("erase", "dfu", "reboot", "password"),
                             "this tool sent %r" % line)

    def test_a_list_file_with_no_line_in_it_is_refused(self):
        self.config()
        path = os.path.join(self.tmp, "empty.txt")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("# nothing but a comment\n\n")
        code, text = self.run_hunt(argv=["--list", path, "--label", "empty"])
        self.assertEqual(cfg.EXIT_REFUSED, code, text)
        self.assertIn("no line in it", text)

    def test_the_default_list_is_the_tasks_own_questions(self):
        names = [name for name, _line, _about in hunt.DEFAULT_LIST]
        self.assertIn("wide-467", names)
        self.assertIn("narrow-58", names)
        self.assertIn("wide-467-shifted", names)
        self.assertIn("boost-off", names)
        self.assertEqual("boost-on", names[-1], "the scanner is left with boosted gain on")
        for _name, line, _about in hunt.DEFAULT_LIST:
            if line.startswith("scope"):
                words = line.split()
                self.assertEqual("0", words[5], "%r must ask for a frame per sweep" % line)
                self.assertEqual("last", words[6], "%r must ask for one sweep a frame" % line)
                self.assertIsNotNone(scope_plan_of(words), "%r is not a scope" % line)


# ----------------------------------------------------------------------------- the run

class TheRun(HuntCase):

    def test_the_list_is_sent_line_for_line_on_one_open_port(self):
        self.config()
        path = os.path.join(self.tmp, "two.txt")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("# two lines and one that is not a scope\n")
            handle.write("wide = scope 779.618 959.618 0 467 0 last\n")
            handle.write("boost-off = boost off\n")
            handle.write("narrow = scope 863.118 876.118 250 58.6 0 last\n")
        code, text = self.run_hunt(argv=["--list", path, "--label", "two", "--seconds", "1"],
                                   firmware=planted_scanner(rule="points", value=12))
        self.assertEqual(cfg.EXIT_OK, code, text)
        sent = self.boards[0].firmware.received
        self.assertEqual(["info", "scope 779.618 959.618 0 467 0 last", "stop",
                          "boost off", "scope 863.118 876.118 250 58.6 0 last", "stop"],
                         sent)
        self.assertEqual(1, len(self.ports.opened), "one open port for the whole run: %r"
                         % (self.ports.opened,))

    def test_a_capture_per_scope_and_a_run_file(self):
        self.config()
        code, text = self.run_hunt(argv=["--list", self._four(), "--label", "four",
                                         "--seconds", "1"],
                                   firmware=planted_scanner(rule="mhz", value=5.596))
        self.assertEqual(cfg.EXIT_OK, code, text)
        directory = self.directory("four")
        files = sorted(os.listdir(directory))
        self.assertEqual(["01-wide-467.ndjson", "02-wide-234.ndjson",
                          "03-wide-467-halved.ndjson", "04-wide-234-halved.ndjson",
                          "run.json"], files)
        with open(os.path.join(directory, "run.json"), encoding="utf-8") as handle:
            run = json.load(handle)
        self.assertEqual("four", run["label"])
        self.assertEqual(869.618, run["carrier_mhz"])
        self.assertEqual("noisescope", run["info"]["fw"])
        self.assertEqual("SX1261 V2D 2D02", run["info"]["rom"])
        self.assertEqual(4, len(run["lines"]))
        for row in run["lines"]:
            self.assertTrue(row["ok"], row)
            self.assertEqual(1, len(row["scopes"]), row["name"])
            self.assertTrue(row["scopes"][0]["echoes"], row["name"])
            self.assertTrue(row["capture"].startswith("out/bench/echoes/four/"), row["capture"])

    def test_the_ack_is_kept_in_the_capture_with_the_frames(self):
        self.config()
        code, text = self.run_hunt(argv=["--label", "one", "--seconds", "1",
                                         "--list", self._four(1)],
                                   firmware=planted_scanner(rule="points", value=12))
        self.assertEqual(cfg.EXIT_OK, code, text)
        path = os.path.join(self.directory("one"), "01-wide-467.ndjson")
        with open(path, encoding="utf-8") as handle:
            objects = [json.loads(line) for line in handle if line.strip()]
        self.assertEqual("ack", objects[0]["t"])
        self.assertEqual("scope", objects[0]["cmd"])
        self.assertIn("seg=6", objects[0]["msg"])
        self.assertTrue([one for one in objects if one["t"] == "scope"])
        # And echoes.py reads that capture as it reads any other.
        scopes, _skipped, bad = echoes.read_capture(path)
        self.assertEqual(0, bad)
        self.assertEqual(1, len(scopes))
        self.assertEqual(6, scopes[0].ack_segments)
        self.assertEqual(386, scopes[0].plan.points)

    def _rule_is_named(self, rule, value, expected):
        """The whole point of the list: four scopes, one rule, one answer."""
        self.config()
        code, text = self.run_hunt(argv=["--list", self._four(), "--label", rule,
                                         "--seconds", "1"],
                                   firmware=planted_scanner(rule=rule, value=value))
        self.assertEqual(cfg.EXIT_OK, code, text)
        with open(os.path.join(self.directory(rule), "run.json"), encoding="utf-8") as handle:
            run = json.load(handle)
        self.assertEqual(4, sum(len(row["scopes"]) for row in run["lines"]))
        # Every line of the run says what its own capture follows, which inside one scope is
        # nothing; the four captures read together are what name the rule.
        together = []
        for row in run["lines"]:
            scopes, _skipped, _bad = echoes.read_capture(os.path.join(self.repo, row["capture"]))
            together.extend(echoes.ScopeReading(scope, carrier_mhz=869.618)
                            for scope in scopes if scope.frames)
        unit, line = echoes.verdict(together)
        self.assertEqual(rule, unit, line)
        self.assertIn(expected, line)

    def test_a_run_whose_echoes_follow_the_frequency_says_so(self):
        self._rule_is_named("mhz", 5.596, "the frequency")

    def test_a_run_whose_echoes_follow_the_points_says_so(self):
        self._rule_is_named("points", 12, "the sweep's own points")

    def test_a_run_whose_echoes_follow_the_clock_says_so(self):
        self._rule_is_named("us", 41.1, "the clock")

    def test_a_line_the_board_refuses_is_a_failure_and_the_rest_still_runs(self):
        self.config()
        firmware = planted_scanner(rule="points", value=12)
        firmware.rom = "SX1261 V1B 2D00"   # the plugin is made for one ROM and this is not it
        code, text = self.run_hunt(argv=["--list", self._four(2), "--label", "wrong-rom",
                                         "--seconds", "1"], firmware=firmware)
        self.assertEqual(cfg.EXIT_FAILED, code, text)
        self.assertIn("the sweep plugin needs", text)
        with open(os.path.join(self.directory("wrong-rom"), "run.json"), encoding="utf-8") as fh:
            run = json.load(fh)
        self.assertEqual(2, len(run["lines"]))
        for row in run["lines"]:
            self.assertFalse(row["ok"], row["name"])
            self.assertEqual([], row["scopes"])
            self.assertIsNone(row["capture"])

    def test_a_scope_that_answers_no_frame_is_a_failure(self):
        self.config()
        firmware = planted_scanner()
        firmware.answers_data = False      # acks, and no frame at all
        code, text = self.run_hunt(argv=["--list", self._four(1), "--label", "silent",
                                         "--seconds", "1"], firmware=firmware)
        self.assertEqual(cfg.EXIT_FAILED, code, text)
        self.assertIn("no frame came back", text)

    def test_a_capture_with_a_carrier_and_no_echoes_says_so(self):
        self.config()
        code, text = self.run_hunt(argv=["--list", self._four(1), "--label", "quiet",
                                         "--seconds", "1"],
                                   firmware=planted_scanner(rule=None))
        self.assertEqual(cfg.EXIT_OK, code, text)
        self.assertIn("no echo", text)

    # ---- the list file this class sends

    def _four(self, how_many=4):
        path = os.path.join(self.tmp, "four-%d.txt" % how_many)
        with open(path, "w", encoding="utf-8") as handle:
            for name, line, _about in FOUR_LINES[:how_many]:
                handle.write("%s = %s\n" % (name, line))
        return path

# ------------------------------------------- the garden: a list as session programs, and no board

# A `scope` that costs a session almost nothing: sixteen points and a whole minute between
# frames, so a step of it brings two lines. It is here so that a test of one rule breaks that one
# rule and no other.
CHEAP = "scope 869 870.2 16 4.8 60000"
# And the longest line a session takes: forty characters exactly, every number and both of phase
# 3.16's task 02's.
FORTY = "scope 869 870.2 16 4.8 60000 max 5.6 500"


class NoBenchIsAsked(object):
    """A bench that cannot be used at all: every attribute of it is an error.

    `--sessions` must open no port and ask tools/bench for nothing (the task's deliverable 1), and
    the strongest way to say that in a test is to hand the tool a bench that explodes on being
    looked at.
    """

    def __getattr__(self, name):
        raise AssertionError("--sessions asked the bench for %r" % name)


class TheSessions(HuntCase):
    """`hunt.py --sessions`: a list as the programs meshmaster.dk and the Walter would take."""

    def _list(self, text, name="list.txt"):
        path = os.path.join(self.tmp, name)
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        return path

    def _sessions(self, text, argv=()):
        path = self._list(text)
        out = io.StringIO()
        code = hunt.main(["--sessions", path] + list(argv), bench=NoBenchIsAsked(), out=out)
        return code, out.getvalue()

    def _garden(self, letter):
        return os.path.join(os.path.dirname(os.path.abspath(hunt.__file__)), "lists",
                            "garden-%s.txt" % letter)

    # ---- the rails

    def test_sessions_opens_no_port_and_asks_the_bench_for_nothing(self):
        before = "serial" in sys.modules
        code, text = self._sessions("one = %s\n" % CHEAP)
        self.assertEqual(cfg.EXIT_OK, code, text)
        if not before:
            self.assertNotIn("serial", sys.modules,
                             "--sessions must not reach anything that opens a port")

    def test_the_five_garden_lists_would_be_taken(self):
        """The lists of tasks/phase-3.16/README.md's *The garden instead of the bench*.

        Six steps is a program's most and four of the five are at it; garden-d is five since
        phase 3.17's task 01, which cost it a step to say `max 32 0` on its reference line.
        garden-e is phase 3.17's task 02's: the two ways to sweep, twice each and then narrow.
        """
        for letter, steps in (("a", 6), ("b", 6), ("c", 6), ("d", 5), ("e", 6)):
            out = io.StringIO()
            code = hunt.main(["--sessions", self._garden(letter)], bench=NoBenchIsAsked(),
                             out=out)
            text = out.getvalue()
            self.assertEqual(cfg.EXIT_OK, code, "garden-%s: %s" % (letter, text))
            self.assertIn("every session of this list would be taken", text)
            self.assertNotIn("REFUSED", text)
            sessions = hunt.read_sessions(self._garden(letter))
            self.assertEqual(1, len(sessions))
            self.assertEqual(steps, len(sessions[0].steps), "garden-%s" % letter)
            program = sessions[0].program().encode("utf-8")
            self.assertLessEqual(len(program), hunt.SESSION_PROGRAM_BYTES_MAX)
            for step in sessions[0].steps:
                self.assertLessEqual(len(step.command), hunt.SESSION_COMMAND_MAX)
                self.assertEqual(85, step.seconds, "85 seconds a step, sized from a 762-byte frame")

    def test_garden_d_has_room_again_and_its_reference_line_says_max_32_0(self):
        """garden-d was 384 bytes of 384 and is 328 since phase 3.17's task 01.

        `max 32 0` is nine characters and the reference line needs them: with 500 us the default,
        the sweep that stands on nothing is the one that has to say so. The room came from the
        100 us step, which was never run, and from the 500 us step becoming the plain line.
        """
        session = hunt.read_sessions(self._garden("d"))[0]
        self.assertEqual(5, len(session.steps))
        self.assertEqual(328, len(session.program().encode("utf-8")))
        self.assertLess(len(session.program().encode("utf-8")),
                        hunt.SESSION_PROGRAM_BYTES_MAX)
        commands = [step.command for step in session.steps]
        self.assertEqual("scope 840 900 0 467 1000 max 32 0", commands[0],
                         "the reference: no stand on either segment's first point")
        self.assertEqual("scope 840 900 0 467 1000", commands[1],
                         "and the same line as a session types it, which stands the default")
        self.assertIn("scope 863 876 250 58.6 1000 max 32 0", commands)
        for command in commands:
            self.assertLessEqual(len(command), hunt.SESSION_COMMAND_MAX, command)
        # The two 60 MHz lines are one plan but for the settle, which is what pairs them.
        first = echoes.parse_scope(commands[0])
        second = echoes.parse_scope(commands[1])
        self.assertEqual(0, first.plan.settle_us)
        self.assertEqual(echoes.SETTLE_DEFAULT_US, second.plan.settle_us)
        self.assertEqual(first.plan.points, second.plan.points)
        self.assertEqual(first.plan.segments, second.plan.segments)

    def test_the_lines_are_not_rewritten_to_a_frame_a_sweep(self):
        """A bench list is forced to interval 0 and `last`; a garden step keeps its own."""
        for letter in ("a", "b", "c", "d"):
            for step in hunt.read_sessions(self._garden(letter))[0].steps:
                self.assertEqual(1000, step.request.interval_ms, step.command)
                self.assertEqual("max", step.request.agg, step.command)

    # ---- every rule of *what a session takes*, broken once

    def test_a_character_a_session_cannot_carry_is_refused_and_named(self):
        code, text = self._sessions("comma = scope 863 876 250 58,6 1000\n")
        self.assertEqual(cfg.EXIT_REFUSED, code, text)
        self.assertIn("it carries ','", text)
        self.assertIn("`a-z`, `0-9`, space and the full stop only", text)

    def test_a_command_of_more_than_forty_characters_is_refused(self):
        long_one = "scope 779.618 959.618 2048 467 1000 max 5.6"
        self.assertEqual(43, len(long_one))
        code, text = self._sessions("long = %s\n" % long_one)
        self.assertEqual(cfg.EXIT_REFUSED, code, text)
        self.assertIn("43 characters, and a step's command is at most 40", text)

    def test_a_first_word_that_is_not_a_scanner_command_is_refused(self):
        """`boost off` is a line a bench list has in the middle of it and a session cannot."""
        code, text = self._sessions("boost-off = boost off\n")
        self.assertEqual(cfg.EXIT_REFUSED, code, text)
        self.assertIn("its first word is 'boost'", text)
        self.assertIn("stat sweep scope hist dwell cad", text)

    def test_a_seventh_step_is_refused(self):
        text = "".join("s%d = %s\n" % (number, CHEAP) for number in range(7))
        code, said = self._sessions(text)
        self.assertEqual(cfg.EXIT_REFUSED, code, said)
        self.assertIn("7 step(s), and a program is 1 to 6", said)

    def test_a_list_with_no_step_in_it_is_refused(self):
        code, text = self._sessions("# nothing but a comment\n\n")
        self.assertEqual(cfg.EXIT_REFUSED, code, text)
        self.assertIn("no step in it", text)

    def test_seconds_outside_five_to_three_hundred_are_refused(self):
        for seconds in (2, 400):
            code, text = self._sessions("seconds = %d\none = %s\n" % (seconds, CHEAP))
            self.assertEqual(cfg.EXIT_REFUSED, code, text)
            self.assertIn("%d seconds, and a step lasts 5 to 300" % seconds, text)

    def test_a_program_of_more_than_six_hundred_seconds_is_refused(self):
        text = "seconds = 150\n" + "".join("s%d = %s\n" % (n, CHEAP) for n in range(5))
        code, said = self._sessions(text)
        self.assertEqual(cfg.EXIT_REFUSED, code, said)
        self.assertIn("750 seconds over 5 step(s), and a program lasts 600 at most", said)

    def test_a_program_of_more_than_384_bytes_is_refused(self):
        self.assertEqual(hunt.SESSION_COMMAND_MAX, len(FORTY))
        text = "".join("s%d = %s\n" % (number, FORTY) for number in range(6))
        code, said = self._sessions(text)
        self.assertEqual(cfg.EXIT_REFUSED, code, said)
        self.assertIn("428 bytes as it goes out, and a program is at most 384", said)

    def test_a_step_whose_frames_would_not_fit_the_walters_bytes_is_refused(self):
        """The README's own 90 seconds of the wide span, which is why the lists say 85."""
        code, text = self._sessions("seconds = 90\nwide = scope 780 960 0 467 1000\n")
        self.assertEqual(cfg.EXIT_REFUSED, code, text)
        self.assertIn("90 frame(s) of 762 bytes is 68580, and the Walter keeps 65536 bytes of "
                      "lines a step", text)
        # And the same line at 85 seconds is 64 770 and is taken.
        code, text = self._sessions("seconds = 85\nwide = scope 780 960 0 467 1000\n")
        self.assertEqual(cfg.EXIT_OK, code, text)
        self.assertIn("64770 of 65536", text)

    def test_the_servers_two_numbers_cannot_be_reached_before_the_walters(self):
        """So no list can break them, and that is why no test breaks them.

        The server keeps 512 kB and 4 000 lines of a session, and the Walter keeps 65 536 bytes of
        a step: six steps of the Walter's most is a third of a megabyte, and six steps of as many
        of the shortest frames as 65 536 bytes hold is 1 452 lines. Both are checked by
        `refusals_of()` all the same - if a Walter one day keeps more of a step, this test is what
        fails first and says why the two checks are there.
        """
        self.assertLess(hunt.SESSION_STEPS_MAX * hunt.WALTER_STEP_BYTES,
                        hunt.SESSION_SERVER_BYTES)
        shortest = echoes.frame_bytes(echoes.MIN_POINTS)
        self.assertEqual(270, shortest, "sixteen points is the fewest a `scope` has")
        self.assertLess(hunt.SESSION_STEPS_MAX * (hunt.WALTER_STEP_BYTES // shortest),
                        hunt.SESSION_SERVER_LINES)

    def test_a_step_that_asks_for_a_frame_per_sweep_is_refused_by_the_same_rule(self):
        """`interval 0` is what a bench list types; the Walter's UART would cut such a step off."""
        code, text = self._sessions("wide = scope 780 960 0 467 0\n")
        self.assertEqual(cfg.EXIT_REFUSED, code, text)
        self.assertIn("the Walter keeps 65536 bytes", text)

    # ---- the list, and what is printed

    def test_a_list_of_two_sessions_is_two_programs_and_the_seconds_line_sets_the_steps(self):
        code, text = self._sessions(
            "# the first\n"
            "--- one\n"
            "seconds = 20\n"
            "a = %s\n"
            "# the second\n"
            "--- two\n"
            "seconds = 30\n"
            "b = %s\n" % (CHEAP, CHEAP))
        self.assertEqual(cfg.EXIT_OK, code, text)
        self.assertIn("20 %s" % CHEAP, text)
        self.assertIn("30 %s" % CHEAP, text)
        sessions = hunt.read_sessions(self._list(
            "--- one\nseconds = 20\na = %s\n--- two\nseconds = 30\nb = %s\n"
            % (CHEAP, CHEAP), name="two.txt"))
        self.assertEqual(["one", "two"], [one.name for one in sessions])
        self.assertEqual([20], [step.seconds for step in sessions[0].steps])
        self.assertEqual([30], [step.seconds for step in sessions[1].steps])

    def test_a_list_with_no_session_mark_is_one_session_named_after_the_file(self):
        sessions = hunt.read_sessions(self._list("a = %s\n" % CHEAP, name="unmarked.txt"))
        self.assertEqual(["unmarked"], [one.name for one in sessions])

    def test_eighty_five_seconds_a_step_when_the_list_does_not_say(self):
        sessions = hunt.read_sessions(self._list("a = %s\n" % CHEAP, name="default.txt"))
        self.assertEqual(hunt.SESSION_SECONDS_DEFAULT, sessions[0].steps[0].seconds)

    def test_what_is_printed_is_what_the_form_takes_and_what_it_costs(self):
        out = io.StringIO()
        code = hunt.main(["--sessions", self._garden("b")], bench=NoBenchIsAsked(), out=out)
        text = out.getvalue()
        self.assertEqual(cfg.EXIT_OK, code, text)
        # The lines to paste, the seconds and the command, as meshmaster.dk's form takes them.
        self.assertIn("85 scope 780 960 0 467 1000 ldo", text)
        # The program as it goes out, byte for byte, and the bytes it is.
        self.assertIn('{"slot":0,"steps":[{"command":"scope 780 960 0 467 1000","seconds":85}',
                      text)
        self.assertIn("358 of 384 bytes as it goes out", text)
        # What each step is expected to bring, and how long Bellevue is off the mesh.
        self.assertIn("64770 of 65536", text)
        self.assertIn("Bellevue off the mesh: about 27 minutes", text)

    def test_the_same_as_json(self):
        out = io.StringIO()
        code = hunt.main(["--sessions", self._garden("c"), "--json"], bench=NoBenchIsAsked(),
                         out=out)
        self.assertEqual(cfg.EXIT_OK, code, out.getvalue())
        parsed = json.loads(out.getvalue())
        self.assertTrue(parsed["taken"])
        self.assertEqual(1, len(parsed["sessions"]))
        session = parsed["sessions"][0]
        self.assertEqual("garden-c", session["name"])
        self.assertEqual(6, len(session["steps"]))
        self.assertEqual(510, session["seconds"])
        self.assertEqual([], session["refusals"])
        self.assertEqual(1590, session["off_mesh_seconds"])
        first = session["steps"][0]
        self.assertEqual(129, first["points"])
        self.assertEqual(2, first["segments"])
        self.assertEqual(85, first["frames"])
        self.assertEqual(418, first["frame_bytes"])
        self.assertEqual(85 * 418, first["bytes_back"])
        self.assertEqual("85 scope 840 900 0 467 1000", first["typed"])

    def test_a_refused_list_as_json_is_exit_2_and_names_the_rule(self):
        path = self._list("seconds = 90\nwide = scope 780 960 0 467 1000\n")
        out = io.StringIO()
        code = hunt.main(["--sessions", path, "--json"], bench=NoBenchIsAsked(), out=out)
        self.assertEqual(cfg.EXIT_REFUSED, code)
        parsed = json.loads(out.getvalue())
        self.assertFalse(parsed["taken"])
        self.assertEqual(1, len(parsed["sessions"][0]["refusals"]))
        self.assertIn("65536", parsed["sessions"][0]["refusals"][0])

if __name__ == "__main__":
    unittest.main()
