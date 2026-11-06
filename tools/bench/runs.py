"""The board is left as it was found: the run record and the restore.

Before the first flash of a run the tool records what is on the board - `hello` and `identity`
over the link if a dongle is there, else the console's `Repeater ID:` and `sensor get fw.version`
(or the scanner's `info`) - in a file under the tool's data directory, one per board. `bench
restore` reads it and puts that role back: the image the run was given (`bench record --image`,
`bench flash --found`) if there was one, otherwise this checkout's own build of that role
(out/build/<role>_<board>/firmware.zip, what tools/verify.ps1 leaves), and then checks the
Repeater ID is the one it recorded. The last line says which image went on, or says in capitals
that the board could not be put back.

A board that shows only its bootloader - which is how every new unit arrives, with its base and
no application of its own - has no console to read, so the run records the image that is about to
be written instead (`bootloader_image` below): its role, its version and its descriptor, out of
the file. The restore then puts that image back and says so.
"""

import json
import os
import re
import subprocess
import sys
import time

from config import NoBench
from flash import read_board

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

_HEX64 = re.compile(r"\b([0-9A-Fa-f]{64})\b")


class RunRecord(object):
    FIELDS = ("serial", "started", "commit", "role", "version", "repeater_id", "name", "found_image",
              "how", "board_said", "found_descriptor")

    def __init__(self, **kw):
        for key in self.FIELDS:
            setattr(self, key, kw.get(key))

    def as_dict(self):
        return dict((key, getattr(self, key)) for key in self.FIELDS)

    @classmethod
    def from_dict(cls, data):
        return cls(**dict((k, data.get(k)) for k in cls.FIELDS))

    def describe(self):
        if self.role is None:
            return "nothing recognisable (role unknown)"
        text = "%s %s" % (self.role, self.version or "(version unknown)")
        if self.how == "bootloader":
            text += " (read from the image, not from the board: it arrived with only its bootloader)"
        if self.repeater_id:
            text += ", Repeater ID %s" % self.repeater_id
        if self.name:
            text += ", name %r" % self.name
        text += ", recorded %s via %s" % (self.started, self.how)
        if self.found_image:
            text += ", image given: %s" % self.found_image
        return text


def run_path(home, serial):
    return os.path.join(home, "runs", "%s.json" % serial.upper())


def load_run(home, serial):
    path = run_path(home, serial)
    if not os.path.isfile(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return RunRecord.from_dict(json.load(fh))
    except (OSError, ValueError):
        return None


def save_run(home, record):
    path = run_path(home, record.serial)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(record.as_dict(), fh, indent=2, sort_keys=True)
        fh.write("\n")
    return path


def clear_run(home, serial):
    try:
        os.remove(run_path(home, serial))
    except OSError:
        pass


def git_commit(repo=REPO):
    try:
        out = subprocess.run(["git", "-C", repo, "rev-parse", "HEAD"], stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, timeout=10)
        text = out.stdout.decode("ascii", errors="replace").strip()
        return text if re.match(r"^[0-9a-f]{40}$", text) else None
    except Exception:
        return None


# ----------------------------------------------------------------------------- recording

def parse_linkcli(output):
    """`role 1 (repeater)`, `fw_version X`, `band X`, `pubkey <64 hex>`, `name ...` out of
    linkcli's output. `band` arrives with box link 1.1 and is absent from a 1.0 radio's `hello`,
    which is why nothing here requires it."""
    found = {}
    for raw in (output or "").splitlines():
        line = raw.strip()
        m = re.match(r"^role\s+\d+\s+\((\w+)\)", line)
        if m:
            found["role"] = m.group(1)
        m = re.match(r"^fw_version\s+(\S+)", line)
        if m:
            found["version"] = m.group(1)
        m = re.match(r"^band\s+(\S+)", line)
        if m:
            found["band"] = m.group(1)
        m = re.match(r"^pubkey\s+([0-9A-Fa-f]{64})", line)
        if m:
            found["repeater_id"] = m.group(1).upper()
        m = re.match(r"^name\s+(.*)$", line)
        if m and "name" not in found:
            found["name"] = m.group(1).strip()
    return found


def record_over_link(config, ports, runner, log, python=None, repo=REPO):
    """`hello` and `identity` through tools/linkcli.py on the dongle, or None when there is no
    dongle in the config or on the machine."""
    if not config.dongle_serial:
        return None
    dongle = ports.find_serial(config.dongle_serial)
    if dongle is None:
        log("the dongle (serial %s) is not on this machine; the console it is" % config.dongle_serial)
        return None
    python = python or sys.executable
    linkcli = os.path.join(repo, "tools", "linkcli.py")
    found = {}
    for command in ("hello", "identity"):
        code, output = runner.run([python, linkcli, "--port", dongle.device, command], timeout_s=30)
        if code != 0:
            log("linkcli %s on %s exited %d; the console it is" % (command, dongle.device, code))
            return None
        found.update(parse_linkcli(output))
    if "role" not in found:
        return None
    found["how"] = "link (%s)" % dongle.device
    return found


def record_board(config, ports, runner, log, home, found_image=None, timeouts=None, python=None,
                 repo=REPO, bootloader_image=None):
    """What is on the board now, saved as the run's record. Raises NoBench when it is not there.

    `bootloader_image` is the Image (images.py) the caller is about to write, and it is used only
    when the board shows nothing but its bootloader: then it is the only thing that can be known
    about the board, and recording it is what makes the run restorable."""
    from flash import Timeouts
    timeouts = timeouts or Timeouts()
    app, boot = ports.find_board(config.serial, config.bootloader_serial)
    if app is None and boot is None:
        raise NoBench("no port with serial number %s: the board is not on this machine (no bench)" % config.serial)
    started = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    if app is None:
        # Every new unit arrives like this: the Walter has written its base and nothing else
        # (spec/SYSTEM.md S-5a), so there is no application port and no console, and the board can
        # say nothing about what it ran. What *can* be known is the file that is about to be
        # written, so that is what the run records - the image, with the role, the version and the
        # descriptor read out of it. Without it `bench restore` had nothing to put back and said
        # so in capitals, and the first board on this bench needed a `bench record --image ...`
        # typed by hand after the first flash to get past it (p3.4-02's report, p3.5-01). `how`
        # stays `bootloader`: this is still not something a board said.
        image = bootloader_image
        record = RunRecord(serial=config.serial, started=started, commit=git_commit(repo),
                           role=image.role if image is not None else None,
                           version=image.version if image is not None else None,
                           how="bootloader",
                           found_image=(os.path.abspath(image.path) if image is not None
                                        else found_image),
                           found_descriptor=dict(image.descriptor) if image is not None else None)
        if image is None:
            log("the board is in its bootloader and no image was named: nothing of what it ran "
                "can be recorded")
        else:
            log("the board is in its bootloader: nothing of what it ran can be read, so the run "
                "records the image it is about to be given - %s" % record.describe())
        save_run(home, record)
        return record
    found = record_over_link(config, ports, runner, log, python=python, repo=repo)
    if found is None:
        found = read_board(ports, app, log, config.disposable, timeouts, expect_banner=False)
    record = RunRecord(serial=config.serial, started=started, commit=git_commit(repo),
                       role=found.get("role"), version=found.get("version"),
                       repeater_id=found.get("repeater_id"), name=found.get("name"),
                       found_image=found_image, how=found.get("how"),
                       # `band` is HELLO's since link 1.1 and is what a release's image is
                       # chosen by with the role and the board, so a run that read it keeps it.
                       board_said=dict((k, v) for k, v in found.items()
                                       if k in ("rom", "radio_ok", "first_boot", "band")
                                       and v is not None))
    save_run(home, record)
    log("recorded: %s" % record.describe())
    return record


# ----------------------------------------------------------------------------- restoring

def own_image_of(role, board, repo=REPO):
    """This checkout's own build of a role for this board: what tools/verify.ps1 leaves."""
    env = "%s_%s" % (role, board.replace("-", "_"))
    return os.path.join(repo, "out", "build", env, "firmware.zip")


def choose_restore_image(record, board, repo=REPO):
    """(path, how) - which image puts the recorded role back. Raises RestoreNotPossible."""
    if record is None:
        raise RestoreNotPossible("NOTHING WAS RECORDED FOR THIS BOARD: no run is open, so there is "
                                 "nothing to restore to")
    if record.role is None:
        raise RestoreNotPossible("THE RUN RECORDED NO RECOGNISABLE ROLE ON THE BOARD (%s): the tool "
                                 "cannot know what to put back" % record.how)
    if record.found_image:
        if not os.path.isfile(record.found_image):
            raise RestoreNotPossible("THE IMAGE THE RUN WAS GIVEN IS GONE: %s" % record.found_image)
        return record.found_image, "the image the run was given (%s)" % record.found_image
    path = own_image_of(record.role, board, repo)
    if not os.path.isfile(path):
        raise RestoreNotPossible("THIS CHECKOUT HAS NO BUILD OF THE %s ROLE TO PUT BACK (%s is not "
                                 "there; tools/verify.ps1 builds it)" % (record.role.upper(), path))
    return path, "this run's own image of that role (%s)" % os.path.relpath(path, repo).replace("\\", "/")


class RestoreNotPossible(Exception):
    """Said in capitals in the last line."""
