"""Flashing, and only one kind: the DFU package through adafruit-nrfutil, after the guards.

The sequence is the one PlatformIO's nordicnrf52 platform runs for `upload_protocol = nrfutil`
(~/.platformio/platforms/nordicnrf52/builder/main.py, BeforeUpload and the nrfutil branch;
boards/rak4631.json says use_1200bps_touch and wait_for_upload_port), done here step by step so
that each step can be refused, logged and faked:

  1. the 1200-baud touch: the application's port opened at 1200 baud and closed. The Adafruit
     core's CDC sees DTR drop at 1200 and resets into the serial-only bootloader
     (libraries/Adafruit_TinyUSB_Arduino/src/arduino/Adafruit_USBD_CDC.cpp, tud_cdc_line_state_cb,
     at the core's pinned commit d541301; cores/nRF5/wiring.c, enterSerialDfu). A board found
     already in its bootloader skips this step;
  2. wait for the bootloader's port - the same serial number, product id 0x0029 - because on
     Windows it is not always the same COM number as the application's;
  3. `adafruit-nrfutil dfu serial -p <that port> -b 115200 --singlebank -pkg <image>`, the
     platform's own flags, run as a subprocess of the copy PlatformIO bundles
     (packages/tool-adafruit-nrfutil, version 1.503.0 = adafruit-nrfutil 0.5.3.post16 - pinned by
     the platform, platformio/nordicnrf52@11.0.0 in platformio.ini). Its own `--touch` is not
     used: step 1 is it, with the re-enumeration handled here;
  4. wait for the application's port to come back, and read what the board now says it is.

No UF2, no SWD, no erase, no `reboot 2`/`3`: nothing in this file knows how to do any of them.
Every flash is logged with the image's descriptor and SHA-256 (flash.log under the tool's data
directory, and a copy under out/bench/ beside the rest of the evidence).
"""

import json
import os
import subprocess
import sys
import time

from config import NoBench
from console import RepeaterConsole, ScannerConsole
from ports import same_serial

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

NRFUTIL_PACKAGE = "tool-adafruit-nrfutil"
NRFUTIL_SCRIPT = "adafruit-nrfutil.py"
NRFUTIL_BAUD = "115200"


class FlashError(Exception):
    """The board did not take the image, or did not come back. The message says what to do."""


DOUBLE_TAP = ("If the board shows no port at all now: double-tap the reset button on the adapter's "
              "USB-C side - the bootloader comes up (its drive appears) and `bench flash` can be run "
              "again against it. That is a person's hand; the bootloader is untouched, so it is all "
              "it takes.")


class Timeouts(object):
    def __init__(self, bootloader_port_s=15.0, application_port_s=40.0, banner_s=6.0,
                 nrfutil_s=240.0, settle_s=0.5):
        self.bootloader_port_s = bootloader_port_s
        self.application_port_s = application_port_s
        self.banner_s = banner_s
        self.nrfutil_s = nrfutil_s
        self.settle_s = settle_s


class SubprocessRunner(object):
    """Runs a program and returns (exit code, its output). The fake in fakes.py has the same shape."""

    def run(self, argv, timeout_s=None):
        try:
            done = subprocess.run([str(a) for a in argv], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  timeout=timeout_s)
        except subprocess.TimeoutExpired:
            return 124, "timed out after %s s" % timeout_s
        except OSError as exc:
            return 127, str(exc)
        return done.returncode, done.stdout.decode("utf-8", errors="replace")


def platformio_core_dir():
    return os.environ.get("PLATFORMIO_CORE_DIR") or os.path.join(os.path.expanduser("~"), ".platformio")


def locate_nrfutil(core_dir=None):
    """PlatformIO's bundled adafruit-nrfutil, or None. Installed with the platform on the first
    build, so a machine that has built this firmware has it."""
    path = os.path.join(core_dir or platformio_core_dir(), "packages", NRFUTIL_PACKAGE, NRFUTIL_SCRIPT)
    return path if os.path.isfile(path) else None


def nrfutil_argv(python, nrfutil, port_device, package):
    return [python, nrfutil, "dfu", "serial", "-p", port_device, "-b", NRFUTIL_BAUD, "--singlebank",
            "-pkg", package]


class FlashOutcome(object):
    def __init__(self, bootloader_device, application_port, touched, nrfutil_output):
        self.bootloader_device = bootloader_device
        self.application_port = application_port
        self.touched = touched
        self.nrfutil_output = nrfutil_output
        self.board_says = {}


def flash_image(image, config, ports, runner, log, nrfutil, python=None, timeouts=None):
    """Steps 1 to 4 over an Image that passed every guard. Raises FlashError or NoBench."""
    timeouts = timeouts or Timeouts()
    python = python or sys.executable
    if not nrfutil:
        raise FlashError("adafruit-nrfutil is not here: PlatformIO installs it with the platform "
                         "(%s/packages/%s). Build once - `pio run` - and it is." % (platformio_core_dir(), NRFUTIL_PACKAGE))

    app, boot = ports.find_board(config.serial, config.bootloader_serial)
    if app is None and boot is None:
        raise NoBench("no port with serial number %s: the board is not on this machine (no bench)" % config.serial)

    touched = False
    if boot is None:
        log("1. the 1200-baud touch on %s" % app.device)
        port = ports.open(app.device, 1200)
        ports.sleep(0.1)
        port.close()
        touched = True
        log("2. waiting up to %.0f s for the bootloader's port (serial %s, pid 0x%04X)"
            % (timeouts.bootloader_port_s, config.bootloader_serial, 0x0029))
        boot = ports.wait_for(lambda i: i.is_bootloader and same_serial(i.serial_number, config.bootloader_serial),
                              timeouts.bootloader_port_s)
        if boot is None:
            raise FlashError("the bootloader's port did not appear within %.0f s of the touch. Nothing "
                             "was written. %s" % (timeouts.bootloader_port_s, DOUBLE_TAP))
        ports.sleep(timeouts.settle_s)
    else:
        log("1-2. the board is already in its bootloader on %s (no touch needed)" % boot.device)

    argv = nrfutil_argv(python, nrfutil, boot.device, image.path)
    log("3. %s" % " ".join(os.path.basename(a) if i < 2 else a for i, a in enumerate(argv)))
    code, output = runner.run(argv, timeout_s=timeouts.nrfutil_s)
    for line in (output or "").splitlines():
        if line.strip():
            log("   nrfutil: %s" % line.rstrip())
    if code != 0:
        raise FlashError("adafruit-nrfutil exited %d: the image was not activated. The bootloader is "
                         "untouched and the board should still be in it - `bench flash` again. %s"
                         % (code, DOUBLE_TAP))

    log("4. waiting up to %.0f s for the application's port (serial %s, pid 0x%04X)"
        % (timeouts.application_port_s, config.serial, 0x8029))
    ports.sleep(timeouts.settle_s)
    app = ports.wait_for(lambda i: i.is_application and same_serial(i.serial_number, config.serial),
                         timeouts.application_port_s)
    if app is None:
        raise FlashError("the application did not enumerate within %.0f s of the flash. %s"
                         % (timeouts.application_port_s, DOUBLE_TAP))
    return FlashOutcome(boot.device, app, touched, output)


# ----------------------------------------------------------------------------- what the board says

_FW_VERSION_PREFIX = "> twinoak-"


def parse_fw_version(reply_text):
    """`> twinoak-<role> <version> on MeshCore <tag> (<pin>)` -> (role, version) or (None, None)."""
    text = (reply_text or "").strip()
    if not text.startswith(_FW_VERSION_PREFIX):
        return None, None
    words = text[len("> "):].split()
    if len(words) < 2 or not words[0].startswith("twinoak-"):
        return None, None
    return words[0][len("twinoak-"):], words[1]


def read_repeater(ports, app_info, log, disposable, timeouts, expect_banner=True, reboot_for_id=True):
    """Open a repeater's console and read what it is: the banner's Repeater ID (if it can be caught)
    and `sensor get fw.version`. Returns {role, version, repeater_id, name, how}."""
    port = ports.open(app_info.device, 115200)
    try:
        console = RepeaterConsole(port, log=log, disposable=disposable, now=ports.now, sleep=ports.sleep)
        banner = {"repeater_id": None, "first_boot": []}
        if expect_banner:
            banner = console.read_banner(timeouts.banner_s)
        else:
            console.drain(0.3)
        reply = console.send("sensor get fw.version")
        role, version = parse_fw_version(reply.text)
        name = None
        if role is not None:
            got = console.send("get name")
            if got.ok and got.text.startswith("> "):
                name = got.text[2:].strip()
        found = {"role": role, "version": version, "repeater_id": banner["repeater_id"],
                 "first_boot": banner["first_boot"], "name": name, "how": "console"}
        if role is not None and found["repeater_id"] is None and reboot_for_id:
            # The banner is printed one second after boot and only while a host holds the port
            # (Adafruit_USBD_CDC::write drops bytes while tud_cdc_connected() is false), so it can
            # be missed. One `reboot` - upstream's own, the clock file written first - and the port
            # is reopened as soon as it is back.
            log("the boot banner was not caught; `reboot` once to read Repeater ID")
            console.send("reboot", timeout_s=0.5)
            port.close()
            port = None
            ports.sleep(timeouts.settle_s)
            again = ports.wait_for(lambda i: i.is_application and same_serial(i.serial_number, app_info.serial_number),
                                   timeouts.application_port_s)
            if again is None:
                raise FlashError("the board did not come back after `reboot`. %s" % DOUBLE_TAP)
            port = ports.open(again.device, 115200)
            console = RepeaterConsole(port, log=log, disposable=disposable, now=ports.now, sleep=ports.sleep)
            banner = console.read_banner(timeouts.banner_s)
            found["repeater_id"] = banner["repeater_id"]
            found["first_boot"] = banner["first_boot"]
        return found
    finally:
        if port is not None:
            port.close()


def read_scanner(ports, app_info, log, disposable, timeouts, expect_banner=True):
    """Open a scanner's console: the `boot` line if it can be caught, then `info`."""
    port = ports.open(app_info.device, 115200)
    try:
        console = ScannerConsole(port, log=log, disposable=disposable, now=ports.now, sleep=ports.sleep)
        boot = console.first("boot", timeouts.banner_s) if expect_banner else None
        ack, batch = console.send("info", timeout_s=2.0)
        infos = batch.of_type("info")
        info = infos[0] if infos else console.first("info", 2.0)
        if info is None:
            return {"role": None, "version": None, "how": "console", "boot": boot}
        return {"role": info.get("role"), "version": info.get("ver"), "how": "console", "boot": boot,
                "info": info, "repeater_id": None, "name": None, "rom": info.get("rom"),
                "radio_ok": (boot or {}).get("radio_ok", info.get("radio_ok"))}
    finally:
        port.close()


def read_board(ports, app_info, log, disposable, timeouts, expect_banner=True, reboot_for_id=True):
    """What is on the board, through its console: a repeater, a scanner, or neither (role None).
    The repeater is asked first - its line ends in CR, which the scanner also takes as a line end,
    where a scanner line sent to upstream's console would sit in its buffer until the next CR."""
    found = read_repeater(ports, app_info, log, disposable, timeouts, expect_banner=expect_banner,
                          reboot_for_id=False)
    if found["role"] is not None:
        if found["repeater_id"] is None and reboot_for_id:
            found = read_repeater(ports, app_info, log, disposable, timeouts, expect_banner=False,
                                  reboot_for_id=True)
        return found
    return read_scanner(ports, app_info, log, disposable, timeouts, expect_banner=False)


# ----------------------------------------------------------------------------- the log

def log_flash(home, repo, config, image, outcome, result, extra=None):
    """One JSON line per flash, in two places."""
    record = {
        "at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "serial": config.serial,
        "rf": config.rf,
        "result": result,
        "bootloader_port": getattr(outcome, "bootloader_device", None),
        "touched": getattr(outcome, "touched", None),
    }
    record.update(image.log_record())
    if extra:
        record.update(extra)
    line = json.dumps(record, sort_keys=True) + "\n"
    for path in (os.path.join(home, "flash.log"), os.path.join(repo, "out", "bench", "flash.log")):
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "a", encoding="utf-8") as fh:
                fh.write(line)
        except OSError:
            pass
    return record
