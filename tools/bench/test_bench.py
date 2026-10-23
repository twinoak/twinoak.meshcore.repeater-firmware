#!/usr/bin/env python3
"""tools/bench's guards, each one a test, none of them opening a port.

    python tools/bench/test_bench.py

tools/verify.ps1's release step runs these beside the release tooling's tests
(tools/release/selfcheck.py), so CI - which has no board and needs none - proves on every commit
that the tool refuses what it must refuse. Everything runs against tools/bench/fakes.py: a serial
layer that records every open, a runner that runs nothing, a process table of one line.

What is held here:
  * the config's "no bench" paths: no file, a file that is not a bench, no port with the serial
    number, a lone COM port with another serial number that is never picked;
  * every refusal of an image - a bootloader or a SoftDevice in the package, no descriptor, two,
    another board, too large for the region, a transmitting role with the RF port off the dummy
    load, UF2 / HEX / BIN by name and by content - each refused before any port is opened;
  * the lock: taken, refused while a live process holds it, a stale one named and taken over;
  * the console's refusals and its redaction of `password now:`;
  * a flash end to end against the fake board: the touch, the bootloader's port, nrfutil's
    argv, the application back, the log line with the descriptor and the SHA-256;
  * record and restore: the run's own image or the given one, the last line, the capitals.
"""

import io
import json
import os
import shutil
import sys
import tempfile
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
for _p in (HERE, os.path.join(REPO, "spec", "tools"), os.path.join(REPO, "tools", "release")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import build_release  # noqa: E402
import manifestref  # noqa: E402

import bench as benchcli  # noqa: E402
import config as cfg  # noqa: E402
import console  # noqa: E402
import images  # noqa: E402
from fakes import (FakeBoard, FakeClock, FakePorts, FakeProbe, FakeRepeaterFirmware,  # noqa: E402
                   FakeRunner, FakeScannerFirmware, SlowToOpenPorts)
from flash import Timeouts, nrfutil_argv  # noqa: E402
from lock import BoardLock, Locked  # noqa: E402
from ports import (PortInfo, Ports, RAK_APP_PID, RAK_APP_PIDS, RAK_BOOT_PIDS,  # noqa: E402
                   RAK_VID)
from runs import RunRecord, load_run, save_run  # noqa: E402

SERIAL = "E6614C311B5C2E35"
BOARD = "rak4630-single"
REGION = build_release.AppRegion(0x26000, 0xED000, "ldscript.ld", 7,
                                 "FLASH (rx)     : ORIGIN = 0x26000, LENGTH = 0xED000 - 0x26000")
SMALL_REGION = build_release.AppRegion(0x26000, 0x26000 + 2048, "ldscript.ld", 7, "FLASH ... 2048")
REPEATER_ID = "7F" * 32
OTHER_ID = "11" * 32


# ----------------------------------------------------------------------------- building images

def descriptor(role="repeater", version="0.0.0-dev", board=BOARD, band="868"):
    return manifestref.make_descriptor(role, version, board, band, 1, 0)


def application(desc, size=4096, where=0x400, extra_where=None, broken=False):
    blob = bytearray(manifestref.fake_image(desc or b"\0" * 96, size, where, 7)) if desc else \
        bytearray(manifestref.fake_image(b"\0" * 96, size, where, 7))
    if desc is None:
        blob[where:where + 96] = os.urandom(96).replace(b"TWOK", b"xxxx")
    if extra_where is not None:
        blob[extra_where:extra_where + 96] = desc
    if broken:
        blob[where + 20] ^= 0x01
    return bytes(blob)


def dfu_manifest(app_name="firmware.bin", parts=None):
    manifest = {"manifest": {"application": {"bin_file": app_name, "dat_file": "firmware.dat",
                                             "init_packet_data": {"application_version": 4294967295,
                                                                  "device_revision": 65535,
                                                                  "device_type": 82,
                                                                  "firmware_crc16": 1234,
                                                                  "softdevice_req": [182]}},
                             "dfu_version": 0.5}}
    for part in (parts or []):
        manifest["manifest"][part] = {"bin_file": part + ".bin", "dat_file": part + ".dat"}
    return json.dumps(manifest)


def write_package(path, app, parts=None, manifest=True):
    with zipfile.ZipFile(path, "w") as zf:
        zf.writestr("firmware.bin", app)
        zf.writestr("firmware.dat", b"\x52\x00\xff\xff\xff\xff\xff\xff\x01\x00\xb6\x00\xd2\x04")
        if manifest:
            zf.writestr("manifest.json", dfu_manifest(parts=parts))
        for part in (parts or []):
            zf.writestr(part + ".bin", b"a bootloader or a softdevice")
    return path


class BenchCase(unittest.TestCase):
    """A temporary repository root with a bench.local.json, a temporary data directory, fakes."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="twinoak-bench-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.repo = os.path.join(self.tmp, "repo")
        os.makedirs(os.path.join(self.repo, "out", "build", "repeater_rak4630_single"))
        os.makedirs(os.path.join(self.repo, "out", "build", "scanner_rak4630_single"))
        self.home = os.path.join(self.tmp, "home")
        self.clock = FakeClock()
        self.out = io.StringIO()
        self.region = REGION

    def config(self, rf="dummy-load", identity="keep", dongle=None, serial=SERIAL, **extra):
        data = {"board": BOARD, "serial": serial, "rf": rf, "identity": identity}
        if dongle:
            data["dongle_serial"] = dongle
        data.update(extra)
        with open(os.path.join(self.repo, cfg.BENCH_FILE), "w", encoding="utf-8") as fh:
            json.dump(data, fh)
        return data

    def package(self, name="firmware.zip", role="repeater", version="0.0.0-dev", board=BOARD, size=4096,
                parts=None, no_descriptor=False, two=False, broken=False, where="repeater_rak4630_single"):
        desc = None if no_descriptor else descriptor(role, version, board)
        app = application(desc, size=size, extra_where=0x800 if two else None, broken=broken)
        path = os.path.join(self.repo, "out", "build", where, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        return write_package(path, app, parts=parts)

    def bench(self, ports=None, runner=None, probe=None, region=None, timeouts=None):
        self.ports = ports if ports is not None else FakePorts(clock=self.clock)
        self.runner = runner if runner is not None else FakeRunner(boards=self.ports.boards)
        self.probe = probe if probe is not None else FakeProbe({os.getpid(): 1.0})
        chosen = self.region if region is None else (None if region is False else region)
        return benchcli.Bench(repo=self.repo, ports=self.ports, runner=self.runner, home=self.home, out=self.out,
                              probe=self.probe, nrfutil="/pio/packages/tool-adafruit-nrfutil/adafruit-nrfutil.py",
                              python="python", timeouts=timeouts or Timeouts(settle_s=0.0, dfu_settle_s=0.0),
                              region_for=lambda path: chosen)

    def run_cli(self, argv, **kw):
        bench = self.bench(**kw)
        code = benchcli.main(argv, bench=bench)
        return code, self.out.getvalue()

    def board(self, firmware=None, mode="application"):
        board = FakeBoard(serial=SERIAL, firmware=firmware or FakeRepeaterFirmware(REPEATER_ID))
        board.mode = mode
        return board

    def assertNothingOpened(self):
        self.assertEqual(self.ports.opened, [], "a port was opened: %r" % self.ports.opened)
        self.assertEqual(self.runner.calls, [], "a program was run: %r" % self.runner.calls)


# ----------------------------------------------------------------------------- the config

class TheConfig(BenchCase):
    def test_no_file_is_no_bench(self):
        code, out = self.run_cli(["status"])
        self.assertEqual(code, cfg.EXIT_NO_BENCH)
        self.assertIn("no bench", out)
        self.assertIn("bench.local.json", out)
        self.assertNothingOpened()

    def test_a_file_that_is_not_json_is_no_bench(self):
        with open(os.path.join(self.repo, cfg.BENCH_FILE), "w") as fh:
            fh.write("{not json")
        code, out = self.run_cli(["status"])
        self.assertEqual(code, cfg.EXIT_NO_BENCH)
        self.assertIn("not JSON", out)

    def test_an_rf_port_nobody_named_is_no_bench(self):
        self.config(rf="roof")
        code, out = self.run_cli(["status"])
        self.assertEqual(code, cfg.EXIT_NO_BENCH)
        self.assertIn("dummy-load, antenna, nothing", out)

    def test_an_identity_mode_nobody_named_is_no_bench(self):
        self.config(identity="maybe")
        code, out = self.run_cli(["status"])
        self.assertEqual(code, cfg.EXIT_NO_BENCH)

    def test_a_missing_serial_is_no_bench(self):
        with open(os.path.join(self.repo, cfg.BENCH_FILE), "w") as fh:
            json.dump({"board": BOARD, "rf": "dummy-load", "identity": "keep"}, fh)
        code, out = self.run_cli(["status"])
        self.assertEqual(code, cfg.EXIT_NO_BENCH)
        self.assertIn("serial", out)

    def test_no_port_with_that_serial_is_no_bench(self):
        self.config()
        code, out = self.run_cli(["status"])
        self.assertEqual(code, cfg.EXIT_NO_BENCH)
        self.assertIn(SERIAL, out)
        self.assertIn("NOT PRESENT", out)

    def test_the_only_com_port_is_never_picked(self):
        self.config()
        lone = PortInfo("COM3", "SOMEBODY-ELSE", RAK_VID, RAK_APP_PID, "a RAK that is not the bench's")
        ports = FakePorts(others=[lone], clock=self.clock)
        code, out = self.run_cli(["status"], ports=ports)
        self.assertEqual(code, cfg.EXIT_NO_BENCH)
        self.assertNothingOpened()
        self.package()
        self.out = io.StringIO()
        code, out = self.run_cli(["flash", os.path.join(self.repo, "out", "build", "repeater_rak4630_single", "firmware.zip")],
                                 ports=FakePorts(others=[lone], clock=self.clock))
        self.assertEqual(code, cfg.EXIT_NO_BENCH)
        self.assertNothingOpened()

    def test_every_command_says_no_bench_without_a_board(self):
        self.config()
        path = self.package()
        for argv in (["record"], ["flash", path], ["console", "ver"], ["restore"], ["console", "--scanner", "info"]):
            self.out = io.StringIO()
            code, out = self.run_cli(argv)
            self.assertEqual(code, cfg.EXIT_NO_BENCH, argv)
            self.assertIn("no bench", out)
            self.assertNothingOpened()

    def test_status_with_the_board_present(self):
        self.config(dongle="A50285BI")
        ports = FakePorts(boards=[self.board()], others=[PortInfo("COM9", "A50285BI", 0x0403, 0x6001, "FT232")],
                          clock=self.clock)
        code, out = self.run_cli(["status"], ports=ports)
        self.assertEqual(code, cfg.EXIT_OK)
        self.assertIn("application on COM7", out)
        self.assertIn("dongle on COM9", out)
        self.assertIn("none open", out)
        self.assertEqual(self.ports.opened, [])

    def test_the_committed_example_is_a_bench(self):
        with open(os.path.join(REPO, "bench.example.json"), "r", encoding="utf-8") as fh:
            c = cfg.parse_config(fh.read(), "bench.example.json")
        self.assertEqual(c.board, BOARD)
        self.assertIn(c.rf, cfg.RF_PORTS)
        self.assertIn(c.identity, cfg.IDENTITY_MODES)

    def test_the_data_dir_override(self):
        old = os.environ.get("TWINOAK_BENCH_HOME")
        os.environ["TWINOAK_BENCH_HOME"] = self.home
        try:
            self.assertEqual(cfg.data_dir(), self.home)
        finally:
            if old is None:
                del os.environ["TWINOAK_BENCH_HOME"]
            else:
                os.environ["TWINOAK_BENCH_HOME"] = old


# ----------------------------------------------------------------------------- the images

class TheImageGuards(BenchCase):
    """Each refusal through `check` (no board needed) and through `flash` with the board present:
    the same answer, and not one port opened by either."""

    def refuse(self, path, *needles, **kw):
        rf = kw.pop("rf", "dummy-load")
        region = kw.pop("region", None)
        self.config(rf=rf)
        self.out = io.StringIO()
        code, out = self.run_cli(["check", path], region=region)
        self.assertEqual(code, cfg.EXIT_REFUSED, out)
        for needle in needles:
            self.assertIn(needle, out)
        self.assertNothingOpened()
        self.out = io.StringIO()
        code, out = self.run_cli(["flash", path], ports=FakePorts(boards=[self.board()], clock=self.clock), region=region)
        self.assertEqual(code, cfg.EXIT_REFUSED, out)
        self.assertNothingOpened()
        self.assertEqual(self.ports.boards[0].flashes, [])
        return out

    def test_a_good_repeater_package_passes_on_the_dummy_load(self):
        self.config()
        code, out = self.run_cli(["check", self.package()])
        self.assertEqual(code, cfg.EXIT_OK, out)
        self.assertIn("repeater rak4630-single 868 version 0.0.0-dev", out)
        self.assertIn("sha256", out)
        self.assertNothingOpened()

    def test_a_package_with_a_bootloader_is_refused(self):
        self.refuse(self.package(parts=["bootloader"]), "bootloader", "application region and nothing else")

    def test_a_package_with_a_softdevice_is_refused(self):
        self.refuse(self.package(parts=["softdevice"]), "softdevice")

    def test_a_package_with_a_softdevice_and_bootloader_is_refused(self):
        self.refuse(self.package(parts=["softdevice_bootloader"]), "softdevice+bootloader")

    def test_an_application_without_a_descriptor_is_refused(self):
        self.refuse(self.package(no_descriptor=True), "0 valid descriptors", "not an image this build or this release made")

    def test_an_application_with_two_descriptors_is_refused(self):
        self.refuse(self.package(two=True), "2 valid descriptors")

    def test_a_descriptor_that_fails_its_crc_is_no_descriptor(self):
        self.refuse(self.package(broken=True), "0 valid descriptors")

    def test_an_image_for_another_board_is_refused(self):
        self.refuse(self.package(board="rak4630-dual-a"), "rak4630-dual-a", "another board")

    def test_the_walters_image_is_refused(self):
        self.refuse(self.package(role="manager", board=BOARD), "manager", "not a role of this board")

    def test_an_image_too_large_for_the_region_is_refused(self):
        out = self.refuse(self.package(size=4096), "does not fit", region=SMALL_REGION)
        self.assertIn("4096 bytes", out)

    def test_an_image_that_exactly_fills_the_region_fits(self):
        self.config()
        code, out = self.run_cli(["check", self.package(size=2048)], region=SMALL_REGION)
        self.assertEqual(code, cfg.EXIT_OK, out)

    def test_an_unknown_region_is_refused(self):
        self.refuse(self.package(), "application region could not be read", region=False)

    def test_a_repeater_image_is_refused_with_the_rf_port_on_an_antenna(self):
        self.refuse(self.package(), "transmits", "antenna", rf="antenna")

    def test_a_repeater_image_is_refused_with_nothing_on_the_rf_port(self):
        self.refuse(self.package(), "transmits", rf="nothing")

    def test_a_scanner_image_is_allowed_on_any_rf_port(self):
        for rf in cfg.RF_PORTS:
            self.config(rf=rf)
            self.out = io.StringIO()
            code, out = self.run_cli(["check", self.package(role="scanner", where="scanner_rak4630_single")])
            self.assertEqual(code, cfg.EXIT_OK, (rf, out))
            self.assertIn("never transmits", out)
            self.assertNothingOpened()

    def test_a_uf2_is_refused_by_name(self):
        path = os.path.join(self.repo, "firmware.uf2")
        with open(path, "wb") as fh:
            fh.write(b"UF2\n" * 100)
        self.refuse(path, "UF2", "no UF2 drive, no SWD")

    def test_a_hex_and_a_bin_are_refused_by_name(self):
        for ext in ("hex", "bin", "elf"):
            path = os.path.join(self.repo, "firmware." + ext)
            with open(path, "wb") as fh:
                fh.write(b":00000001FF\n")
            self.refuse(path, "SWD")

    def test_a_zip_that_is_not_a_dfu_package_is_refused(self):
        path = os.path.join(self.repo, "release.zip")
        with zipfile.ZipFile(path, "w") as zf:
            zf.writestr("README.md", "not a package")
        self.refuse(path, "manifest.json")

    def test_a_file_that_is_not_a_zip_is_refused(self):
        path = os.path.join(self.repo, "firmware.zip")
        with open(path, "wb") as fh:
            fh.write(b"this is not a zip file at all")
        self.refuse(path, "not a zip")

    def test_a_missing_file_is_refused(self):
        self.refuse(os.path.join(self.repo, "nowhere.zip"), "not a file")

    def test_a_package_whose_bin_is_missing_is_refused(self):
        path = os.path.join(self.repo, "firmware.zip")
        with zipfile.ZipFile(path, "w") as zf:
            zf.writestr("manifest.json", dfu_manifest())
        self.refuse(path, "does not contain it")

    def test_the_real_build_passes_when_it_is_there(self):
        """out/build/<env>/firmware.zip of this checkout, when tools/verify.ps1 has left one: the
        guards read the real package and the real linker script copy beside it."""
        real = os.path.join(REPO, "out", "build", "repeater_rak4630_single", "firmware.zip")
        if not os.path.isfile(real):
            self.skipTest("no build in out/build (tools/verify.ps1 leaves one)")
        region = images.find_region(real, REPO)
        self.assertIsNotNone(region)
        image = images.inspect_image(real, BOARD, "dummy-load", region)
        self.assertEqual(image.role, "repeater")
        with self.assertRaises(cfg.Refused):
            images.inspect_image(real, BOARD, "antenna", region)


# ----------------------------------------------------------------------------- the lock

class TheLock(BenchCase):
    def test_taken_and_released(self):
        probe = FakeProbe({os.getpid(): 1.0})
        lock = BoardLock(SERIAL, "test", probe=probe, home=self.home)
        with lock:
            self.assertTrue(os.path.exists(lock.path))
            with open(lock.path) as fh:
                held = json.load(fh)
            self.assertEqual(held["pid"], os.getpid())
            self.assertEqual(held["command"], "test")
            self.assertIn("taken", held)
        self.assertFalse(os.path.exists(lock.path))

    def test_refused_while_a_live_process_holds_it(self):
        probe = FakeProbe({os.getpid(): 1.0, 4242: 500.0})
        BoardLock(SERIAL, "them", probe=probe, home=self.home).acquire()
        path = BoardLock(SERIAL, "x", home=self.home).path
        with open(path, "r+") as fh:
            held = json.load(fh)
            held["pid"] = 4242
            held["process_started"] = 500.0
            fh.seek(0)
            fh.truncate()
            json.dump(held, fh)
        with self.assertRaises(Locked) as caught:
            BoardLock(SERIAL, "us", probe=probe, home=self.home).acquire()
        self.assertIn("4242", str(caught.exception))
        self.assertIn("one user at a time", str(caught.exception))

    def test_a_stale_lock_is_named_and_taken_over(self):
        os.makedirs(os.path.join(self.home, "locks"))
        path = BoardLock(SERIAL, "x", home=self.home).path
        with open(path, "w") as fh:
            json.dump({"pid": 4242, "process_started": 500.0, "taken": "2026-10-18T10:00:00Z",
                       "command": "flash firmware.zip", "host": "bench"}, fh)
        said = []
        probe = FakeProbe({os.getpid(): 1.0})  # 4242 is not in the table: gone
        with BoardLock(SERIAL, "us", probe=probe, home=self.home, log=said.append) as lock:
            with open(lock.path) as fh:
                self.assertEqual(json.load(fh)["pid"], os.getpid())
        self.assertEqual(len(said), 1)
        self.assertIn("stale", said[0])
        self.assertIn("4242", said[0])
        self.assertIn("flash firmware.zip", said[0])

    def test_a_reused_pid_is_stale(self):
        os.makedirs(os.path.join(self.home, "locks"))
        path = BoardLock(SERIAL, "x", home=self.home).path
        with open(path, "w") as fh:
            json.dump({"pid": 4242, "process_started": 500.0, "taken": "t", "command": "c", "host": "h"}, fh)
        probe = FakeProbe({os.getpid(): 1.0, 4242: 9000.0})  # alive, but started much later
        said = []
        with BoardLock(SERIAL, "us", probe=probe, home=self.home, log=said.append):
            pass
        self.assertIn("reused", said[0])

    def test_the_cli_exits_locked(self):
        self.config()
        probe = FakeProbe({os.getpid(): 1.0, 4242: 500.0})
        os.makedirs(os.path.join(self.home, "locks"))
        with open(BoardLock(SERIAL, "x", home=self.home).path, "w") as fh:
            json.dump({"pid": 4242, "process_started": 500.0, "taken": "t", "command": "console", "host": "h"}, fh)
        ports = FakePorts(boards=[self.board()], clock=self.clock)
        code, out = self.run_cli(["console", "ver"], ports=ports, probe=probe)
        self.assertEqual(code, cfg.EXIT_LOCKED)
        self.assertIn("locked", out)
        self.assertEqual(self.ports.opened, [])


# ----------------------------------------------------------------------------- the console

class TheConsoleGuard(unittest.TestCase):
    def refused(self, line, disposable=False):
        with self.assertRaises(cfg.Refused) as caught:
            console.console_guard(line, disposable)
        return str(caught.exception)

    def test_password_lines_are_refused_unless_disposable(self):
        self.assertIn("disposable", self.refused("password hunter2"))
        self.assertIn("disposable", self.refused("set guest.password hello"))
        self.assertEqual(console.console_guard("password hunter2", disposable=True), "password hunter2")
        self.assertEqual(console.console_guard("set guest.password hello", disposable=True), "set guest.password hello")

    def test_the_private_key_is_never_set_or_printed(self):
        for disposable in (False, True):
            self.assertIn("private key", self.refused("set prv.key 00ff", disposable))
            self.assertIn("private key", self.refused("get prv.key", disposable))

    def test_erase_is_refused_unless_disposable(self):
        self.assertIn("identity", self.refused("erase"))
        self.assertEqual(console.console_guard("erase", disposable=True), "erase")

    def test_bootloader_lines_are_refused(self):
        for line in ("reboot 1", "reboot 2", "reboot 3", "dfu", "dfu serial", "DFU"):
            self.assertIn("bootloader", self.refused(line, disposable=True), line)
        self.assertEqual(console.console_guard("reboot"), "reboot")
        self.assertEqual(console.console_guard("clkreboot"), "clkreboot")

    def test_region_load_is_refused(self):
        self.assertIn("blank line", self.refused("region load"))
        self.assertEqual(console.console_guard("region save"), "region save")

    def test_empty_and_control_lines_are_refused(self):
        self.refused("")
        self.refused("   ")
        self.refused(None)
        self.refused("get\tname")

    def test_ordinary_lines_pass(self):
        # The clock's and the thresholds' lines are among them: docs/bench.md sections 3 and 5
        # are walks, and nothing of theirs is a line this file holds back (p3.5-04).
        for line in ("ver", "board", "get radio", "sensor list", "sensor get fw.version", "set name Bench",
                     "get guest.password", "advert", "neighbors", "info", "sweep", "stop", "reboot",
                     "clock", "clock sync", "time 1792000000", "sensor set power.low_mv 3600",
                     "sensor get clock.state", "get pwrmgt.bootmv"):
            self.assertEqual(console.console_guard(line), line)

    def test_redaction(self):
        text = "OK\npassword now: hunter2\n  Password Now: x\nnext"
        out = console.redact(text)
        self.assertNotIn("hunter2", out)
        self.assertNotIn("Now: x", out)
        self.assertIn("(not shown by tools/bench)", out)
        self.assertEqual(out.splitlines()[0], "OK")
        self.assertEqual(out.splitlines()[-1], "next")


class TheRepeaterConsole(BenchCase):
    def open(self, firmware=None, disposable=False, port_dies_on_reset=True):
        board = self.board(firmware)
        board.port_dies_on_reset = port_dies_on_reset
        ports = FakePorts(boards=[board], clock=self.clock)
        port = ports.open("COM7", 115200)
        said = []
        con = console.RepeaterConsole(port, log=said.append, disposable=disposable, now=ports.now, sleep=ports.sleep)
        return board, con, said

    def test_a_line_ends_in_cr_and_the_reply_follows_the_arrow(self):
        board, con, said = self.open()
        reply = con.send("ver")
        self.assertTrue(reply.ok)
        self.assertEqual(reply.text, "v1.18.0 (Build: 15 Oct 2026)")
        self.assertEqual(board.firmware.received, ["ver"])
        self.assertIn("> ver", said)

    def test_a_multi_line_reply_is_whole(self):
        fw = FakeRepeaterFirmware(REPEATER_ID, answers={"sensor list": "10 vars\nfw.version=x\nwalter.wake=0"})
        board, con, said = self.open(fw)
        reply = con.send("sensor list")
        self.assertEqual(reply.text.splitlines(), ["10 vars", "fw.version=x", "walter.wake=0"])

    def test_a_line_that_answers_nothing_times_out(self):
        board, con, said = self.open(port_dies_on_reset=False)
        reply = con.send("reboot", timeout_s=0.5)
        self.assertTrue(reply.timed_out)
        self.assertEqual(reply.text, "")
        self.assertEqual(board.firmware.rebooted, 1)

    def test_a_reset_takes_the_port_with_it_and_that_is_the_reset(self):
        """What the first RAK4630 on this bench did (p3.4-02): the USB device went down with the
        reset and the next read of the open handle raised instead of falling quiet. `send_reset`
        is the line for that; plain `send` would carry the error up."""
        board, con, said = self.open()
        with self.assertRaises(IOError):
            con.send("reboot", timeout_s=0.5)
        board, con, said = self.open()
        reply = con.send_reset("reboot")
        self.assertTrue(reply.timed_out)
        self.assertEqual(reply.text, "")
        self.assertEqual(board.firmware.rebooted, 1)
        self.assertIn("> reboot", said)
        self.assertTrue(any("went away with the reset" in line for line in said), said)

    def test_a_reset_line_that_the_board_answers_is_an_ordinary_reply(self):
        board, con, said = self.open(port_dies_on_reset=False)
        reply = con.send_reset("reboot")
        self.assertTrue(reply.timed_out)          # upstream's `reboot` answers nothing either way
        self.assertEqual(board.firmware.rebooted, 1)

    def test_the_password_echo_is_never_printed_nor_returned(self):
        board, con, said = self.open(disposable=True)
        reply = con.send("password hunter2")
        self.assertEqual(board.firmware.password, "hunter2")
        self.assertNotIn("hunter2", reply.text)
        self.assertNotIn("hunter2", reply.raw)
        self.assertNotIn("hunter2", "\n".join(said))
        self.assertIn("not shown", reply.text)
        # the typed line's echo is the same secret and is redacted too; an ordinary line is not
        self.assertNotIn("hunter2", console.redact("password hunter2\r\n  -> password now: hunter2"))
        self.assertEqual(console.redact("get guest.password\n  -> > hello"), "get guest.password\n  -> > hello")

    def test_a_refused_line_is_not_sent(self):
        board, con, said = self.open()
        with self.assertRaises(cfg.Refused):
            con.send("password hunter2")
        self.assertEqual(board.firmware.received, [])
        self.assertEqual(board.firmware.password, "not-a-password")

    def test_the_banner_is_read(self):
        board, con, said = self.open()
        banner = con.read_banner(2.0)
        self.assertEqual(banner["repeater_id"], REPEATER_ID)

    def test_the_cli_console_prints_the_reply_and_redacts(self):
        self.config(identity="disposable")
        ports = FakePorts(boards=[self.board()], clock=self.clock)
        code, out = self.run_cli(["console", "ver"], ports=ports)
        self.assertEqual(code, cfg.EXIT_OK)
        self.assertIn("v1.18.0", out)
        self.out = io.StringIO()
        code, out = self.run_cli(["console", "password hunter2"], ports=ports)
        self.assertEqual(code, cfg.EXIT_OK)
        self.assertNotIn("hunter2", out)
        self.assertIn("not shown", out)

    def test_the_cli_console_refuses_before_opening(self):
        self.config()
        ports = FakePorts(boards=[self.board()], clock=self.clock)
        for line in ("password x", "set guest.password x", "erase", "reboot 2", "get prv.key"):
            self.out = io.StringIO()
            code, out = self.run_cli(["console", line], ports=ports)
            self.assertEqual(code, cfg.EXIT_REFUSED, line)
            self.assertIn("refused", out)
        self.assertEqual(self.ports.opened, [])


class TheScannerConsole(BenchCase):
    def open(self, firmware=None):
        board = self.board(firmware or FakeScannerFirmware())
        ports = FakePorts(boards=[board], clock=self.clock)
        port = ports.open("COM7", 115200)
        return board, console.ScannerConsole(port, now=ports.now, sleep=ports.sleep)

    def test_lines_are_json_and_a_bad_one_is_kept_and_counted(self):
        batch = console.LineBatch()
        batch.take('{"t":"stat","up":1}\nnot json\n{"t":"ack","cmd":"x","ok":true}\n{"t": nan}\n')
        self.assertEqual(len(batch.objects), 2)
        self.assertEqual(batch.unparsed, ["not json", '{"t": nan}'])
        self.assertEqual(batch.unparsed_count, 2)
        self.assertEqual(batch.of_type("ack")[0]["cmd"], "x")

    def test_info_answers_an_info_line(self):
        board, con = self.open()
        boot = con.first("boot", 1.0)
        self.assertEqual(boot["t"], "boot")
        self.assertTrue(boot["radio_ok"])
        ack, batch = con.send("info", timeout_s=0.5)
        self.assertIsNone(ack)  # info answers an info line and no ack (src/roles/scanner/main.cpp)
        self.assertEqual(batch.of_type("info")[0]["rom"], "SX1261 V2D 2D02")
        self.assertEqual(board.firmware.received, ["info"])

    def test_a_command_gets_its_ack(self):
        board, con = self.open()
        ack, batch = con.send("sweep", timeout_s=0.5)
        self.assertEqual(ack["cmd"], "sweep")
        self.assertTrue(ack["ok"])

    def test_collect_counts_what_did_not_parse(self):
        fw = FakeScannerFirmware(extra_lines=["# chatter", '{"t":"stat","bad":nan}'])
        board, con = self.open(fw)
        ack, batch = con.send("stat", timeout_s=0.5)
        self.assertEqual(len(batch.of_type("stat")), 1)
        self.assertEqual(batch.unparsed_count, 2)
        self.assertEqual(batch.unparsed, ["# chatter", '{"t":"stat","bad":nan}'])
        self.assertEqual(con.collect(0.5).unparsed_count, 0)

    def test_dfu_is_refused_on_the_scanner_too(self):
        board, con = self.open()
        with self.assertRaises(cfg.Refused):
            con.send("dfu")
        self.assertEqual(board.firmware.received, [])

    def test_the_cli_collect(self):
        self.config()
        ports = FakePorts(boards=[self.board(FakeScannerFirmware())], clock=self.clock)
        code, out = self.run_cli(["console", "--scanner", "--collect", "1"], ports=ports)
        self.assertEqual(code, cfg.EXIT_OK)
        self.assertIn('"t": "boot"', out)
        self.assertIn("0 not JSON", out)

    def test_the_cli_sends_a_line_and_then_collects_on_the_same_port(self):
        # A `scope`'s frames - or its `err` - follow the ack, and a port opened for a second
        # command has already missed them (p3.5-02): the line and the collect share one open.
        self.config()
        board = self.board(FakeScannerFirmware(scope_frames=2, extra_lines=['{"t":"err","msg":"late"}']))
        ports = FakePorts(boards=[board], clock=self.clock)
        code, out = self.run_cli(["console", "--scanner", "scope 863 870", "--collect", "1"], ports=ports)
        self.assertEqual(code, cfg.EXIT_OK)
        self.assertEqual(board.firmware.received, ["scope 863 870"])
        self.assertIn('"cmd": "scope"', out)
        self.assertEqual(out.count('"t": "scope"'), 2)
        self.assertIn('"msg": "late"', out)
        self.assertIn("not JSON", out)
        self.assertEqual(len(ports.opened), 1)


# ----------------------------------------------------------------------------- a flash

def firmware_for(path):
    """What a fake DFU of a package puts on the fake board: the role the package's descriptor says."""
    with zipfile.ZipFile(path) as zf:
        _, desc = manifestref.find_descriptor(zf.read("firmware.bin"))
    if desc["role"] == "scanner":
        return FakeScannerFirmware(version=desc["version"])
    return FakeRepeaterFirmware(REPEATER_ID, version=desc["version"])


class TheProductIds(unittest.TestCase):
    """Which USB product ids are this board, and which of them is a bootloader (p3.4-02)."""

    def test_both_bootloader_ids_are_a_bootloader(self):
        for pid in (0x0029, 0x002A):
            info = PortInfo("COM31", SERIAL, RAK_VID, pid, "RAK4631 bootloader")
            self.assertTrue(info.is_bootloader, "0x%04X" % pid)
            self.assertFalse(info.is_application, "0x%04X" % pid)

    def test_both_application_ids_are_the_application(self):
        for pid in (0x8029, 0x802A):
            info = PortInfo("COM30", SERIAL, RAK_VID, pid, "WisCore RAK4631 Board")
            self.assertTrue(info.is_application, "0x%04X" % pid)
            self.assertFalse(info.is_bootloader, "0x%04X" % pid)

    def test_a_stranger_is_neither(self):
        info = PortInfo("COM9", "A50285BI", 0x0403, 0x6001, "FT232")
        self.assertFalse(info.is_application)
        self.assertFalse(info.is_bootloader)

    def test_the_ids_are_the_vendored_board_files_hwids(self):
        """boards/rak4631.json is where they come from, so the two lists cannot drift from the
        board file the build uses."""
        with open(os.path.join(REPO, "boards", "rak4631.json"), encoding="utf-8") as fh:
            hwids = json.load(fh)["build"]["hwids"]
        pairs = set((int(vid, 16), int(pid, 16)) for vid, pid in hwids)
        self.assertEqual(pairs, set((RAK_VID, pid) for pid in RAK_APP_PIDS + RAK_BOOT_PIDS))

    def test_the_board_is_found_in_the_serial_only_dfu(self):
        board = FakeBoard(serial=SERIAL, firmware=FakeRepeaterFirmware(REPEATER_ID),
                          boot_pid=0x002A)
        board.mode = "bootloader"
        ports = FakePorts(boards=[board], clock=FakeClock())
        app, boot = ports.find_board(SERIAL)
        self.assertIsNone(app)
        self.assertIsNotNone(boot)
        self.assertEqual(boot.device, "COM8")


class ThePortLayer(unittest.TestCase):
    """`Ports.open_ready`: a port that has just enumerated is opened with a retry (p3.4-02)."""

    class Counting(Ports):
        def __init__(self, fails, clock):
            self.fails = fails
            self.tries = 0
            self.clock = clock

        def open(self, device, baud):
            self.tries += 1
            if self.tries <= self.fails:
                raise IOError("could not open port %r: FileNotFoundError(2)" % device)
            return "a port"

        def sleep(self, seconds):
            self.clock.sleep(seconds)

        def now(self):
            return self.clock.now()

    def test_an_open_that_fails_twice_then_works(self):
        clock = FakeClock()
        ports = self.Counting(2, clock)
        self.assertEqual(ports.open_ready("COM30", 115200, timeout_s=10.0, poll_s=0.25), "a port")
        self.assertEqual(ports.tries, 3)
        self.assertAlmostEqual(clock.now() - 1000.0, 0.5, places=6)

    def test_an_open_that_never_works_raises_the_last_error_after_the_deadline(self):
        clock = FakeClock()
        ports = self.Counting(10 ** 6, clock)
        with self.assertRaises(IOError) as caught:
            ports.open_ready("COM30", 115200, timeout_s=2.0, poll_s=0.25)
        self.assertIn("FileNotFoundError", str(caught.exception))
        self.assertEqual(ports.tries, 9)                     # 2.0 s of 0.25 s polls, then one more
        self.assertGreaterEqual(clock.now() - 1000.0, 2.0)

    class Vanishing(Ports):
        """A port that is listed for a few polls and then is not (a board that reset)."""

        def __init__(self, polls, clock):
            self.polls = polls
            self.clock = clock

        def list(self):
            if self.polls > 0:
                self.polls -= 1
                return [PortInfo("COM31", SERIAL, RAK_VID, 0x002A, "RAK4631 bootloader")]
            return []

        def sleep(self, seconds):
            self.clock.sleep(seconds)

        def now(self):
            return self.clock.now()

    def test_a_port_that_goes_away_is_waited_for(self):
        clock = FakeClock()
        ports = self.Vanishing(3, clock)
        self.assertTrue(ports.wait_until_gone(lambda i: i.device == "COM31", 10.0, poll_s=0.2))
        self.assertAlmostEqual(clock.now() - 1000.0, 0.6, places=6)

    def test_a_port_that_stays_is_the_timeout(self):
        clock = FakeClock()
        ports = self.Vanishing(10 ** 6, clock)
        self.assertFalse(ports.wait_until_gone(lambda i: i.device == "COM31", 2.0, poll_s=0.5))
        self.assertGreaterEqual(clock.now() - 1000.0, 2.0)

    def test_an_open_that_works_at_once_sleeps_not_at_all(self):
        clock = FakeClock()
        ports = self.Counting(0, clock)
        self.assertEqual(ports.open_ready("COM30", 115200), "a port")
        self.assertEqual(ports.tries, 1)
        self.assertEqual(clock.now(), 1000.0)


class AFlash(BenchCase):
    def setUp(self):
        super().setUp()
        self.config()
        self.repeater_zip = self.package()
        self.scanner_zip = self.package(role="scanner", where="scanner_rak4630_single")

    def fakes(self, board=None, nrfutil_exit=0, nrfutil_exits=None):
        board = board or self.board()
        ports = FakePorts(boards=[board], clock=self.clock)
        runner = FakeRunner(boards=[board], nrfutil_exit=nrfutil_exit, nrfutil_exits=nrfutil_exits,
                            new_firmware=firmware_for)
        return board, ports, runner

    def test_the_whole_sequence(self):
        board, ports, runner = self.fakes()
        code, out = self.run_cli(["flash", self.scanner_zip], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        # the record came first: the console was opened at 115200 before the 1200-baud touch
        opened = self.ports.opened
        self.assertEqual(opened[0], ("COM7", 115200))
        self.assertIn(("COM7", 1200), opened)
        self.assertLess(opened.index(("COM7", 1200)), len(opened) - 1)
        self.assertEqual(board.touches, 1)
        # nrfutil, with the platform's flags, on the bootloader's port
        nrf = [c for c in runner.calls if "adafruit-nrfutil.py" in " ".join(c)]
        self.assertEqual(len(nrf), 1)
        self.assertEqual(nrf[0], nrfutil_argv("python", "/pio/packages/tool-adafruit-nrfutil/adafruit-nrfutil.py",
                                              "COM8", self.scanner_zip))
        self.assertEqual(board.flashes, [self.scanner_zip])
        # the board came back as the scanner and said so
        self.assertIn("the board says scanner 0.0.0-dev", out)
        # the run was recorded before the flash: a repeater, with its id
        run = load_run(self.home, SERIAL)
        self.assertEqual(run.role, "repeater")
        self.assertEqual(run.repeater_id, REPEATER_ID)
        self.assertEqual(run.version, "0.0.0-dev")
        self.assertEqual(run.name, "repeater")
        # and the flash is logged with the descriptor and the hashes, in both places
        for log_path in (os.path.join(self.home, "flash.log"), os.path.join(self.repo, "out", "bench", "flash.log")):
            with open(log_path) as fh:
                lines = [json.loads(l) for l in fh if l.strip()]
            self.assertEqual(len(lines), 1)
            self.assertEqual(lines[0]["descriptor"]["role"], "scanner")
            self.assertEqual(len(lines[0]["package_sha256"]), 64)
            self.assertEqual(len(lines[0]["application_sha256"]), 64)
            self.assertEqual(lines[0]["result"], "ok")
            self.assertEqual(lines[0]["serial"], SERIAL)
            # and with how long it took and how many DFUs it took: the measurement p3.5-03
            # derives WDT_TIMEOUT_S from, and the line that says a scanner was replaced in one.
            self.assertEqual(lines[0]["attempts"], 1)
            self.assertIsInstance(lines[0]["update_s"], float)
            self.assertIsInstance(lines[0]["dfu_s"], float)
            self.assertGreaterEqual(lines[0]["update_s"], lines[0]["dfu_s"])
        self.assertIn("the update took", out)

    def test_a_dfu_that_needed_a_second_attempt_says_two_in_the_log(self):
        """The two-DFU recovery p3.4-02 added is still there for a scanner image from before
        p3.5-03, and `attempts` is how a reader of the log tells the two cases apart."""
        board, ports, runner = self.fakes(self.board(), nrfutil_exits=[1, 0])
        code, out = self.run_cli(["flash", self.scanner_zip], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        self.assertIn("once more", out)
        with open(os.path.join(self.home, "flash.log")) as fh:
            lines = [json.loads(l) for l in fh if l.strip()]
        self.assertEqual(lines[-1]["attempts"], 2)
        self.assertEqual(lines[-1]["result"], "ok")

    def test_a_board_with_no_application_is_not_touched(self):
        """A board that shows only its bootloader has no application port to touch, so the tool
        goes straight to the bootloader's port it can already see: no open at 1200 baud, which is
        what the touch is (fakes.FakePort). p3.4-02 wrote this branch; this is the test that holds
        it (p3.5-01, deliverable 2)."""
        board, ports, runner = self.fakes(self.board(mode="bootloader"))
        code, out = self.run_cli(["flash", self.repeater_zip], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        self.assertEqual(board.touches, 0)
        self.assertEqual([pair for pair in self.ports.opened if pair[1] == 1200], [])
        self.assertIn("already in its bootloader", out)
        self.assertNotIn("the 1200-baud touch", out)
        self.assertEqual(board.flashes, [self.repeater_zip])

    def test_a_new_unit_records_the_image_it_is_about_to_be_given(self):
        """The normal case for every new unit (spec/SYSTEM.md S-5a): the Walter wrote its base and
        nothing else, so the board can say nothing of what it ran. What the run records is the
        file about to be written - the one thing that can be known - and that is what makes the
        run restorable (tasks/phase-3.4/FOLLOWUPS.md, p3.5-01)."""
        board, ports, runner = self.fakes(self.board(mode="bootloader"))
        code, out = self.run_cli(["flash", self.repeater_zip], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        run = load_run(self.home, SERIAL)
        self.assertEqual((run.role, run.version), ("repeater", "0.0.0-dev"))
        self.assertEqual(run.how, "bootloader")              # still not something a board said
        self.assertEqual(run.found_image, os.path.abspath(self.repeater_zip))
        self.assertEqual(run.found_descriptor["role"], "repeater")
        self.assertEqual(run.found_descriptor["board"], BOARD)
        self.assertIsNone(run.repeater_id)                   # there was no identity to read
        self.assertIn("the board is in its bootloader", out)
        self.assertIn("records the image it is about to be given", out)

    def test_a_new_unit_is_flashed_and_then_restored(self):
        """Deliverable 1 end to end: a fake board that enumerates only as the bootloader
        (239A:0029), flashed, then restored - and no capitals in the last line."""
        board, ports, runner = self.fakes(self.board(mode="bootloader"))
        self.assertEqual(board.boot_pid, 0x0029)
        code, out = self.run_cli(["flash", self.repeater_zip], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        self.out = io.StringIO()
        code, out = self.run_cli(["restore"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        last = out.strip().splitlines()[-1]
        self.assertTrue(last.startswith("the board arrived with only its bootloader; restored: "
                                        "the image the run began with"), last)
        self.assertIn("repeater 0.0.0-dev", last)
        self.assertIn("the image the run was given", last)
        self.assertIn("no Repeater ID was recorded", last)
        self.assertNotRegex(last, r"[A-Z]{4,}")              # nothing is said in capitals
        self.assertEqual(board.flashes, [self.repeater_zip, self.repeater_zip])
        self.assertIsNone(load_run(self.home, SERIAL))       # the run is closed

    def test_a_new_unit_flashed_with_found_restores_the_found_image(self):
        """`--found` still wins: a person who names the image that belongs on the board is saying
        what must go back on it."""
        given = self.package(name="given.zip", version="0.3.0+abc1234")
        board, ports, runner = self.fakes(self.board(mode="bootloader"))
        code, out = self.run_cli(["flash", self.scanner_zip, "--found", given], ports=ports,
                                 runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        run = load_run(self.home, SERIAL)
        self.assertEqual((run.role, run.version), ("repeater", "0.3.0+abc1234"))
        self.assertEqual(run.found_image, os.path.abspath(given))
        self.out = io.StringIO()
        code, out = self.run_cli(["restore"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        self.assertEqual(board.flashes[-1], given)

    def test_record_of_a_bootloader_only_board_with_an_image_is_restorable(self):
        """The same rule through `bench record --image`, which is what the first board needed by
        hand (p3.4-02): the image named is the role recorded."""
        board, ports, runner = self.fakes(self.board(mode="bootloader"))
        code, out = self.run_cli(["record", "--image", self.repeater_zip], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        self.assertNotIn("RECORDED NOTHING RESTORABLE", out)
        run = load_run(self.home, SERIAL)
        self.assertEqual((run.role, run.how), ("repeater", "bootloader"))
        self.assertEqual(run.found_image, os.path.abspath(self.repeater_zip))

    def test_record_of_a_bootloader_only_board_with_no_image_records_nothing(self):
        """And with no image named there is still nothing to record: a board in its bootloader is
        a run that cannot be put back, and it says so."""
        board, ports, runner = self.fakes(self.board(mode="bootloader"))
        code, out = self.run_cli(["record"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_FAILED, out)
        self.assertIn("RECORDED NOTHING RESTORABLE", out)
        run = load_run(self.home, SERIAL)
        self.assertIsNone(run.role)
        self.assertEqual(run.how, "bootloader")

    def test_a_port_listed_before_it_can_be_opened_is_waited_for(self):
        """The first flash of the first RAK4630 (p3.4-02): nrfutil said `Device programmed.`, the
        application enumerated, and the open of it failed with `FileNotFoundError(2)` because
        Windows lists a CDC port a shade before it can be opened. The port is waited for."""
        board = self.board()
        ports = SlowToOpenPorts(boards=[board], clock=self.clock, fails=3)
        runner = FakeRunner(boards=[board], new_firmware=firmware_for)
        code, out = self.run_cli(["flash", self.repeater_zip], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        self.assertEqual(ports.refused, 3)
        self.assertIn("the board says repeater 0.0.0-dev", out)
        self.assertEqual(board.flashes, [self.repeater_zip])
        self.assertEqual(load_run(self.home, SERIAL).role, "repeater")

    def test_a_port_that_never_opens_is_still_a_failure_that_says_double_tap(self):
        board = self.board()
        ports = SlowToOpenPorts(boards=[board], clock=self.clock, fails=10 ** 6)
        runner = FakeRunner(boards=[board], new_firmware=firmware_for)
        code, out = self.run_cli(["flash", self.repeater_zip], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_FAILED)
        self.assertIn("FileNotFoundError", out)
        self.assertIn("double-tap", out)
        # bounded: the retry gave up, it did not spin for ever
        self.assertGreater(ports.refused, 1)
        self.assertLess(ports.refused, 200)

    def test_the_bootloaders_port_is_opened_before_nrfutil_is_handed_it(self):
        """The port is listed before its endpoint will take bytes, and nrfutil's first write then
        failed with `PermissionError(13)` and wrote nothing (p3.4-02). The open is the proof."""
        board, ports, runner = self.fakes()
        code, out = self.run_cli(["flash", self.repeater_zip], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        opened = self.ports.opened
        self.assertIn(("COM8", 115200), opened)                    # the bootloader's own port
        self.assertLess(opened.index(("COM8", 115200)), len(opened) - 1)
        self.assertEqual(board.flashes, [self.repeater_zip])

    def test_a_bootloader_port_that_is_listed_before_it_opens_is_waited_for(self):
        board = self.board()
        ports = SlowToOpenPorts(boards=[board], clock=self.clock, fails=3, which="boot")
        runner = FakeRunner(boards=[board], new_firmware=firmware_for)
        code, out = self.run_cli(["flash", self.repeater_zip], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        self.assertEqual(ports.refused, 3)
        self.assertEqual(board.flashes, [self.repeater_zip])

    def test_a_touch_that_lands_in_the_serial_only_dfu_is_a_bootloader(self):
        """The touch's own DFU mode is product id 0x002A and not 0x0029 - what the first RAK4630
        on this bench did (p3.4-02): the walk of section 8 waited 15 s for a port that was
        already there under the other id."""
        board = self.board()
        board.boot_pid = 0x002A
        ports = FakePorts(boards=[board], clock=self.clock)
        runner = FakeRunner(boards=[board], new_firmware=firmware_for)
        code, out = self.run_cli(["flash", self.repeater_zip], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        self.assertEqual(board.touches, 1)
        self.assertEqual(board.flashes, [self.repeater_zip])
        self.assertIn("the board says repeater 0.0.0-dev", out)

    def test_a_dfu_that_did_not_take_is_run_once_more_from_the_bootloader(self):
        """What the first RAK4630 on this bench needed every time the touch came from the scanner
        (p3.4-02): nrfutil's first write was answered `ERROR_BAD_COMMAND`, the board stayed in a
        bootloader, and the same command from there worked."""
        board = self.board()
        ports = FakePorts(boards=[board], clock=self.clock)
        runner = FakeRunner(boards=[board], new_firmware=firmware_for, nrfutil_exits=[1])
        code, out = self.run_cli(["flash", self.scanner_zip], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        self.assertIn("still in its bootloader", out)
        self.assertEqual(board.flashes, [self.scanner_zip])            # written once, by the retry
        nrf = [c for c in runner.calls if "adafruit-nrfutil.py" in " ".join(c)]
        self.assertEqual(len(nrf), 2)
        self.assertEqual(board.touches, 1)                             # and touched only once

    def test_a_dfu_that_failed_while_exiting_zero_is_not_a_flash(self):
        """adafruit-nrfutil prints its traceback and exits 0 (p3.4-02). Without the `Device
        programmed.` line nothing was written, and the retry is what writes it."""
        board = self.board()
        ports = FakePorts(boards=[board], clock=self.clock)
        runner = FakeRunner(boards=[board], new_firmware=firmware_for)
        runner.nrfutil_fails_silently = 1
        code, out = self.run_cli(["flash", self.scanner_zip], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        self.assertIn("never said 'Device programmed.'", out)
        self.assertEqual(board.flashes, [self.scanner_zip])
        self.assertEqual(board.touches, 1)

    def test_a_dfu_that_never_says_it_programmed_the_device_is_a_failure(self):
        board = self.board()
        ports = FakePorts(boards=[board], clock=self.clock)
        runner = FakeRunner(boards=[board], new_firmware=firmware_for)
        runner.nrfutil_fails_silently = 2
        code, out = self.run_cli(["flash", self.scanner_zip], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_FAILED, out)
        self.assertIn("twice", out)
        self.assertEqual(board.flashes, [])

    def test_nrfutil_failing_says_double_tap(self):
        board, ports, runner = self.fakes(nrfutil_exit=1)
        code, out = self.run_cli(["flash", self.scanner_zip], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_FAILED)
        self.assertIn("twice", out)
        self.assertIn("double-tap", out)
        self.assertEqual(board.flashes, [])
        nrf = [c for c in runner.calls if "adafruit-nrfutil.py" in " ".join(c)]
        self.assertEqual(len(nrf), 2)                                  # one retry, and no more
        with open(os.path.join(self.home, "flash.log")) as fh:
            self.assertIn('"result": "failed', fh.read())

    def test_a_board_that_says_another_version_is_a_failure(self):
        board, ports, runner = self.fakes()
        runner.new_firmware = lambda path: FakeRepeaterFirmware(REPEATER_ID, version="9.9.9")
        code, out = self.run_cli(["flash", self.repeater_zip], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_FAILED)
        self.assertIn("does not say what the image said", out)

    def test_a_repeater_on_an_antenna_is_never_replaced(self):
        self.config(rf="antenna")
        board, ports, runner = self.fakes()
        code, out = self.run_cli(["flash", self.scanner_zip], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_REFUSED, out)
        self.assertIn("may not restore", out)
        self.assertEqual(board.touches, 0)
        self.assertEqual(board.flashes, [])

    def test_a_scanner_on_an_antenna_may_be_replaced_by_a_scanner(self):
        self.config(rf="antenna")
        board, ports, runner = self.fakes(self.board(FakeScannerFirmware()))
        code, out = self.run_cli(["flash", self.scanner_zip], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        run = load_run(self.home, SERIAL)
        self.assertEqual(run.role, "scanner")

    def test_a_second_flash_does_not_record_again(self):
        board, ports, runner = self.fakes()
        self.run_cli(["flash", self.scanner_zip], ports=ports, runner=runner)
        self.out = io.StringIO()
        code, out = self.run_cli(["flash", self.scanner_zip], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        self.assertNotIn("recording what is on the board", out)
        self.assertEqual(load_run(self.home, SERIAL).role, "repeater")

    def test_found_is_held_to_the_guards_too(self):
        board, ports, runner = self.fakes()
        bad = self.package(name="other.zip", board="rak4630-dual-a")
        code, out = self.run_cli(["flash", self.scanner_zip, "--found", bad], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_REFUSED)
        self.assertEqual(self.ports.opened, [])

    def test_nrfutil_missing_is_a_failure_that_says_how(self):
        board, ports, runner = self.fakes()
        bench = self.bench(ports=ports, runner=runner)
        bench._nrfutil = None
        bench.nrfutil  # resolves to the real location, which this test does not want
        bench._nrfutil = ""
        code = benchcli.main(["flash", self.scanner_zip], bench=bench)
        self.assertEqual(code, cfg.EXIT_FAILED)
        self.assertIn("adafruit-nrfutil is not here", self.out.getvalue())


# ----------------------------------------------------------------------------- record and restore

class RecordAndRestore(BenchCase):
    def setUp(self):
        super().setUp()
        self.config()
        self.repeater_zip = self.package()
        self.scanner_zip = self.package(role="scanner", where="scanner_rak4630_single")

    def fakes(self, board=None, **kw):
        board = board or self.board()
        ports = FakePorts(boards=[board], clock=self.clock, others=kw.pop("others", None))
        runner = FakeRunner(boards=[board], new_firmware=firmware_for, **kw)
        return board, ports, runner

    def test_record_reads_the_id_off_the_console_without_a_reset(self):
        """The banner cannot be caught on a board that has been up - the first RAK4630 on this
        bench gave it neither on the first open nor after a `reboot` (p3.4-02) - so upstream's own
        `get public.key` is asked, which is the same 32 bytes (CommonCLI.cpp:750 at 81dbae59)."""
        board, ports, runner = self.fakes()
        code, out = self.run_cli(["record"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        self.assertEqual(board.firmware.rebooted, 0)
        self.assertIn("get public.key", board.firmware.received)
        run = load_run(self.home, SERIAL)
        self.assertEqual(run.repeater_id, REPEATER_ID)
        self.assertEqual(run.how, "console (get public.key)")
        self.assertIn("recorded repeater 0.0.0-dev, Repeater ID", out)

    def test_record_falls_back_to_one_reboot_when_the_key_line_answers_nothing(self):
        board, ports, runner = self.fakes(
            self.board(FakeRepeaterFirmware(REPEATER_ID, answers={"get public.key": None})))
        code, out = self.run_cli(["record"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        self.assertEqual(board.firmware.rebooted, 1)
        run = load_run(self.home, SERIAL)
        self.assertEqual(run.repeater_id, REPEATER_ID)      # off the banner, after the reset
        self.assertEqual(run.how, "console")

    def test_record_over_the_link_when_the_dongle_is_there(self):
        self.config(dongle="A50285BI")
        dongle = PortInfo("COM9", "A50285BI", 0x0403, 0x6001, "FT232")
        linkcli = ("HELLO\n  protocol      1.0\n  role          1 (repeater)\n  fw_version    0.0.0-dev\n"
                   "  board         rak4630-single\nIDENTITY\n  pubkey        %s\n  name          Bench\n" % REPEATER_ID.lower())
        board, ports, runner = self.fakes(others=[dongle], linkcli_output=linkcli)
        code, out = self.run_cli(["record"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        run = load_run(self.home, SERIAL)
        self.assertEqual(run.repeater_id, REPEATER_ID)
        self.assertEqual(run.name, "Bench")
        self.assertTrue(run.how.startswith("link"))
        self.assertEqual(board.firmware.rebooted, 0)
        self.assertEqual(self.ports.opened, [])  # the console was never needed
        link_calls = [c for c in runner.calls if "linkcli.py" in " ".join(c)]
        self.assertEqual([c[-1] for c in link_calls], ["hello", "identity"])
        self.assertEqual(link_calls[0][link_calls[0].index("--port") + 1], "COM9")

    def test_record_of_a_scanner(self):
        board, ports, runner = self.fakes(self.board(FakeScannerFirmware(version="1.2.3", rom="SX1261 V2D 2D02")))
        code, out = self.run_cli(["record"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        run = load_run(self.home, SERIAL)
        self.assertEqual((run.role, run.version), ("scanner", "1.2.3"))
        self.assertEqual(run.board_said["rom"], "SX1261 V2D 2D02")

    def test_restore_puts_the_runs_own_image_back_and_checks_the_id(self):
        board, ports, runner = self.fakes()
        self.run_cli(["flash", self.scanner_zip], ports=ports, runner=runner)
        self.assertIsInstance(board.firmware, FakeScannerFirmware)
        self.out = io.StringIO()
        code, out = self.run_cli(["restore"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        last = out.strip().splitlines()[-1]
        self.assertTrue(last.startswith("restored: repeater back on the board"), last)
        self.assertIn("this run's own image of that role", last)
        self.assertIn("out/build/repeater_rak4630_single/firmware.zip", last)
        self.assertIn("Repeater ID %s matches" % REPEATER_ID, last)
        self.assertEqual(board.flashes[-1], self.repeater_zip)
        self.assertIsNone(load_run(self.home, SERIAL))

    def test_restore_prefers_the_image_the_run_was_given(self):
        given = self.package(name="given.zip", version="0.3.0+abc1234")
        board = self.board(FakeRepeaterFirmware(REPEATER_ID, version="0.3.0+abc1234"))
        board, ports, runner = self.fakes(board)
        self.run_cli(["flash", self.scanner_zip, "--found", given], ports=ports, runner=runner)
        self.out = io.StringIO()
        code, out = self.run_cli(["restore"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_OK, out)
        last = out.strip().splitlines()[-1]
        self.assertIn("the recorded version 0.3.0+abc1234", last)
        self.assertIn("the image the run was given", last)
        self.assertEqual(board.flashes[-1], given)

    def test_restore_with_a_changed_identity_says_so_in_capitals(self):
        board, ports, runner = self.fakes()
        self.run_cli(["flash", self.scanner_zip], ports=ports, runner=runner)
        runner.new_firmware = lambda path: FakeRepeaterFirmware(OTHER_ID)
        self.out = io.StringIO()
        code, out = self.run_cli(["restore"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_FAILED)
        last = out.strip().splitlines()[-1]
        self.assertTrue(last.startswith("RESTORE FAILED:"), last)
        self.assertIn("IDENTITY IS NOT WHAT IT WAS", last)
        self.assertIsNotNone(load_run(self.home, SERIAL))  # the run stays open

    def test_restore_with_nothing_recorded(self):
        board, ports, runner = self.fakes()
        code, out = self.run_cli(["restore"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_FAILED)
        self.assertTrue(out.strip().splitlines()[-1].startswith("RESTORE NOT POSSIBLE:"))
        self.assertEqual(self.ports.opened, [])
        # and with a run open but the board gone, it is no bench and not a failure
        save_run(self.home, RunRecord(serial=SERIAL, role="repeater", version="0.0.0-dev", started="now", how="console"))
        self.out = io.StringIO()
        code, out = self.run_cli(["restore"])
        self.assertEqual(code, cfg.EXIT_NO_BENCH)

    def test_restore_with_no_role_recorded_still_says_so_in_capitals(self):
        """The capitals have not gone: a record with no role - a board recorded in its bootloader
        with no image named - is still a run that nothing can put back."""
        board, ports, runner = self.fakes(self.board(mode="bootloader"))
        self.run_cli(["record"], ports=ports, runner=runner)
        self.out = io.StringIO()
        code, out = self.run_cli(["restore"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_FAILED)
        self.assertIn("RESTORE NOT POSSIBLE: THE RUN RECORDED NO RECOGNISABLE ROLE", out)
        self.assertEqual(board.flashes, [])

    def test_restore_without_a_build_of_that_role(self):
        board, ports, runner = self.fakes()
        self.run_cli(["flash", self.scanner_zip], ports=ports, runner=runner)
        os.remove(self.repeater_zip)
        self.out = io.StringIO()
        code, out = self.run_cli(["restore"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_FAILED)
        self.assertIn("NO BUILD OF THE REPEATER ROLE", out)

    def test_restore_of_a_repeater_image_obeys_the_rf_guard(self):
        """A run opened on the dummy load, the load then swapped for an antenna: the repeater is
        not flashed back on the air, and the last line says so."""
        board, ports, runner = self.fakes()
        self.run_cli(["flash", self.scanner_zip], ports=ports, runner=runner)
        self.config(rf="antenna")
        self.out = io.StringIO()
        code, out = self.run_cli(["restore"], ports=ports, runner=runner)
        self.assertEqual(code, cfg.EXIT_REFUSED)
        self.assertIn("transmits", out)
        self.assertEqual(board.flashes, [self.scanner_zip])


if __name__ == "__main__":
    unittest.main(verbosity=2)
