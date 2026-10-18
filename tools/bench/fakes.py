"""A board that is not there: the fake serial layer, a fake nrfutil and a fake process table.

tools/bench/test_bench.py proves every guard with these - a refusal opens no port, a flash goes
through the same four steps the real one would - and phase 3.3's task 03 drives its walks
against the same fake board. Nothing here imports pyserial or touches a device.

FakeBoard is a RAK4630 as the bench sees it over USB: an application port while a role runs,
a bootloader port after the 1200-baud touch, the application port again after a DFU. It carries
one of two fake consoles, the repeater's (`  -> ` replies out of a table, `Repeater ID:` on boot)
or the scanner's (NDJSON with a `boot` line, an `ack` per command and an `info` line).
"""

import json
import os
import shlex

from ports import PortInfo, Ports, RAK_APP_PID, RAK_BOOT_PID, RAK_VID

# ----------------------------------------------------------------------------- a fake clock


class FakeClock(object):
    def __init__(self, start=1000.0):
        self.t = start

    def now(self):
        return self.t

    def sleep(self, seconds):
        self.t += max(seconds, 0.0)


# ----------------------------------------------------------------------------- the consoles


class FakeRepeaterFirmware(object):
    """Upstream's console, as far as the bench needs it."""

    def __init__(self, repeater_id, version="0.0.0-dev", name="repeater",
                 upstream="repeater-v1.17.1 (d9296435)", answers=None):
        self.repeater_id = repeater_id
        self.version = version
        self.name = name
        self.upstream = upstream
        self.password = "not-a-password"
        self.guest_password = ""
        self.answers = dict(answers or {})
        self.received = []
        self.rebooted = 0
        self.erased = 0

    def banner(self):
        return "Repeater ID: %s\r\n" % self.repeater_id

    def fw_version(self):
        return "twinoak-repeater %s on MeshCore %s" % (self.version, self.upstream)

    def handle(self, line):
        """The reply text for one line, or None for a line that answers nothing."""
        self.received.append(line)
        if line in self.answers:
            return self.answers[line]
        if line == "ver":
            return "v1.17.1 (Build: 14 Aug 2026)"
        if line == "board":
            return "RAK 4631"
        if line == "get radio":
            return "> 869.618,62.5,8,8"
        if line == "get name":
            return "> %s" % self.name
        if line.startswith("set name "):
            self.name = line[len("set name "):]
            return "OK"
        if line == "sensor get fw.version":
            return "> %s" % self.fw_version()
        if line.startswith("password "):
            self.password = line[len("password "):]
            return "password now: %s" % self.password
        if line.startswith("set guest.password "):
            self.guest_password = line[len("set guest.password "):]
            return "OK"
        if line == "get guest.password":
            return "> %s" % self.guest_password
        if line == "get prv.key":
            return "> deadbeef" * 8
        if line == "erase":
            self.erased += 1
            return "File system erase: OK"
        if line == "reboot":
            self.rebooted += 1
            return None
        if line == "get dutycycle":
            return "> 10.0%"
        return "Unknown command: %s" % line


class FakeScannerFirmware(object):
    """NoiseScope's console, as far as the bench needs it (docs/noisescope.md section 5)."""

    def __init__(self, version="0.0.0-dev", rom="SX1261 V2D 2D02", radio_ok=True,
                 devid="deadbeef01020304", extra_lines=None):
        self.version = version
        self.rom = rom
        self.radio_ok = radio_ok
        self.devid = devid
        self.received = []
        self.extra_lines = list(extra_lines or [])  # lines pushed after the next command

    def identity(self):
        return {"fw": "noisescope", "ver": self.version, "role": "scanner", "board": "rak4630-single",
                "hw": "rak4630", "radio": "sx1262"}

    def banner(self):
        boot = dict(self.identity())
        boot.update({"t": "boot", "reset": "soft", "radio_ok": self.radio_ok, "rtc": 0, "ina": 0, "ts": 0})
        return json.dumps(boot) + "\n"

    def handle(self, line):
        """The lines the scanner answers with, as text."""
        self.received.append(line)
        words = line.split()
        word = words[0].lower() if words else ""
        out = []
        if word == "info":
            info = dict(self.identity())
            info.update({"t": "info", "build": "Oct 18 2026", "devid": self.devid, "reset": "soft",
                         "rtc": 0, "ina": 0, "radio_ok": 1 if self.radio_ok else 0, "boost": 1,
                         "auto_min": 10, "rom": self.rom,
                         "home": {"f": 869.618, "bw": 62.5, "sf": 8, "cr": 8}})
            out.append(json.dumps(info))
        elif word == "stat":
            out.append(json.dumps({"t": "stat", "ts": 0, "ms": 12345, "up": 12, "mode": "monitor",
                                   "home": 869.618, "rssi": {"n": 9900, "avg": -114.2, "max": -99.5,
                                                             "min": -116.0, "dc": 0.03, "th": -106.0}}))
        elif word in ("sweep", "hist", "dwell", "cad", "stop", "scope", "home", "boost", "auto", "time",
                      "help"):
            out.append(json.dumps({"t": "ack", "cmd": word, "ok": True, "msg": "started"}))
        else:
            out.append(json.dumps({"t": "ack", "cmd": word, "ok": False, "msg": "unknown command"}))
        out.extend(self.extra_lines)
        self.extra_lines = []
        return "".join(ln + "\n" for ln in out)


# ----------------------------------------------------------------------------- the board


class FakeBoard(object):
    """The RAK as USB sees it. `firmware` is a FakeRepeaterFirmware or a FakeScannerFirmware;
    flash() swaps it for what a DFU package said it was (the test decides)."""

    def __init__(self, serial="E6614C311B5C2E35", firmware=None, app_device="COM7", boot_device="COM8",
                 enumerate_polls=2, bootloader_serial=None):
        self.serial = serial
        self.bootloader_serial = bootloader_serial or serial
        self.firmware = firmware or FakeRepeaterFirmware("AB" * 32)
        self.app_device = app_device
        self.boot_device = boot_device
        self.mode = "application"       # application | bootloader | absent
        self.enumerate_polls = enumerate_polls
        self._polls_left = 0
        self._next_mode = None
        self.touches = 0
        self.flashes = []
        self.app_port = None
        self._fresh = True      # the banner is waiting to be printed to the first host that opens it

    # what list() sees
    def infos(self):
        if self._polls_left > 0:
            self._polls_left -= 1
            if self._polls_left == 0 and self._next_mode:
                self.mode, self._next_mode = self._next_mode, None
            return []
        if self.mode == "application":
            return [PortInfo(self.app_device, self.serial, RAK_VID, RAK_APP_PID, "WisCore RAK4631 Board")]
        if self.mode == "bootloader":
            return [PortInfo(self.boot_device, self.bootloader_serial, RAK_VID, RAK_BOOT_PID, "RAK4631 bootloader")]
        return []

    def _transition(self, to):
        self._next_mode = to
        self._polls_left = self.enumerate_polls
        self.mode = "absent"

    def touched(self):
        """The 1200-baud touch: the application resets into the serial-only bootloader."""
        self.touches += 1
        self._transition("bootloader")

    def dfu(self, package_path, new_firmware=None):
        """What nrfutil does: the bootloader writes the application and resets into it."""
        self.flashes.append(package_path)
        if new_firmware is not None:
            self.firmware = new_firmware
        self._transition("application")

    def reboot(self):
        self._transition("application")


class FakePort(object):
    """One open port to a FakeBoard's console. Bytes written are lines to the firmware; bytes read
    are what it printed. A port opened at 1200 baud and closed is the touch."""

    def __init__(self, board, device, baud, ports):
        self.board = board
        self.device = device
        self.baud = baud
        self.ports = ports
        self.closed = False
        self._in = b""
        self._out = b""
        self.is_boot = device == board.boot_device
        if not self.is_boot and baud == 115200:
            # the banner a freshly booted board printed: only if the board just came up
            if board.app_port is None and board._fresh:
                self._out += board.firmware.banner().encode("utf-8")
            board.app_port = self
            board._fresh = False

    def write(self, data):
        if self.is_boot:
            return
        self._in += data
        fw = self.board.firmware
        if isinstance(fw, FakeRepeaterFirmware):
            # echo as typed, as upstream's console does
            self._out += data.replace(b"\r", b"\r\n")
            while b"\r" in self._in:
                line, self._in = self._in.split(b"\r", 1)
                text = line.decode("utf-8", errors="replace")
                reply = fw.handle(text)
                if text == "reboot":
                    self.board._fresh = True
                    self.board.app_port = None
                    self.board.reboot()
                    return
                if reply is not None:
                    self._out += ("  -> %s\r\n" % reply).encode("utf-8")
        else:
            while b"\n" in self._in:
                line, self._in = self._in.split(b"\n", 1)
                text = line.decode("utf-8", errors="replace").strip()
                if text:
                    self._out += fw.handle(text).encode("utf-8")

    def read(self, n, timeout_s):
        if self._out:
            out, self._out = self._out[:n], self._out[n:]
            return out
        self.ports.clock.sleep(timeout_s)
        return b""

    def close(self):
        if self.closed:
            return
        self.closed = True
        if not self.is_boot and self.baud == 1200 and self.board.mode == "application":
            self.board.touched()
        if self.board.app_port is self:
            self.board.app_port = None
            self.board._fresh = True


class FakePorts(Ports):
    """The serial layer with no serial in it. `opened` is every open() ever made - the proof a
    refusal opened nothing. `boards` may be empty (no board on this machine) or hold one FakeBoard;
    `others` are PortInfos of unrelated ports (a dongle, a stranger's device)."""

    def __init__(self, boards=None, others=None, clock=None):
        self.boards = list(boards or [])
        self.others = list(others or [])
        self.clock = clock or FakeClock()
        self.opened = []

    def list(self):
        out = []
        for board in self.boards:
            out.extend(board.infos())
        out.extend(self.others)
        return out

    def open(self, device, baud):
        self.opened.append((device, baud))
        for board in self.boards:
            if device in (board.app_device, board.boot_device):
                if device == board.app_device and board.mode != "application":
                    raise IOError("could not open port %s: the device is not there" % device)
                if device == board.boot_device and board.mode != "bootloader":
                    raise IOError("could not open port %s: the device is not there" % device)
                return FakePort(board, device, baud, self)
        raise IOError("could not open port %s: no such port" % device)

    def sleep(self, seconds):
        self.clock.sleep(seconds)

    def now(self):
        return self.clock.now()


# ----------------------------------------------------------------------------- the tools


class FakeRunner(object):
    """A subprocess runner that runs nothing. adafruit-nrfutil becomes a DFU on the fake board;
    linkcli.py answers out of a table. `calls` is every argv ever given."""

    def __init__(self, boards=None, nrfutil_exit=0, linkcli_output=None, new_firmware=None,
                 linkcli_exit=0):
        self.boards = list(boards or [])
        self.nrfutil_exit = nrfutil_exit
        self.linkcli_output = linkcli_output or ""
        self.linkcli_exit = linkcli_exit
        self.new_firmware = new_firmware
        self.calls = []

    def run(self, argv, timeout_s=None):
        self.calls.append(list(argv))
        joined = " ".join(os.path.basename(str(a)) for a in argv)
        if "adafruit-nrfutil.py" in joined:
            if self.nrfutil_exit != 0:
                return self.nrfutil_exit, "Failed to upgrade target. Error is: Timeout"
            port = argv[argv.index("-p") + 1]
            pkg = argv[argv.index("-pkg") + 1]
            for board in self.boards:
                if port == board.boot_device and board.mode == "bootloader":
                    fw = self.new_firmware(pkg) if callable(self.new_firmware) else self.new_firmware
                    board.dfu(pkg, fw)
                    return 0, "Upgrading target on %s with DFU package %s. Flow control is disabled, Dual bank, Touch disabled\nActivating new firmware\nDevice programmed." % (port, pkg)
            return 1, "Failed to upgrade target. Error is: no bootloader on %s" % port
        if "linkcli.py" in joined:
            return self.linkcli_exit, self.linkcli_output
        return 127, "no such program: %s" % shlex.join(str(a) for a in argv)


class FakeProbe(object):
    """A process table: {pid: start time}. A pid that is not in it is dead."""

    def __init__(self, alive=None):
        self.alive = dict(alive or {})

    def is_alive(self, pid):
        return pid in self.alive

    def started_at(self, pid):
        return self.alive.get(pid)
