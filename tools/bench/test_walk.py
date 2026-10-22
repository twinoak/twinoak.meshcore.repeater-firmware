#!/usr/bin/env python3
"""Every walk of docs/bench.md, run end to end with no board at all.

    python tools/bench/test_walk.py

tools/verify.ps1's release step runs these beside tools/bench/test_bench.py (selfcheck.py), so
CI - which has no board and needs none - proves on every commit that each walk runs through to
its result file, that what it says about docs/bench.md is true of the page as committed, and that
no password reaches a transcript, a file or the follow-ups.

The board is tools/bench/fakes.py: a fake serial layer that records every open, a fake repeater
console with upstream's two `sensor list` pages and a first boot after an `erase`, a fake scanner
that answers the page's own lines, and a runner that runs nothing. The repository the walks read
is a temporary one with the real platformio.ini and the real docs/bench.md copied in, so the
band's numbers and the page's bullets are the committed ones and not a copy of them here.

What is held:
  * the page: every line of every walk is a bullet that is in docs/bench.md, exactly once, and
    every bullet of a walked section is a step of its walk - a page that moves fails here;
  * section 1: the console's eight lines, the blanks filled, and the five lines that are a
    person's said to be;
  * section 3: the clock on a board with no RTC - `unset` and upstream's 2024 date, a
    `time <epoch>` set, `clock sync`, the `restored` after a `reboot`, the `clkreboot` that
    cannot put the node behind itself, and `rtc` never said - with the seven lines that need the
    link, the switch, the power, a companion or a part nobody has said to be a person's;
  * section 5: `power.low_mv` set to 3600, listed, still 3600 after a restart, `3350`, `2400`
    and `4400` refused with nothing changed, and both thresholds put back as they were found;
  * section 8: the descriptor, the flash, `hello` over the link (and skipped without a dongle),
    and the restore that closes the run;
  * section 9: the round trip - the `boot` line, `sweep` at 281 points, `hist`, `dwell`, `cad`,
    `stop` mid-sweep, a minute of output with its heartbeats, the `rom` gate, `scope`, the
    dummy-load baseline on disk - and the repeater back with its identity;
  * section 10: refused unless the identity is disposable and refused while a run is open; then
    the erase, both `First boot:` lines, `done` and `never`, the provisioning at the console and
    over the link, and the password nowhere;
  * a board that answers differently: exit 1, a *differs* in the result file, and an item for the
    follow-ups of the task that wrote the line - never a fix;
  * `--tick`: the bullets that came back ok ticked `(bench, <date>, <commit>)` in the page, twice
    without harm, and nothing else touched.
"""

import io
import json
import os
import re
import shutil
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
for _p in (HERE, os.path.join(REPO, "spec", "tools"), os.path.join(REPO, "tools", "release")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import bench as benchcli  # noqa: E402
import config as cfg  # noqa: E402
import walk  # noqa: E402
from fakes import (FakePorts, FakeRepeaterFirmware, FakeRunner, FakeScannerFirmware)  # noqa: E402
from lock import BoardLock  # noqa: E402
from ports import PortInfo  # noqa: E402
from runs import RunRecord, load_run, save_run  # noqa: E402
from test_bench import BenchCase, REPEATER_ID, SERIAL, firmware_for  # noqa: E402

COMMIT = "0123456789abcdef0123456789abcdef01234567"
# What `time.time()` is pretended to be while section 3 is walked: 14 October 2026, 17:46:40 UTC.
# The walk sets the node's clock to this machine's time, so a fixed one makes what the board says
# afterwards something a test can hold exactly (upstream prints the minute and not the second).
WALK_NOW = 1792000000
PASSWORD = "a-password-nobody-keeps"
DONGLE = "A50285BI"

HELLO = """protocol 1.0
role 1 (repeater)
slot 0
max_payload 240
caps 0x61
boot_id 7
fw_version 0.0.0-dev
board rak4630-single
upstream repeater-v1.17.1+d9296435
"""

PROVISIONED = """provision unit.txt: 8 line(s)
  2:   set name bench-walk
       -> OK
  8:   region default dk
       ->  default scope is now dk
  11:  password ****
       -> accepted (the reply to a password line is not printed)
every line was taken
"""


class WalkCase(BenchCase):
    """A temporary repository with the real platformio.ini and docs/bench.md in it, so that a
    walk reads the band's numbers and the page's bullets as they are committed."""

    def setUp(self):
        super().setUp()
        shutil.copy(os.path.join(REPO, "platformio.ini"), os.path.join(self.repo, "platformio.ini"))
        os.makedirs(os.path.join(self.repo, "docs"), exist_ok=True)
        with open(os.path.join(REPO, "docs", "bench.md"), encoding="utf-8") as fh:
            page = fh.read()
        # The page's own bullets, with every tick taken off the copy: a walk on a board ticks
        # lines in the committed page (p3.4-02 ticked the first of them), and these tests are
        # about what a walk does to a line that is not ticked yet. The text of the bullets - what
        # the walk's anchors are held to, both ways - is the committed one.
        page = re.sub(r"^- \[x\] ", "- [ ] ", page, flags=re.M)
        page = re.sub(r" \(bench, \d{4}-\d\d-\d\d, [0-9a-f]{7}\)", "", page)
        with open(os.path.join(self.repo, "docs", "bench.md"), "w", encoding="utf-8",
                  newline="\n") as fh:
            fh.write(page)
        self.config()
        self.repeater_zip = self.package()
        self.scanner_zip = self.package(role="scanner", where="scanner_rak4630_single")

    # ---- the rig

    def fakes(self, board=None, others=None, **kw):
        board = board or self.board()
        ports = FakePorts(boards=[board], clock=self.clock, others=others)
        runner = FakeRunner(boards=[board], new_firmware=firmware_for, **kw)
        return board, ports, runner

    def walk(self, argv, ports=None, runner=None, probe=None, region=None, commit=COMMIT,
             now_epoch=WALK_NOW):
        bench = self.bench(ports=ports, runner=runner, probe=probe, region=region)
        bench.commit = commit
        bench.now_epoch = (lambda: now_epoch)
        code = benchcli.main(argv, bench=bench)
        return code, self.out.getvalue()

    def result_file(self):
        out_dir = os.path.join(self.repo, "out", "bench")
        found = sorted(name for name in os.listdir(out_dir) if name.startswith("walk-"))
        self.assertEqual(len(found), 1, found)
        with open(os.path.join(out_dir, found[0]), "r", encoding="utf-8") as fh:
            return found[0], fh.read()

    def page(self):
        with open(os.path.join(self.repo, "docs", "bench.md"), "r", encoding="utf-8") as fh:
            return fh.read()

    def phase_followups(self, phase, text="# Phase %s\n\nNothing yet.\n"):
        path = os.path.join(self.repo, "tasks", "phase-%s" % phase, "FOLLOWUPS.md")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text % phase if "%s" in text else text)
        return path

    # ---- what every walk must look like afterwards

    def assertEveryLineHasAStep(self, section, text):
        for line in walk.SECTIONS[section].lines:
            self.assertIn("### %s " % line.key, text,
                          "docs/bench.md section %s line %s is in no step of the result file"
                          % (section, line.key))

    def assertNoPassword(self, text):
        self.assertNotIn(PASSWORD, text)
        self.assertNotIn("not-a-password", text)
        self.assertFalse(re.search(r"(?i)password now:\s*\S", text.replace(
            walk.consolemod.PASSWORD_ECHO_SHOWN, "")), text[:2000])


# ----------------------------------------------------------------------------- the page itself

class ThePage(unittest.TestCase):
    """What the walks say about docs/bench.md, held against the page as committed."""

    @classmethod
    def setUpClass(cls):
        with open(os.path.join(REPO, "docs", "bench.md"), "r", encoding="utf-8") as fh:
            cls.text = fh.read()

    def test_the_walkable_sections_are_the_six_the_page_names(self):
        """p3.3-03 wrote four of them; p3.5-04 added §3 and §5, the console's half of the clock
        and of the thresholds."""
        self.assertEqual(walk.WALKABLE, ("1", "3", "5", "8", "9", "10"))

    def test_every_anchor_is_a_bullet_of_the_page_exactly_once(self):
        for number in walk.WALKABLE:
            for line in walk.SECTIONS[number].lines:
                hits = self.text.count(line.anchor)
                self.assertEqual(hits, 1, "section %s line %s: %d hits for %r"
                                 % (number, line.key, hits, line.anchor))
                bounds = walk.bullet_bounds(self.text, line.anchor)
                self.assertIsNotNone(bounds, line.key)
                bullet = self.text[bounds[0]:bounds[1]]
                self.assertTrue(bullet.startswith("- [ ] ") or bullet.startswith("- [x] "),
                                "%s is not a tickable bullet: %r" % (line.key, bullet[:40]))
                head = bullet.split("\n")[0]
                for task in line.tasks:
                    self.assertIn(task, head,
                                  "%s: the bullet does not name %s" % (line.key, task))

    def test_every_bullet_of_a_walked_section_is_a_line_of_its_walk(self):
        """The other way round: a bullet added to §1, §8, §9 or §10 and not to a walk fails."""
        body = self.text
        sections = dict()
        for match in re.finditer(r"(?m)^## (\d+(?:\.\d+)?)\. ", body):
            number = match.group(1)
            end = body.find("\n## ", match.end())
            sections[number] = body[match.start():end if end > 0 else len(body)]
        for number in walk.WALKABLE:
            self.assertIn(number, sections, "docs/bench.md has no section %s" % number)
            bullets = re.findall(r"(?m)^- \[[ x]\] (.*)$", sections[number])
            anchors = [line.anchor for line in walk.SECTIONS[number].lines]
            self.assertEqual(len(bullets), len(anchors),
                             "section %s has %d bullets and the walk has %d lines"
                             % (number, len(bullets), len(anchors)))

    def test_a_page_whose_bullet_moved_ticks_nothing_rather_than_the_wrong_thing(self):
        result = walk.WalkResult(walk.SECTIONS["1"], None, COMMIT, "2026-10-18T00:00:00Z")
        line = walk.SECTIONS["1"].line("1.1")
        result.add(walk.Step(line, "a step", walk.OK))
        text, ticked, already, missing = walk.tick_page("# a page with no such line\n", result)
        self.assertEqual((ticked, already, missing), ([], [], ["1.1"]))
        self.assertEqual(text, "# a page with no such line\n")

    def test_a_line_that_is_already_ticked_is_said_to_be_ticked_already(self):
        """Not *not found in docs/bench.md*, which is what it used to say: the page did not
        lose the line, it has it already (tasks/phase-3.4/FOLLOWUPS.md, p3.5-01)."""
        result = walk.WalkResult(walk.SECTIONS["1"], None, COMMIT, "2026-10-18T00:00:00Z")
        line = walk.SECTIONS["1"].line("1.1")
        result.add(walk.Step(line, "a step", walk.OK))
        page = "## 1\n\n- [x] %s (bench, 2026-10-22, 9fb721b)\n" % line.anchor
        text, ticked, already, missing = walk.tick_page(page, result)
        self.assertEqual((ticked, already, missing), ([], ["1.1"], []))
        self.assertEqual(text, page)

    def test_the_head_table_says_which_sections_a_walk_covers(self):
        head = self.text.split("## 0.")[0]
        self.assertIn("bench walk", head)
        for number in walk.WALKABLE:
            self.assertIn("§%s" % number, head)


# ----------------------------------------------------------------------------- no bench, no walk

class NoBench(WalkCase):
    def test_no_board_is_exit_three_and_nothing_opened(self):
        for section in walk.WALKABLE:
            self.out = io.StringIO()
            code, out = self.walk(["walk", section])
            self.assertEqual(code, cfg.EXIT_NO_BENCH, out)
            self.assertIn("no bench", out)
            self.assertNothingOpened()

    def test_no_config_is_exit_three(self):
        os.remove(os.path.join(self.repo, cfg.BENCH_FILE))
        code, out = self.walk(["walk", "1"])
        self.assertEqual(code, cfg.EXIT_NO_BENCH)
        self.assertNothingOpened()

    def test_a_section_with_no_walk_is_refused(self):
        _board, ports, runner = self.fakes()
        code, out = self.walk(["walk", "4"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_REFUSED)
        self.assertIn("there is no walk for section 4", out)
        self.assertEqual(self.ports.opened, [])

    def test_a_board_held_by_another_process_is_exit_four(self):
        _board, ports, runner = self.fakes()
        from fakes import FakeProbe
        probe = FakeProbe({os.getpid(): 1.0, 4242: 500.0})
        BoardLock(SERIAL, "a person's terminal", probe=probe, home=self.home).acquire()
        with open(os.path.join(self.home, "locks", "%s.lock" % SERIAL), "w") as fh:
            json.dump({"pid": 4242, "process_started": 500.0, "command": "console",
                       "taken": "now", "host": "elsewhere"}, fh)
        code, out = self.walk(["walk", "1"], ports=ports, runner=runner, probe=probe)
        self.assertEqual(code, cfg.EXIT_LOCKED, out)
        self.assertIn("one user at a time", out)


# ----------------------------------------------------------------------------- section 1

class SectionOne(WalkCase):
    def test_a_board_that_prints_every_digit_of_the_frequency_is_still_the_band(self):
        """What the first real board answered (p3.4-02): `869.6179809,62.5,8,8` is the band's
        `869.618,62.5,8,8` in the float upstream keeps - not a differs."""
        board, ports, runner = self.fakes(self.board(
            FakeRepeaterFirmware(REPEATER_ID, answers={"get radio": "> 869.6179809,62.5,8,8"})))
        code, out = self.walk(["walk", "1"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        _name, text = self.result_file()
        self.assertNotIn("| differs |", text)
        self.assertIn("**section 1 get radio** -> 869.6179809,62.5,8,8", text)

    def test_a_board_on_another_frequency_is_a_differs(self):
        board, ports, runner = self.fakes(self.board(
            FakeRepeaterFirmware(REPEATER_ID, answers={"get radio": "> 868.0,62.5,8,8"})))
        code, out = self.walk(["walk", "1"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_FAILED, out)
        _name, text = self.result_file()
        self.assertIn("| differs |", text)

    def test_the_console_lines_of_section_one(self):
        board, ports, runner = self.fakes()
        code, out = self.walk(["walk", "1"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        name, text = self.result_file()
        self.assertTrue(name.startswith("walk-1-"), name)
        self.assertIn("0123456", name)                       # the commit is in the file's name
        self.assertEveryLineHasAStep("1", text)
        # the eight lines the task names, each one ok
        for sent in ("ver", "board", "get radio", "sensor list", "sensor list 5",
                     "sensor get fw.version", "sensor set fw.version x", "get dutycycle"):
            self.assertIn("sent: `%s`" % sent, text)
        self.assertIn("Repeater ID: %s" % REPEATER_ID, text)
        self.assertNotIn("| differs |", text)
        # the blanks, filled in from the board and from platformio.ini
        self.assertIn("**section 1 get radio** -> 869.618,62.5,8,8", text)
        self.assertIn("**section 1 get dutycycle** -> 10.0%", text)
        self.assertIn("**section 1 board** -> RAK 4631", text)
        self.assertIn("**section 1 application serial number** -> %s" % SERIAL, text)
        # the identity came off `get public.key`, so section 1 resets nothing at all (p3.4-02)
        self.assertEqual(board.firmware.rebooted, 0)
        self.assertIn("get public.key", board.firmware.received)
        self.assertEqual(board.flashes, [])
        self.assertIsNone(load_run(self.home, SERIAL))        # section 1 opens no run
        self.assertNoPassword(text)
        self.assertNoPassword(out)

    def test_the_sensor_list_paging_is_what_the_page_says(self):
        _board, ports, runner = self.fakes()
        code, out = self.walk(["walk", "1"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        _name, text = self.result_file()
        self.assertIn("characters, keys fw.version,walter.wake,link.errors,walter.seen,"
                      "clock.state, `... next:` with the digit cut", text)
        self.assertIn("**section 1 sensor list 5** -> power.low_mv,power.crit_mv,obs.wake,"
                      "client.refused,firstboot.state", text)

    def test_the_five_lines_that_need_a_person_say_what_they_need(self):
        _board, ports, runner = self.fakes()
        self.walk(["walk", "1"], ports=ports, runner=runner)
        _name, text = self.result_file()
        still = text.split("## Still a person's")[1]
        for owed in ("roof antenna", "phone on the mesh", "INFO_UF2.TXT", "scope probe",
                     "double-tap reset"):
            self.assertIn(owed, still)
        self.assertIn("`bench console \"password x\"` was refused here with nothing opened", text)

    def test_a_board_that_answers_differently_is_exit_one_and_a_follow_up(self):
        firmware = FakeRepeaterFirmware(REPEATER_ID, answers={"get radio": "> 868.000,250.0,7,5"})
        board, ports, runner = self.fakes(self.board(firmware))
        self.phase_followups("1")
        code, out = self.walk(["walk", "1", "--tick"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_FAILED, out)
        _name, text = self.result_file()
        self.assertIn("## What differs", text)
        self.assertIn("868.000,250.0,7,5", text)
        # the line is p1-01's, so the item goes to phase 1's follow-ups and the bullet is not ticked
        with open(os.path.join(self.repo, "tasks", "phase-1", "FOLLOWUPS.md")) as fh:
            followups = fh.read()
        self.assertIn("**[01] `docs/bench.md` 1.1 differs from the board**", followups)
        self.assertIn("a bench walk of", followups)
        bounds = walk.bullet_bounds(self.page(), walk.SECTIONS["1"].line("1.1").anchor)
        self.assertTrue(self.page()[bounds[0]:].startswith("- [ ] "))

    def test_a_board_in_its_bootloader_is_refused_with_the_reason(self):
        board, ports, runner = self.fakes(self.board(mode="bootloader"))
        code, out = self.walk(["walk", "1"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_REFUSED, out)
        self.assertIn("is in its bootloader", out)


# ----------------------------------------------------------------------------- section 3

class SectionThree(WalkCase):
    """docs/bench.md section 3's console lines, on a fake board whose clock keeper is the
    firmware's own rules (fakes.py: the file, the `clean` flag, the save interval)."""

    def fresh(self, **kw):
        """A node whose clock has never been set: `unset`, and upstream's 2024 date."""
        return FakeRepeaterFirmware(REPEATER_ID, **kw)

    def walk_three(self, firmware=None, **kw):
        board, ports, runner = self.fakes(self.board(firmware or self.fresh()))
        code, out = self.walk(["walk", "3"], ports=ports, runner=runner, **kw)
        return board, code, out

    def test_the_console_lines_of_the_clock(self):
        """Every line of the section a console can read, with `clock sync` given a sender
        timestamp - which is the only way upstream's own line can move a clock at all
        (CommonCLI.cpp:199-207), and so the walk's own ok path."""
        board, code, out = self.walk_three(self.fresh(clock_sync_sender=WALK_NOW + 5))
        self.assertEqual(code, cfg.EXIT_OK, out)
        name, text = self.result_file()
        self.assertTrue(name.startswith("walk-3-"), name)
        self.assertEveryLineHasAStep("3", text)
        self.assertNotIn("| differs |", text)
        for sent in ("sensor get clock.state", "clock", "time %d" % WALK_NOW, "clock sync",
                     "clkreboot"):
            self.assertIn("sent: `%s`" % sent, text)
        # the blanks: `unset` and 2024 before anything, `set` after the set, `restored` after
        # each of the two resets - and the clkreboot's margin is the keeper's save interval
        self.assertIn("**section 3 clock.state, before any set** -> unset", text)
        self.assertIn("**section 3 clock, before any set** -> 10:52 - 15/5/2024 UTC", text)
        self.assertIn("**section 3 clock.state, after the set** -> set", text)
        self.assertIn("**section 3 after a reboot** -> restored, 17:46 - 14/10/2026 UTC "
                      "(-40 s from the set)", text)
        self.assertIn("**section 3 after a clkreboot** -> restored, 18:46 - 14/10/2026 UTC "
                      "(+3600 s ahead of where the set left it)", text)
        self.assertIn("**section 3 the clock.state words read** -> "
                      "unset, set, set, restored, restored", text)
        # two resets, both of them upstream's own console lines, and no flash and no run
        self.assertEqual(board.firmware.rebooted, 2)
        self.assertEqual(board.flashes, [])
        self.assertIsNone(load_run(self.home, SERIAL))
        self.assertNoPassword(text)

    def test_clock_sync_at_the_usb_console_is_a_differs_for_p1_04(self):
        """What the pinned firmware really does at this port: ours calls `handleCommand(0, ...)`
        (src/roles/repeater/main.cpp), so `clock sync` has no sender timestamp to use and can
        only refuse. The page's line is p1-04's, so that is a *differs* and a follow-up - and
        every other line of the section is still walked and ticked."""
        self.phase_followups("1")
        _board, code, out = self.walk_three()
        self.assertEqual(code, cfg.EXIT_FAILED, out)
        _name, text = self.result_file()
        self.assertIn("ERR: clock cannot go backwards", text)
        self.assertIn("plus one and refuses when that is not ahead", text)
        differs = text.split("## What differs")[1]
        self.assertIn("3.9", differs)
        self.assertNotIn("3.7", differs)
        self.assertNotIn("3.10", differs)

    def test_a_differs_is_a_follow_up_for_p1_04_and_the_rest_is_ticked(self):
        self.phase_followups("1")
        _board, code, out = self.walk_three(now_epoch=WALK_NOW)
        self.assertEqual(code, cfg.EXIT_FAILED, out)
        code, out = self.walk(["walk", "3", "--tick"],
                              *self.fakes(self.board(self.fresh()))[1:])
        with open(os.path.join(self.repo, "tasks", "phase-1", "FOLLOWUPS.md")) as fh:
            followups = fh.read()
        self.assertIn("**[04] `docs/bench.md` 3.9 differs from the board**", followups)
        page = self.page()
        for key in ("3.7", "3.8", "3.10", "3.11", "3.12"):
            bounds = walk.bullet_bounds(page, walk.SECTIONS["3"].line(key).anchor)
            self.assertTrue(page[bounds[0]:].startswith("- [x] "), key)
        bounds = walk.bullet_bounds(page, walk.SECTIONS["3"].line("3.9").anchor)
        self.assertTrue(page[bounds[0]:].startswith("- [ ] "))       # the differs is not ticked

    def test_a_node_whose_clock_has_been_set_before_skips_rather_than_differs(self):
        """A second walk of this section, an hour after the first one's `clkreboot`: the node is
        no longer the node the first lines are about, and the walk says so rather than calling
        the board wrong (p3.5-01's rule for a line already walked)."""
        ahead = WALK_NOW + 7200
        firmware = self.fresh(clock_state="restored", epoch=ahead, clock_file=(ahead, False))
        board, code, out = self.walk_three(firmware)
        self.assertEqual(code, cfg.EXIT_OK, out)
        _name, text = self.result_file()
        self.assertNotIn("| differs |", text)
        self.assertIn("this node's clock has been set before", text)
        self.assertIn("already 7160 s ahead of this machine's", text)   # the minute is cut
        # the clkreboot line is still walked, and the node is still not put behind itself
        self.assertIn("(+3600 s ahead of where the set left it)", text)
        self.assertEqual(board.firmware.rebooted, 1)          # the `reboot` line was skipped

    def test_a_board_with_no_keeper_comes_back_in_2024_and_that_is_a_differs(self):
        """The whole of why section 3 exists: a reset that lands on upstream's volatile clock
        again is a node every companion that heard its earlier adverts would drop."""
        board, code, out = self.walk_three(self.fresh(forgets_clock=True))
        self.assertEqual(code, cfg.EXIT_FAILED, out)
        _name, text = self.result_file()
        differs = text.split("## What differs")[1]
        self.assertIn("3.10", differs)
        self.assertIn("15/5/2024", differs)
        self.assertEqual(board.firmware.clock_state, "unset")

    def test_a_board_that_says_rtc_with_no_rtc_fitted_is_a_differs(self):
        """U5 is DNP on this batch, so `rtc` is not an answer this board can honestly give."""
        firmware = self.fresh(clock_state="rtc", epoch=WALK_NOW - 600, rtc_epoch=WALK_NOW - 600)
        _board, code, out = self.walk_three(firmware)
        self.assertEqual(code, cfg.EXIT_FAILED, out)
        _name, text = self.result_file()
        differs = text.split("## What differs")[1]
        self.assertIn("3.12", differs)
        self.assertIn("never said `rtc`", text)

    def test_the_seven_lines_that_need_a_person_say_what_they_need(self):
        self.walk_three(self.fresh(clock_sync_sender=WALK_NOW + 5))
        _name, text = self.result_file()
        still = text.split("## Still a person's")[1]
        for owed in ("dongle on the link pins", "reset switch", "lab supply's switch",
                     "cannot hear a radio", "part nobody on this bench has"):
            self.assertIn(owed, still)
        self.assertEqual(still.count("- **3."), 7)

    def test_a_scanner_on_the_board_is_refused(self):
        _board, code, out = self.walk_three(FakeScannerFirmware())
        self.assertEqual(code, cfg.EXIT_REFUSED, out)
        self.assertIn("nothing a repeater says", out)

    def test_a_board_in_its_bootloader_is_refused_with_the_reason(self):
        board, ports, runner = self.fakes(self.board(mode="bootloader"))
        code, out = self.walk(["walk", "3"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_REFUSED, out)
        self.assertIn("there is no console to walk section 3 with", out)


# ----------------------------------------------------------------------------- section 5

class SectionFive(WalkCase):
    """docs/bench.md section 5's two threshold lines, over the fake console. The fake refuses a
    pair with fakes.thresholds_acceptable, which is src/common/power/BatteryWatch.cpp's own
    rule."""

    def walk_five(self, firmware=None, **kw):
        board, ports, runner = self.fakes(self.board(firmware
                                                     or FakeRepeaterFirmware(REPEATER_ID)))
        code, out = self.walk(["walk", "5"], ports=ports, runner=runner, **kw)
        return board, code, out

    def test_the_thresholds_are_set_listed_and_still_there_after_a_restart(self):
        board, code, out = self.walk_five()
        self.assertEqual(code, cfg.EXIT_OK, out)
        name, text = self.result_file()
        self.assertTrue(name.startswith("walk-5-"), name)
        self.assertEveryLineHasAStep("5", text)
        self.assertNotIn("| differs |", text)
        for sent in ("sensor set power.low_mv 3600", "sensor list 5", "reboot",
                     "sensor get power.low_mv"):
            self.assertIn("sent: `%s`" % sent, text)
        self.assertIn("power.low_mv=3600", text)
        self.assertIn("**section 5 power.low_mv after the restart** -> 3600", text)
        self.assertEqual(board.firmware.rebooted, 1)
        self.assertEqual(board.flashes, [])
        self.assertIsNone(load_run(self.home, SERIAL))

    def test_the_three_refusals_are_upstreams_one_word_and_change_nothing(self):
        _board, code, out = self.walk_five()
        self.assertEqual(code, cfg.EXIT_OK, out)
        _name, text = self.result_file()
        for mv in (3350, 2400, 4400):
            self.assertIn("**section 5 the refusal of %d** -> can't find custom var" % mv, text)
            self.assertIn("`sensor set power.low_mv %d` is refused" % mv, text)
        self.assertIn("(`power.low_mv` is still 3600)", text)

    def test_the_thresholds_are_put_back_as_they_were_found(self):
        """And a node whose `crit` is not the page's 3300 does not have the page's three
        refusals: `3350` above a `crit` of 3200 is a pair this firmware accepts, so the line is
        skipped with that reason rather than failed."""
        firmware = FakeRepeaterFirmware(REPEATER_ID, low_mv=3400, crit_mv=3200)
        board, code, out = self.walk_five(firmware)
        self.assertEqual(code, cfg.EXIT_OK, out)
        self.assertEqual((board.firmware.low_mv, board.firmware.crit_mv), (3400, 3200))
        _name, text = self.result_file()
        self.assertIn("this node's `power.crit_mv` is 3200 and the page's three values", text)
        self.assertIn("Found `power.low_mv` 3400 and `power.crit_mv` 3200", text)
        self.assertIn("**section 5 the thresholds this walk found, and left** -> "
                      "power.low_mv 3400, power.crit_mv 3200", text)
        # and the walk's last step on the console is that they are back (the task: "its last
        # line says so")
        steps = [line for line in out.splitlines()
                 if re.match(r"^  (ok|differs|needs a person|skipped)\s", line)]
        self.assertIn("back at the values this walk found", steps[-1])

    def test_a_board_that_forgets_the_threshold_after_a_restart_is_a_differs(self):
        self.phase_followups("1")
        board, ports, runner = self.fakes(self.board(
            FakeRepeaterFirmware(REPEATER_ID, forgets_thresholds=True)))
        code, out = self.walk(["walk", "5", "--tick"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_FAILED, out)
        _name, text = self.result_file()
        differs = text.split("## What differs")[1]
        self.assertIn("5.5", differs)
        with open(os.path.join(self.repo, "tasks", "phase-1", "FOLLOWUPS.md")) as fh:
            self.assertIn("**[06] `docs/bench.md` 5.5 differs from the board**", fh.read())

    def test_a_board_that_takes_a_pair_it_should_refuse_is_a_differs(self):
        firmware = FakeRepeaterFirmware(REPEATER_ID,
                                        answers={"sensor set power.low_mv 3350": "ok"})
        _board, code, out = self.walk_five(firmware)
        self.assertEqual(code, cfg.EXIT_FAILED, out)
        _name, text = self.result_file()
        differs = text.split("## What differs")[1]
        self.assertIn("5.6", differs)

    def test_the_paging_line_is_skipped_because_walk_one_reads_it(self):
        _board, code, out = self.walk_five()
        _name, text = self.result_file()
        self.assertIn("`bench walk 1` reads the same two pages at the same console", text)

    def test_the_lines_that_need_a_person_say_what_they_need_and_the_blanks_are_filled(self):
        _board, code, out = self.walk_five()
        _name, text = self.result_file()
        still = text.split("## Still a person's")[1]
        for owed in ("DMM in series with the panel lead", "phone on the mesh",
                     "thermometer in the enclosure", "lab supply's knob",
                     "hand on the I2C connector"):
            self.assertIn(owed, still)
        self.assertEqual(still.count("- **5."), 6)
        # the boot-voltage line is the lab supply's, but its three `get`s are read here
        self.assertIn("**section 5 get pwrmgt.bootmv** -> 0 mV", text)
        self.assertIn("**section 5 get pwrmgt.support** -> supported", text)
        self.assertIn("**section 5 get pwrmgt.bootreason** -> Reset: SREQ; Shutdown: None", text)

    def test_a_scanner_on_the_board_is_refused(self):
        _board, code, out = self.walk_five(FakeScannerFirmware())
        self.assertEqual(code, cfg.EXIT_REFUSED, out)
        self.assertIn("nothing a repeater says", out)


# ----------------------------------------------------------------------------- section 8

class SectionEight(WalkCase):
    def test_the_descriptor_the_flash_and_the_restore(self):
        board, ports, runner = self.fakes()
        code, out = self.walk(["walk", "8"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        _name, text = self.result_file()
        self.assertEveryLineHasAStep("8", text)
        self.assertIn("**section 8 role** -> repeater", text)
        self.assertIn("**section 8 board** -> rak4630-single", text)
        self.assertIn("**section 8 band** -> 868", text)
        self.assertIn("**section 8 version** -> 0.0.0-dev", text)
        self.assertRegex(text, r"\*\*section 8 offset\*\* -> 0x[0-9A-F]+")
        self.assertIn("the board says repeater 0.0.0-dev", text)
        # the image went on, and the run the walk opened was closed by the restore
        self.assertEqual(board.flashes, [self.repeater_zip, self.repeater_zip])
        self.assertIn("restored: repeater back on the board", text)
        self.assertIsNone(load_run(self.home, SERIAL))
        self.assertNotIn("| differs |", text)

    def test_a_new_unit_is_walked_and_restored_rather_than_left_differing(self):
        """A board with its base and no application - how every new unit arrives - walked through
        section 8: the run records the image the walk is about to write, so the restore at the end
        is a step that passed and not the *differs* the first board gave (p3.4-02, p3.5-01)."""
        board, ports, runner = self.fakes(self.board(mode="bootloader"))
        code, out = self.walk(["walk", "8"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        _name, text = self.result_file()
        # the walk's own flash found the bootloader and touched nothing; the one touch is the
        # restore's, from the application the walk had just given the board
        self.assertIn("the board is already in its bootloader", out)
        self.assertEqual(board.touches, 1)
        self.assertEqual(board.flashes, [self.repeater_zip, self.repeater_zip])
        self.assertIn("the board arrived with only its bootloader; restored: the image the run "
                      "began with", text)
        self.assertNotIn("RESTORE NOT POSSIBLE", text)
        self.assertNotIn("| differs |", text)
        self.assertIsNone(load_run(self.home, SERIAL))

    def test_hello_is_skipped_without_a_dongle_and_walked_with_one(self):
        _board, ports, runner = self.fakes()
        self.walk(["walk", "8"], ports=ports, runner=runner)
        _name, text = self.result_file()
        self.assertIn("no `dongle_serial` in bench.local.json", text)

        self.setUp()
        self.config(dongle=DONGLE)
        dongle = PortInfo("COM9", DONGLE, 0x0403, 0x6001, "FT232")
        _board, ports, runner = self.fakes(others=[dongle], linkcli_output=HELLO)
        code, out = self.walk(["walk", "8"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        _name, text = self.result_file()
        self.assertIn("**section 8 hello** -> role repeater, board rak4630-single, "
                      "fw_version 0.0.0-dev, slot 0", text)

    def test_an_image_that_is_not_ours_is_refused_before_any_port(self):
        _board, ports, runner = self.fakes()
        uf2 = os.path.join(self.repo, "out", "build", "repeater_rak4630_single", "firmware.uf2")
        with open(uf2, "wb") as fh:
            fh.write(b"UF2\n")
        code, out = self.walk(["walk", "8", "--image", uf2], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_REFUSED, out)
        self.assertIn("is a UF2 image", out)
        self.assertEqual(self.ports.opened, [])

    def test_no_image_at_all_is_refused_with_what_builds_one(self):
        os.remove(self.repeater_zip)
        _board, ports, runner = self.fakes()
        code, out = self.walk(["walk", "8"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_REFUSED, out)
        self.assertIn("tools/verify.ps1 builds it", out)


# ----------------------------------------------------------------------------- section 9

class SectionNine(WalkCase):
    def walk_nine(self, firmware=None, **kw):
        board = self.board(firmware or FakeRepeaterFirmware(REPEATER_ID))
        ports = FakePorts(boards=[board], clock=self.clock)
        runner = FakeRunner(boards=[board], new_firmware=firmware_for, **kw)
        code, out = self.walk(["walk", "9", "--collect", "60", "--baseline", "20"],
                              ports=ports, runner=runner)
        return board, code, out

    def test_a_boot_line_nobody_caught_is_infos_own_fields(self):
        """What the first real board did (p3.4-02): no `boot` line reached the host, and `info`
        carried the same fw, role, board, ver, reset and radio_ok - which is the line."""
        board = self.board(FakeRepeaterFirmware(REPEATER_ID))
        ports = FakePorts(boards=[board], clock=self.clock)
        runner = FakeRunner(boards=[board],
                            new_firmware=lambda path: FakeScannerFirmware(prints_banner=False)
                            if "scanner" in path else FakeRepeaterFirmware(REPEATER_ID))
        code, out = self.walk(["walk", "9", "--collect", "60", "--baseline", "20"],
                              ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        _name, text = self.result_file()
        self.assertNotIn("| differs |", text)
        self.assertIn("the `boot` line was not caught", text)
        self.assertIn("**section 9 radio_ok** -> 1", text)     # info's own field
        self.assertIn("**section 9 reset** -> soft", text)    # info's own field

    def test_a_restore_that_cannot_flash_is_a_differs_and_keeps_the_result_file(self):
        """A scanner's watchdog ended two restores on the first real board (p3.4-02) and took the
        walk's own result file with them. The restore is a step like any other."""
        board = self.board(FakeRepeaterFirmware(REPEATER_ID))
        ports = FakePorts(boards=[board], clock=self.clock)
        runner = FakeRunner(boards=[board], new_firmware=firmware_for,
                            nrfutil_exits=[0, 1, 1])      # the scanner on, the repeater back twice
        code, out = self.walk(["walk", "9", "--collect", "60", "--baseline", "20"],
                              ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_FAILED, out)
        _name, text = self.result_file()                   # the evidence of the walk survived
        self.assertIn("RESTORE FAILED", text)
        self.assertIn("| differs |", text)
        self.assertIn("**section 9 rom** -> SX1261 V2D 2D02", text)
        self.assertEqual(board.flashes, [self.scanner_zip])

    def test_the_round_trip(self):
        board, code, out = self.walk_nine()
        self.assertEqual(code, cfg.EXIT_OK, out)
        _name, text = self.result_file()
        self.assertEveryLineHasAStep("9", text)
        # the scanner went on and the repeater came back, in that order
        self.assertEqual(board.flashes, [self.scanner_zip, self.repeater_zip])
        self.assertNotIn("| differs |", text)
        # the boot line, the blanks, and radio_ok
        self.assertIn("**section 9 radio_ok** -> True", text)
        self.assertIn("**section 9 reset** -> soft", text)
        # the scans
        self.assertIn("**section 9 sweep n** -> 281", text)
        self.assertIn("**section 9 hist** -> 2 lines", text)
        self.assertRegex(text, r"\*\*section 9 cad\*\* -> det 17 of 100")
        self.assertRegex(text, r"\*\*section 9 stop\*\* -> 0\.\d+ s")
        # a minute of output: every line JSON, no nan, and the heartbeats
        self.assertIn("0 not JSON, 0 with `nan`", text)
        self.assertRegex(text, r"\*\*section 9 heartbeats\*\* -> [1-9] in 60 s")
        # the fast sweep, with the plugin's rom
        self.assertIn("**section 9 rom** -> SX1261 V2D 2D02", text)
        self.assertIn("**section 9 scope n** -> 250", text)
        self.assertIn("**section 9 scope sps** -> 47.6", text)
        # the baseline is on disk, and the picture was attempted
        out_dir = os.path.join(self.repo, "out", "bench")
        baseline = [n for n in os.listdir(out_dir) if n.startswith("baseline-150-960-")]
        self.assertEqual(len(baseline), 1, os.listdir(out_dir))
        self.assertTrue(baseline[0].endswith(".ndjson"))
        with open(os.path.join(out_dir, baseline[0]), encoding="utf-8") as fh:
            frames = [json.loads(one) for one in fh if one.strip()]
        self.assertTrue(any(frame.get("t") == "scope" and frame.get("n") == 2048
                            for frame in frames), frames[:2])
        self.assertIn("the picture was not drawn", text)      # no matplotlib behind a fake runner
        self.assertIn("waterfall_plot.py", " ".join(" ".join(c) for c in self.runner.calls))
        # and the node is the node it was
        self.assertIn("Repeater ID matches", text)
        self.assertIsNone(load_run(self.home, SERIAL))

    def test_the_watchdog_line_is_walked_with_hang(self):
        """9.6 was a person's until p3.5-03 - *a session cannot cause one*. `hang` is the cause:
        the loop stops, the loop watchdog resets the unit, and `boot` says `wdt` `loop`."""
        board, code, out = self.walk_nine()
        self.assertEqual(code, cfg.EXIT_OK, out)
        _name, text = self.result_file()
        self.assertIn("### 9.6 ", text)
        self.assertNotIn("| needs a person | `hang`", text)
        self.assertIn("**section 9 watchdog reset** -> wdt", text)
        self.assertIn("**section 9 watchdog half** -> loop", text)
        self.assertRegex(text, r"\*\*section 9 watchdog, time to the port\*\* -> \d+\.\d s")
        self.assertIn("sent: `hang`", text)   # the line the step sent, in the transcript
        # and the restore after it put the repeater back on a board that had just reset
        self.assertEqual(board.flashes, [self.scanner_zip, self.repeater_zip])
        self.assertIn("Repeater ID matches", text)

    def test_an_image_with_no_hang_command_makes_the_watchdog_line_a_differs(self):
        """A scanner image from before p3.5-03: the line cannot be walked, and the step says so
        rather than pretending the watchdog was seen."""
        def no_hang(path):
            firmware = firmware_for(path)
            if isinstance(firmware, FakeScannerFirmware):
                firmware.hangs = False
            return firmware
        board = self.board()
        ports = FakePorts(boards=[board], clock=self.clock)
        runner = FakeRunner(boards=[board], new_firmware=no_hang)
        code, out = self.walk(["walk", "9", "--collect", "30", "--baseline", "5"],
                              ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_FAILED, out)
        _name, text = self.result_file()
        self.assertIn("is a person's again", text)
        self.assertIn("| differs |", text)
        # the walk still finished: the repeater is back on the board
        self.assertEqual(board.flashes[-1], self.repeater_zip)
        self.assertIn("restored: repeater back on the board", text)

    def test_a_rom_that_is_not_the_plugins_skips_the_fast_sweep_and_walks_on(self):
        def other_rom(path):
            firmware = firmware_for(path)
            if isinstance(firmware, FakeScannerFirmware):
                firmware.rom = "SX1261 V1A 1A01"
            return firmware
        board = self.board()
        ports = FakePorts(boards=[board], clock=self.clock)
        runner = FakeRunner(boards=[board], new_firmware=other_rom)
        code, out = self.walk(["walk", "9", "--collect", "30", "--baseline", "5"],
                              ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_FAILED, out)          # the rom line itself differs
        _name, text = self.result_file()
        self.assertIn("SX1261 V1A 1A01", text)
        self.assertIn("the first line of *The fast sweep* stops the rest of it", text)
        self.assertEqual(len([s for s in text.splitlines() if "| skipped |" in s]), 2,
                         "the two scope lines are the only skipped ones")
        # the walk went on to the end: the repeater is back
        self.assertEqual(board.flashes[-1], self.repeater_zip)
        self.assertIn("restored: repeater back on the board", text)

    def test_the_baseline_needs_the_dummy_load(self):
        self.config(rf="antenna")
        # a repeater on a board whose RF port is not the dummy load is never replaced (p3.3-02),
        # so the scanner cannot go on at all - which is the refusal, before any port
        board, code, out = self.walk_nine()
        self.assertEqual(code, cfg.EXIT_REFUSED, out)
        self.assertIn("would leave a board this tool may not restore", out)
        self.assertEqual(board.flashes, [])

        # with a scanner already on the board and the antenna connected, the walk runs and the
        # baseline is the one step that is skipped for the RF port
        self.setUp()
        self.config(rf="antenna")
        board, code, out = self.walk_nine(firmware=FakeScannerFirmware())
        _name, text = self.result_file()
        self.assertIn("it needs the 50 ohm dummy load", text)
        self.assertFalse([n for n in os.listdir(os.path.join(self.repo, "out", "bench"))
                          if n.startswith("baseline-")])

    def test_a_scanner_that_answers_no_scan_line_differs(self):
        def silent(path):
            firmware = firmware_for(path)
            if isinstance(firmware, FakeScannerFirmware):
                firmware.answers_data = False
            return firmware
        board = self.board()
        ports = FakePorts(boards=[board], clock=self.clock)
        runner = FakeRunner(boards=[board], new_firmware=silent)
        code, out = self.walk(["walk", "9", "--collect", "20", "--baseline", "5"],
                              ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_FAILED, out)
        _name, text = self.result_file()
        self.assertIn("## What differs", text)
        self.assertIn("`sweep` is acked and answers a `sweep` line of 281 points", text)
        self.assertEqual(board.flashes[-1], self.repeater_zip)   # still put back

    def test_a_line_that_is_not_json_or_carries_nan_is_a_differs(self):
        def chatty(path):
            firmware = firmware_for(path)
            if isinstance(firmware, FakeScannerFirmware):
                # lines nobody asked for, arriving with a heartbeat in the middle of the minute
                firmware.chatter = ['{"t":"sweep","avg":[nan]}', "# chatter"]
            return firmware
        board = self.board()
        ports = FakePorts(boards=[board], clock=self.clock)
        runner = FakeRunner(boards=[board], new_firmware=chatty)
        code, out = self.walk(["walk", "9", "--collect", "20", "--baseline", "5"],
                              ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_FAILED, out)
        _name, text = self.result_file()
        self.assertIn("with `nan`", text)

    def test_a_repeater_image_is_refused_for_section_nine(self):
        _board, ports, runner = self.fakes()
        code, out = self.walk(["walk", "9", "--scanner-image", self.repeater_zip],
                              ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_REFUSED, out)
        self.assertIn("is a repeater image: section 9 is the scanner's round trip", out)
        self.assertEqual(self.ports.opened, [])


# ----------------------------------------------------------------------------- section 10

class SectionTen(WalkCase):
    def setUp(self):
        super().setUp()
        self.config(identity="disposable")

    def walk_ten(self, firmware=None, others=None, **kw):
        # The fake answers `get name` with what this build sets, as a fresh board does: the walk
        # reads `-D ADVERT_NAME` out of platformio.ini and not upstream's own default (p3.4-02).
        board = self.board(firmware or FakeRepeaterFirmware(REPEATER_ID,
                                                            name=walk.advert_name(REPO)))
        ports = FakePorts(boards=[board], clock=self.clock, others=others)
        runner = FakeRunner(boards=[board], new_firmware=firmware_for, **kw)
        bench = self.bench(ports=ports, runner=runner)
        bench.commit = COMMIT
        bench.walk_password = PASSWORD      # a session generates one and keeps it nowhere
        code = benchcli.main(["walk", "10"], bench=bench)
        return board, code, self.out.getvalue()

    def test_boot_lines_nobody_could_catch_are_a_persons_and_the_key_is_still_read(self):
        """What the first real board did (p3.4-02): the `Repeater ID:` and `First boot:` lines
        were gone before the port was back, `get public.key` gave the new key, and
        `firstboot.state` said `done`. The two lines themselves become a person's."""
        board, code, out = self.walk_ten(FakeRepeaterFirmware(REPEATER_ID,
                                                              name=walk.advert_name(REPO),
                                                              prints_banner=False))
        self.assertEqual(code, cfg.EXIT_OK, out)
        _name, text = self.result_file()
        self.assertNotIn("| differs |", text)
        self.assertIn("the node's key is a new one after the erase", text)
        self.assertIn("`get public.key`", text)
        self.assertIn("the boot lines were gone before the port was back", text)
        self.assertIn("**section 10 firstboot.state** -> done", text)
        self.assertNoPassword(text)

    def test_an_identity_that_is_kept_is_refused(self):
        self.config(identity="keep")
        _board, ports, runner = self.fakes()
        code, out = self.walk(["walk", "10"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_REFUSED, out)
        self.assertIn("erases the file system", out)
        self.assertEqual(self.ports.opened, [])

    def test_an_open_run_is_refused_because_the_erase_would_outlive_it(self):
        save_run(self.home, RunRecord(serial=SERIAL, role="repeater", version="0.0.0-dev",
                                      repeater_id=REPEATER_ID, started="now", how="console"))
        _board, ports, runner = self.fakes()
        code, out = self.walk(["walk", "10"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_REFUSED, out)
        self.assertIn("`bench restore` it first", out)
        self.assertEqual(self.ports.opened, [])

    def test_the_first_boot_the_second_boot_and_the_provisioning(self):
        board, code, out = self.walk_ten()
        self.assertEqual(code, cfg.EXIT_OK, out)
        _name, text = self.result_file()
        self.assertEveryLineHasAStep("10", text)
        self.assertEqual(board.firmware.erased, 1)
        self.assertEqual(board.firmware.rebooted, 2)          # the two boots of the page
        self.assertNotIn("| differs |", text)
        self.assertIn("**section 10 First boot** -> 2 line(s)", text)
        self.assertIn("**section 10 firstboot.state** -> done", text)
        self.assertIn("**section 10 firstboot.state, second boot** -> never", text)
        self.assertIn("**section 10 get dutycycle** -> 10.0%", text)
        self.assertIn("**section 10 path.hash.mode** -> 1", text)
        self.assertIn("**section 10 guest.password** -> empty", text)
        self.assertIn("**section 10 provisioned** -> 7 line(s) and the password, at the console",
                      text)
        self.assertIn("**section 10 get name** -> bench-walk", text)
        self.assertIn("no dongle on this machine", text)      # the link half, skipped
        # the identity afterwards is the one the first boot printed
        self.assertIn("the node is the same node", text)
        self.assertNotEqual(board.firmware.repeater_id, REPEATER_ID)
        self.assertIsNone(load_run(self.home, SERIAL))         # it never opened a run
        self.assertEqual(board.flashes, [])                    # and never flashed

    def test_the_password_reaches_nothing(self):
        board, code, out = self.walk_ten()
        self.assertEqual(code, cfg.EXIT_OK, out)
        _name, text = self.result_file()
        self.assertNoPassword(text)
        self.assertNoPassword(out)
        self.assertIn("password ****", text)
        self.assertIn(walk.consolemod.PASSWORD_ECHO_SHOWN, text)
        # the board did take it: it is the firmware's password now, and nothing printed it
        self.assertEqual(board.firmware.password, PASSWORD)

    def test_over_the_link_when_the_dongle_is_there(self):
        self.config(identity="disposable", dongle=DONGLE)
        dongle = PortInfo("COM9", DONGLE, 0x0403, 0x6001, "FT232")
        board, code, out = self.walk_ten(others=[dongle], linkcli_output=PROVISIONED)
        self.assertEqual(code, cfg.EXIT_OK, out)
        _name, text = self.result_file()
        self.assertIn("the same lines through `linkcli.py provision`", text)
        self.assertIn("**section 10 provisioned** -> 7 line(s) and the password, over the link",
                      text)
        self.assertIn("the dongle is on this machine", text)   # the console half, skipped
        self.assertNoPassword(text)
        self.assertNoPassword(out)
        # the file it wrote is gone, and it was never in the repository
        provision = [c for c in self.runner.calls if "provision" in c]
        self.assertEqual(len(provision), 1)
        self.assertFalse(os.path.exists(provision[0][-1]))
        self.assertNotIn(self.repo, provision[0][-1])

    def test_a_link_tool_that_printed_the_password_would_be_a_finding(self):
        self.config(identity="disposable", dongle=DONGLE)
        dongle = PortInfo("COM9", DONGLE, 0x0403, 0x6001, "FT232")
        leaking = PROVISIONED.replace("password ****", "password %s" % PASSWORD)
        _board, code, out = self.walk_ten(others=[dongle], linkcli_output=leaking)
        self.assertEqual(code, cfg.EXIT_FAILED, out)
        _name, text = self.result_file()
        self.assertIn("THE PASSWORD APPEARED IN THE TOOL'S OUTPUT", text)
        self.assertNoPassword(text)                            # and still not in the file

    def test_a_scanner_on_the_board_is_refused(self):
        board, code, out = self.walk_ten(firmware=FakeScannerFirmware())
        self.assertEqual(code, cfg.EXIT_REFUSED, out)
        self.assertIn("nothing a repeater says", out)


# ----------------------------------------------------------------------------- ticking the page

class Ticking(WalkCase):
    def test_a_confirmed_line_is_ticked_with_the_date_and_the_commit(self):
        _board, ports, runner = self.fakes()
        code, out = self.walk(["walk", "1", "--tick"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        page = self.page()
        bounds = walk.bullet_bounds(page, walk.SECTIONS["1"].line("1.1").anchor)
        bullet = page[bounds[0]:bounds[1]]
        self.assertTrue(bullet.startswith("- [x] "), bullet[:40])
        self.assertRegex(bullet, r"\(bench, \d{4}-\d\d-\d\d, 0123456\)$")
        # the lines a person owes are untouched
        for key in ("1.2", "1.3", "1.4", "1.5", "1.6", "1.8"):
            other = walk.bullet_bounds(page, walk.SECTIONS["1"].line(key).anchor)
            self.assertTrue(page[other[0]:].startswith("- [ ] "), key)
        self.assertIn("ticked in docs/bench.md: 1.1", out)

    def test_ticking_twice_changes_nothing_the_second_time(self):
        _board, ports, runner = self.fakes()
        self.walk(["walk", "1", "--tick"], ports=ports, runner=runner)
        once = self.page()
        self.out = io.StringIO()
        _board, ports, runner = self.fakes()
        self.walk(["walk", "1", "--tick"], ports=ports, runner=runner)
        self.assertEqual(self.page(), once)

    def test_the_second_walk_says_already_ticked_and_not_not_found(self):
        """The words the first board showed (p3.4-02): a second walk of a section said `not
        found in docs/bench.md` of lines an earlier run had ticked."""
        _board, ports, runner = self.fakes()
        self.walk(["walk", "1", "--tick"], ports=ports, runner=runner)
        self.out = io.StringIO()
        _board, ports, runner = self.fakes()
        code, out = self.walk(["walk", "1", "--tick"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        self.assertIn("already ticked in docs/bench.md, so left as they are: 1.1", out)
        self.assertNotIn("not found in docs/bench.md", out)
        self.assertIn("ticked in docs/bench.md: nothing", out)

    def test_a_line_the_page_lost_still_says_not_found(self):
        """The other half: the message stays for a page that really did move."""
        page = self.page()
        bounds = walk.bullet_bounds(page, walk.SECTIONS["1"].line("1.1").anchor)
        with open(os.path.join(self.repo, "docs", "bench.md"), "w", encoding="utf-8",
                  newline="\n") as fh:
            fh.write(page[:bounds[0]] + page[bounds[1]:].lstrip("\n"))
        _board, ports, runner = self.fakes()
        code, out = self.walk(["walk", "1", "--tick"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        self.assertIn("not found in docs/bench.md, so not ticked: 1.1", out)
        self.assertNotIn("already ticked", out)

    def test_without_the_flag_the_page_is_not_touched(self):
        before = self.page()
        _board, ports, runner = self.fakes()
        code, out = self.walk(["walk", "1"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        self.assertEqual(self.page(), before)
        self.assertIn("to tick 1.1 in docs/bench.md: the same walk with --tick", out)

    def test_a_follow_up_goes_to_the_phase_of_the_task_that_wrote_the_line(self):
        result = walk.WalkResult(walk.SECTIONS["9"], None, COMMIT, "2026-10-18T00:00:00Z")
        line = walk.SECTIONS["9"].line("9.7")                  # p3.1-01's
        result.add(walk.Step(line, "the rom", walk.DIFFERS, saw="rom 'SX1261 V1A 1A01'",
                             expected=walk.PLUGIN_ROM))
        self.phase_followups("3.1")
        written = walk.write_followups(result, self.repo)
        self.assertEqual([os.path.relpath(p, self.repo).replace("\\", "/") for p in written],
                         ["tasks/phase-3.1/FOLLOWUPS.md"])
        with open(written[0], encoding="utf-8") as fh:
            text = fh.read()
        self.assertIn("## From a bench walk of section 9 (2026-10-18)", text)
        self.assertIn("**[01] `docs/bench.md` 9.7 differs from the board**", text)
        self.assertIn("SX1261 V1A 1A01", text)

    def test_a_phase_with_no_followups_file_is_left_alone(self):
        result = walk.WalkResult(walk.SECTIONS["9"], None, COMMIT, "2026-10-18T00:00:00Z")
        result.add(walk.Step(walk.SECTIONS["9"].line("9.7"), "the rom", walk.DIFFERS))
        self.assertEqual(walk.write_followups(result, self.repo), [])


# ----------------------------------------------------------------------------- the small pieces

class ThePieces(unittest.TestCase):
    def test_the_band_is_read_from_platformio_ini_and_not_remembered(self):
        expect = walk.band_expectations(REPO)
        self.assertEqual(expect["radio"], "869.618,62.5,8,8")
        self.assertEqual(expect["dutycycle"], "10.0%")
        self.assertIsNone(walk.band_expectations(REPO, band="nosuchband"))

    def test_a_number_read_back_is_held_as_a_number(self):
        """`set lat 55.3250` reads back `55.3250007` on a real board (p3.4-02)."""
        self.assertTrue(walk.same_number("55.3250007", "55.3250"))
        self.assertTrue(walk.same_number(" 10.4899998 ", "10.4900"))
        self.assertFalse(walk.same_number("55.4250007", "55.3250"))
        self.assertFalse(walk.same_number("", "55.3250"))
        self.assertFalse(walk.same_number("Unknown command: get lat", "55.3250"))

    def test_the_node_name_is_this_builds_and_not_upstreams(self):
        """What the first real board answered to `get name` (p3.4-02): `-D ADVERT_NAME` of
        platformio.ini, not upstream's own `repeater`."""
        self.assertEqual(walk.advert_name(REPO), "RAK4631 Repeater")
        self.assertEqual(walk.advert_name(os.path.join(REPO, "nosuchdirectory")), "repeater")

    def test_get_radio_is_held_to_the_band_as_numbers(self):
        """What the first real board answered (p3.4-02): upstream prints every digit of the float
        it keeps, so the string is not the band's and the frequency is."""
        self.assertTrue(walk.same_radio("869.6179809,62.5,8,8", "869.618,62.5,8,8"))
        self.assertTrue(walk.same_radio("869.618,62.5,8,8", "869.618,62.5,8,8"))
        self.assertTrue(walk.same_radio(" 869.6179809 , 62.5 , 8 , 8 ", "869.618,62.5,8,8"))
        # a different radio is still a different radio
        self.assertFalse(walk.same_radio("868.0,62.5,8,8", "869.618,62.5,8,8"))
        self.assertFalse(walk.same_radio("869.6179809,125.0,8,8", "869.618,62.5,8,8"))
        self.assertFalse(walk.same_radio("869.6179809,62.5,7,8", "869.618,62.5,8,8"))
        self.assertFalse(walk.same_radio("869.6179809,62.5,8,5", "869.618,62.5,8,8"))
        # and nothing, or something that is not four numbers, is not the band
        self.assertFalse(walk.same_radio("", "869.618,62.5,8,8"))
        self.assertFalse(walk.same_radio("869.618,62.5,8", "869.618,62.5,8,8"))
        self.assertFalse(walk.same_radio("Unknown command: get radio", "869.618,62.5,8,8"))

    def test_the_fw_version_line(self):
        parsed = walk.parse_fw_version_line(
            "> twinoak-repeater 0.0.0-dev on MeshCore repeater-v1.17.1 (d9296435)")
        self.assertEqual(parsed, {"role": "repeater", "version": "0.0.0-dev",
                                  "tag": "repeater-v1.17.1", "pin": "d9296435"})
        self.assertIsNone(walk.parse_fw_version_line("> something else"))
        self.assertIsNone(walk.parse_fw_version_line(""))

    def test_an_ack_says_what_was_taken(self):
        fields = walk.ack_fields("n=250 step=28.0000kHz rbw=39.0kHz seg=1 cov=1.393 interval=0ms")
        self.assertEqual(fields["n"], "250")
        self.assertEqual(fields["seg"], "1")
        self.assertEqual(fields["cov"], "1.393")
        self.assertEqual(walk.ack_fields(None), {})

    def test_nan_is_found_and_a_name_with_nan_in_it_is_not(self):
        self.assertEqual(len(walk.nan_lines('{"avg":nan}\n{"avg":-1}\n')), 1)
        self.assertEqual(walk.nan_lines('{"t":"stat","banana":1,"nan_count":0}'), [])
        self.assertEqual(walk.nan_lines('{"avg":NaN}')[0], '{"avg":NaN}')

    def test_a_password_line_is_written_down_as_stars_and_stays_that_way(self):
        self.assertEqual(walk._starred("password hunter2"), "password ****")
        self.assertEqual(walk._starred("set guest.password hello"), "set guest.password ****")
        self.assertEqual(walk._starred("get name"), "get name")
        self.assertEqual(walk._starred(""), "")
        # console.redact leaves an already-starred line alone - that is how a transcript can say
        # which line was sent - and still replaces anything with a value in it
        self.assertEqual(walk.consolemod.redact("password ****"), "password ****")
        self.assertEqual(walk.consolemod.redact("set guest.password ****"),
                         "set guest.password ****")
        for leak in ("password hunter2", "password now: hunter2", "  -> password now: hunter2",
                     "set guest.password hello", "password ****extra"):
            self.assertEqual(walk.consolemod.redact(leak), walk.consolemod.PASSWORD_ECHO_SHOWN,
                             leak)

    def test_a_frame_is_summarised_and_not_dumped(self):
        text = walk.frame_summary({"t": "sweep", "n": 281, "avg": [-115.0] * 281,
                                   "lv": "x" * 400})
        self.assertIn('"avg": "[281 values]"', text)
        self.assertIn("...", text)
        self.assertLess(len(text), 200)
        self.assertEqual(walk.frame_summary(None), "(nothing)")

    def test_every_walk_is_a_method_and_every_line_key_is_unique(self):
        for number, section in walk.SECTIONS.items():
            self.assertTrue(hasattr(walk.Walker, "walk_%s" % number.replace(".", "_")), number)
            keys = [line.key for line in section.lines]
            self.assertEqual(len(keys), len(set(keys)), number)
            for line in section.lines:
                self.assertTrue(line.key.startswith(number + "."), line.key)
                self.assertTrue(line.about, line.key)


if __name__ == "__main__":
    unittest.main(verbosity=2)
