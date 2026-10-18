"""The rig, described and not discovered: bench.local.json.

A session may touch a board only through tools/bench/ (tasks/RUNBOOK.md section 2), and the
tool touches only the board this file names. The file is git-ignored - it carries the serial
numbers of one machine's USB devices - and bench.example.json at the repository root is the
committed example.

    {
      "board": "rak4630-single",       the board as spec/FIRMWARE-MANIFEST.md spells it
      "serial": "E6614C311B5C2E35",    the RAK's USB serial number: the nRF52840's FICR DEVICEID,
                                       which the Adafruit core's application and (as far as a
                                       reading can tell) the Adafruit bootloader both report
      "rf": "dummy-load",              what is on the antenna port: dummy-load | antenna | nothing
      "identity": "keep",              keep | disposable - whether the node's identity may be
                                       thrown away (docs/bench.md section 10 erases it)
      "dongle_serial": "A50285BI",     optional: a 3.3 V USB-UART dongle on the link pins
      "bootloader_serial": "..."       optional: only if the bootloader's port reports another
                                       serial number than the application's; defaults to serial
    }

No file, a file that does not describe a bench, or no port with that serial number present:
every command exits with EXIT_NO_BENCH and says why. That is not a failure - the walk goes to
"Not verified here", as it did before this tool existed. The tool never picks "the only COM
port" by itself: a board is a serial number or it is not a bench.
"""

import json
import os
import re

BENCH_FILE = "bench.local.json"

RF_PORTS = ("dummy-load", "antenna", "nothing")
IDENTITY_MODES = ("keep", "disposable")

# Exit codes. Documented here and in the CLI's --help; tasks/RUNBOOK.md section 2 names the
# first one by meaning.
EXIT_OK = 0
EXIT_FAILED = 1       # the board did not do what was asked (a flash that did not take, a
                      # restore whose identity does not match)
EXIT_REFUSED = 2      # a guard said no before any port was opened (an image, a console line)
EXIT_NO_BENCH = 3     # no bench.local.json, or no port with its serial number: not a failure
EXIT_LOCKED = 4       # another process holds this board


class NoBench(Exception):
    """There is no bench this tool may use. The message says why; the exit code is EXIT_NO_BENCH."""


class Refused(Exception):
    """A guard refused before any port was opened. The message is the reason; EXIT_REFUSED."""


class BenchConfig(object):
    def __init__(self, board, serial, rf, identity, dongle_serial=None, bootloader_serial=None,
                 source=None):
        self.board = board
        self.serial = serial
        self.rf = rf
        self.identity = identity
        self.dongle_serial = dongle_serial
        self.bootloader_serial = bootloader_serial or serial
        self.source = source

    @property
    def disposable(self):
        return self.identity == "disposable"

    def __repr__(self):
        return "BenchConfig(board=%r, serial=%r, rf=%r, identity=%r, dongle=%r)" % (
            self.board, self.serial, self.rf, self.identity, self.dongle_serial)


_SERIAL = re.compile(r"^[0-9A-Za-z._:-]{4,64}$")


def parse_config(text, source="bench.local.json"):
    """The dictionary of bench.local.json as a BenchConfig, or NoBench with the reason."""
    try:
        data = json.loads(text)
    except ValueError as exc:
        raise NoBench("%s is not JSON (%s): no bench" % (source, exc))
    if not isinstance(data, dict):
        raise NoBench("%s is not a JSON object: no bench" % source)

    def text_field(key, required=True):
        value = data.get(key)
        if value is None:
            if required:
                raise NoBench("%s has no %r: no bench" % (source, key))
            return None
        if not isinstance(value, str) or not value.strip():
            raise NoBench("%s: %r must be a non-empty string: no bench" % (source, key))
        return value.strip()

    board = text_field("board")
    serial = text_field("serial")
    if not _SERIAL.match(serial):
        raise NoBench("%s: %r is not a USB serial number: no bench" % (source, serial))
    rf = text_field("rf")
    if rf not in RF_PORTS:
        raise NoBench("%s: rf is %r, not one of %s: no bench" % (source, rf, ", ".join(RF_PORTS)))
    identity = text_field("identity")
    if identity not in IDENTITY_MODES:
        raise NoBench("%s: identity is %r, not one of %s: no bench"
                      % (source, identity, ", ".join(IDENTITY_MODES)))
    dongle = text_field("dongle_serial", required=False)
    boot_serial = text_field("bootloader_serial", required=False)
    for key in (dongle, boot_serial):
        if key is not None and not _SERIAL.match(key):
            raise NoBench("%s: %r is not a USB serial number: no bench" % (source, key))
    return BenchConfig(board, serial, rf, identity, dongle, boot_serial, source=source)


def load_config(repo_root):
    """bench.local.json at the repository root, or NoBench."""
    path = os.path.join(repo_root, BENCH_FILE)
    if not os.path.isfile(path):
        raise NoBench("no %s at %s: no bench on this machine (bench.example.json is the shape)"
                      % (BENCH_FILE, repo_root))
    with open(path, "r", encoding="utf-8") as fh:
        return parse_config(fh.read(), source=path)


def data_dir():
    """Where the tool keeps what must outlive a checkout: the lock per board, the run records and
    the flash log. Under the user's local application data, as the task asks; TWINOAK_BENCH_HOME
    overrides it (the tests use that)."""
    override = os.environ.get("TWINOAK_BENCH_HOME")
    if override:
        return override
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA") or os.path.join(os.path.expanduser("~"), "AppData", "Local")
        return os.path.join(base, "twinoak", "bench")
    xdg = os.environ.get("XDG_DATA_HOME")
    base = xdg if xdg else os.path.join(os.path.expanduser("~"), ".local", "share")
    return os.path.join(base, "twinoak-bench")
