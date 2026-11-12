"""docs/bench.md, walked: the lines a script can check, as steps the tool runs.

    python tools/bench/bench.py walk 1                            the repeater alone on USB
    python tools/bench/bench.py walk 3                            the clock
    python tools/bench/bench.py walk 5                            power: the two thresholds
    python tools/bench/bench.py walk 8  [--image <zip>]           what an image says it is
    python tools/bench/bench.py walk 9  [--scanner-image <zip>]   NoiseScope, a round trip
    python tools/bench/bench.py walk 10                           first boot and provisioning

docs/bench.md is what every task left under *Not verified here*, in the order the rig is brought
up. Six of its sections have lines a script can check with nothing but the board on USB - §1 the
repeater alone, §3 the clock, §5 the battery thresholds, §8 what an image says it is, §9
NoiseScope as a round trip, §10 first boot and provisioning - and this file is those sections, as
walks:

  * a walk is the section's bullets **in the page's order**, each bullet one step or several, and
    a step says the line it checks, what it sent, what it saw and `ok`, `differs`, `needs a
    person` or `skipped`. Every bullet of a walked section is in its walk, including the ones no
    script can do: those are `needs a person`, with what the person needs (a roof antenna, a
    phone, a scope probe, a generator, a power cut);
  * the result is a Markdown file under `out/bench/`, named by date and commit, with every
    `-> ______` of the walked lines filled in. `--tick` then ticks the bullets that came back
    `ok` in docs/bench.md as `[x] ... (bench, <date>, <commit>)` and writes the ones that came
    back `differs` into the follow-ups of the task that wrote the line - a *differs* is a finding
    for that task and never a fix here;
  * nothing a password passes through reaches either file: every line sent and every reply shown
    goes through console.redact, and §10's generated admin password is never written down at all
    (it is a bench unit's);
  * the board is left as it was found. §8 and §9 flash, so they open a run (runs.py) and end in
    the restore that closes it; §1 reads consoles only; §5 moves `power.low_mv` and puts it and
    `power.crit_mv` back as it found them, which is its last step; §10 erases, which is why it
    runs only with `"identity": "disposable"` in bench.local.json and refuses while a run is
    open - the erase makes a recorded Repeater ID unrestorable. §3 is the one walk that cannot
    put back what it moved: a clock this firmware keeps only goes forward
    (src/roles/repeater/clock/ClockKeeper.h), so the node is left `restored` and about an hour
    ahead of the truth, which is the keeper working and which the Walter corrects within seconds
    of EV_BOOT. §3 and §5 reset the board with upstream's own console lines (`reboot`,
    `clkreboot`) and flash nothing.

What a step expects is read and not remembered: the home channel and the duty cycle come from
`[band_<band>]` of platformio.ini (phase 3.3's task 01 put the band's ceiling there and nowhere
else), upstream's version from the tag the firmware's own `fw.version` names, and the role, board
and version of an image from its descriptor. The page's quoted text is compared too, but a
difference there is a note about a stale page and not a finding about the board.

No bench (no bench.local.json, or no port with its serial number) is exit 3 here as everywhere:
the walk is not run, and the task's report says *Not verified here*.
"""

import json
import os
import re
import shutil
import tempfile
import time

import config as cfg
import console as consolemod
from console import RepeaterConsole, ScannerConsole, console_guard
from flash import FlashError, Timeouts, read_repeater
from lock import BoardLock
from ports import same_serial
from runs import (RestoreNotPossible, choose_restore_image, git_commit, load_run, own_image_of,
                  parse_linkcli, record_board)

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

OK = "ok"
DIFFERS = "differs"
PERSON = "needs a person"
SKIPPED = "skipped"

# The radio ROM the fast-sweep plugin is made for (docs/bench.md section 9, *The fast sweep*,
# first line; src/roles/scanner/Radio.cpp refuses `scope` on anything else).
PLUGIN_ROM = "SX1261 V2D 2D02"

# What the page quotes for upstream's own `ver` line and for `board`. A MeshCore bump moves the
# first, and a walk says so as a note rather than as a finding: what it holds the board to is the
# tag our own `fw.version` names.
PAGE_VER = "v1.18.0 (Build: 15 Oct 2026)"
PAGE_BOARD = "RAK 4631"

# What `power.crit_mv` is on a node whose thresholds nobody has moved, and what docs/bench.md
# section 5's three refused values are refusals *of*: `3350` sits less than the hysteresis above
# it, `2400` below it, `4400` over the ceiling (src/common/power/BatteryWatch.h's
# kDefaultCritMv, kHysteresisMv and kThresholdCeilingMv). A node holding another `crit` makes
# some of the three legal, so the walk skips them there rather than calling a correct `ok` wrong.
PAGE_CRIT_MV = "3300"

# `sensor list`'s two pages, as upstream pages them (docs/provisioning.md section 3, and
# test/test_settings, which measures the arithmetic).
FIRST_PAGE_KEYS = ("fw.version", "walter.wake", "link.errors", "walter.seen", "clock.state")
SECOND_PAGE_KEYS = ("power.low_mv", "power.crit_mv", "obs.wake", "client.refused", "firstboot.state")
SETTING_COUNT = len(FIRST_PAGE_KEYS) + len(SECOND_PAGE_KEYS)


# ----------------------------------------------------------------------------- the page

class PageLine(object):
    """One bullet of docs/bench.md: the task(s) that wrote it, a unique piece of its text - so the
    walk can be held against the page, and the bullet found to be ticked - and what it is about."""

    def __init__(self, key, tasks, anchor, about, person=None):
        self.key = key
        self.tasks = tuple(tasks)
        self.anchor = anchor
        self.about = about
        self.person = person        # what a person needs, when no script can do it


class Section(object):
    def __init__(self, number, title, lines):
        self.number = number
        self.title = title
        self.lines = list(lines)

    def line(self, key):
        for line in self.lines:
            if line.key == key:
                return line
        raise KeyError("docs/bench.md has no line %s in this walk" % key)


SECTIONS = {}


def _section(number, title, lines):
    SECTIONS[number] = Section(number, title, lines)


_section("1", "The repeater alone on USB", [
    PageLine("1.1", ("p1-01",), "Flash the UF2 (double-tap reset on the adapter's USB-C",
             "the console on a repeater: the banner, `ver`, `board`, `get radio`, `sensor list` "
             "and its cut digit, `sensor list 5`, `sensor get fw.version`, and `sensor set "
             "fw.version x` refused"),
    PageLine("1.2", ("p1-01",), "It hears adverts (`neighbors` after a few minutes",
             "it hears adverts, and repeats",
             person="the roof antenna, a few minutes on it, a second radio and a phone - a "
                    "session cannot hear a radio"),
    PageLine("1.3", ("p1-01",), "A phone login with the password `password` is refused",
             "a phone's login, and the password the first boot set",
             person="a phone on the mesh, and a `password` line this console does not send on a "
                    "bench whose identity is kept"),
    PageLine("1.4", ("p1-01",), "`INFO.VARIANT` of the module, for the Walter's flasher",
             "the module's `INFO.VARIANT`",
             person="the bootloader's own drive (INFO_UF2.TXT), which this tool never mounts"),
    PageLine("1.5", ("p1-01",), "I2C is at 100 kHz on the scope",
             "the I2C clock, and the BME280 if one is fitted",
             person="a scope probe on SCL (P0.14), and a telemetry request from a phone"),
    PageLine("1.6", ("p3.3-02",), "The board's two USB serial numbers are one",
             "the application's USB serial number and the bootloader's",
             person="a double-tap reset and a power cycle - a hand on the board. The "
                    "application's half is read here and filled in below"),
    PageLine("1.7", ("p3.3-02",), "The round trip through `tools/bench/`, with the dummy load",
             "`status`, `flash`, `console \"ver\"`, `restore`"),
    PageLine("1.8", ("p3.3-02",), "**What it refuses on a real board.**",
             "the refusals, on a board, with `rf: antenna` in the config",
             person="an edit of bench.local.json (`rf: antenna`), which is the person's own file. "
                    "The guards themselves open no port and are host tests (test_bench.py)"),
])

_section("3", "The clock", [
    PageLine("3.1", ("p1-04",), "`linkcli.py time` on a node that was just flashed says",
             "the link's `time` and `status` on a node whose clock has never been set",
             person="the dongle on the link pins (bench.local.json's `dongle_serial`) or the "
                    "Walter: `linkcli.py` speaks the box link, which this tool does not"),
    PageLine("3.2", ("p1-04",), "`time set`, then `reboot 0`: after the restart",
             "the link's `time set` and `reboot 0`, and the epoch after the restart",
             person="the same dongle. The console's own half of it - `time <epoch>` and "
                    "`reboot` - is walked below"),
    PageLine("3.3", ("p1-04",), "The same after **pressing the reset switch**",
             "a reset switch and a watchdog reset, which leave no chance to write the file",
             person="a hand on the reset switch, and a way to provoke the watchdog on a "
                    "repeater image - a session resets a board only with upstream's own "
                    "`reboot`"),
    PageLine("3.4", ("p1-04",), "**Power off for a minute and on again**",
             "a minute without power, and `reset_reason 1 (power-on)` after it",
             person="the lab supply's switch, and the link to read `reset_reason`"),
    PageLine("3.5", ("p1-04",), "**With no laptop attached**, a nearby companion",
             "the advert hold, seen from a companion about a minute after boot",
             person="a second radio within range and a companion app - a session cannot hear a "
                    "radio"),
    PageLine("3.6", ("p1-04",), "**A nearby companion that saw this repeater's adverts before a",
             "a companion that still accepts this node's adverts after a power cycle",
             person="the same second radio, across a power cycle. What `clkreboot` does to this "
                    "node's own clock is walked below; what a companion makes of it is not"),
    PageLine("3.7", ("p1-04",), "**On the USB console of a node whose clock has never been",
             "`sensor get clock.state` and upstream's own `clock` on a node that has never had "
             "a clock"),
    PageLine("3.8", ("p1-04",), "**Upstream's own `time <epoch>` at the console**",
             "`time <epoch>` with this machine's time, and `clock.state` after it"),
    PageLine("3.9", ("p1-04",), "**Upstream's own `clock sync` at the console**",
             "`clock sync` at the console, and whether it moves the state the same way"),
    PageLine("3.10", ("p1-04",), "**After a `reboot` at the console**",
             "the boot after a `reboot`: `restored`, and the clock a few seconds on from the set"),
    PageLine("3.11", ("p1-04",), "**`clkreboot` at the console**",
             "the boot after a `clkreboot`: `restored`, and ahead of where the set left it"),
    PageLine("3.12", ("p1-04",), "**No RV-3028 is fitted on this batch**",
             "`clock.state` never saying `rtc` on a board with no RV-3028 fitted"),
    PageLine("3.13", ("p1-04",), "**An RV-3028 hand-fitted**",
             "a hand-fitted RV-3028 on 0x52, and `time_state 3 (rtc)` after a power cycle",
             person="a part nobody on this bench has, soldered to U5's footprint or wired to "
                    "the I2C bus"),
])

_section("5", "Power", [
    PageLine("5.1", ("p1-06",), "**`linkcli.py power` against a multimeter",
             "the three channels' sign and size against a DMM",
             person="the production connector boards, a bench supply at 18-21 V, a DMM in "
                    "series with the panel lead, and the link"),
    PageLine("5.1a", ("p3.10-01",), "**The readings keep arriving, on the device's",
             "that a real INA3221 raises its conversion-ready flag once an averaging window, so "
             "the first reading after a restart is one window away and not the fallback's two",
             person="the production connector boards and the link - and a part that really "
                    "raises that flag, which only a real one proves (tasks/phase-3.10/01)"),
    PageLine("5.2", ("p1-06",), "**The same three numbers in a phone's telemetry",
             "the same three numbers in a phone's telemetry view, and the battery in `status`",
             person="a phone on the mesh, and the boards - a session cannot hear a radio"),
    PageLine("5.3", ("p1-06",), "**`linkcli.py env` against a thermometer**",
             "temperature, humidity and pressure against something trustworthy",
             person="a thermometer in the enclosure, a BME280 on the bus, and the link"),
    PageLine("5.4", ("p1-06",), "**The battery thresholds, with the simulator.**",
             "the minute below a threshold, the hysteresis and the hourly limit",
             person="the lab supply's knob (or a battery simulator) and the link, which is "
                    "where an `EV_ATTENTION` arrives"),
    PageLine("5.5", ("p1-06",), "**The thresholds are settable and survive a restart.**",
             "`sensor set power.low_mv 3600`, `sensor list 5`, and 3600 still there after a "
             "restart"),
    PageLine("5.6", ("p1-06",), "**A pair this firmware will not take is refused, and nothing",
             "`3350`, `2400` and `4400` refused, with the value unchanged after each"),
    PageLine("5.7", ("p1-06",), "**With the INA3221's bus unplugged, the repeater",
             "a bus that is gone, and the three channels back when it returns",
             person="a hand on the I2C connector (or SDA held low) and the boards; a session "
                    "that cannot see the bus cannot unplug it"),
    PageLine("5.8", ("p1-06",), "**`sensor list` really does page.**",
             "upstream's paging of the ten keys over the link"),
    PageLine("5.9", ("p1-06",), "**The boot-voltage question, which is the reviewer's.**",
             "a node coming up with the pack at 3.2 V, and what upstream's power management "
             "says about it",
             person="the lab supply's knob at 3.2 V, which is the whole of this line; the "
                    "three `get`s it asks for are read here and filled in"),
])

_section("8", "What an image says it is", [
    PageLine("8.1", ("p3-01",), "The file you are about to flash says what it is, before you",
             "the descriptor in the file: role, board, band, version, offset"),
    PageLine("8.2", ("p3-01",), "The unit then says the same thing",
             "`hello` over the link: role, board, band, fw_version, slot"),
    PageLine("8.3", ("p3-01",), "The console agrees too",
             "the console's `sensor get fw.version` after the flash, and upstream's `ver`"),
    PageLine("8.4", ("p3-01",), "The Walter's half, in the Walter's repository",
             "a Walter reading the descriptor out of the file it downloaded",
             person="another repository's tests (twinoak.meshcore.walter-firmware); no session "
                    "and no bench of this repository can show it"),
])

_section("9", "NoiseScope", [
    PageLine("9.1", ("p3-02",), "Flash the scanner over USB",
             "the `boot` line: fw, role, board, ver, reset, and `radio_ok`"),
    PageLine("9.2", ("p3-02",), "`info`, `stat` and `sweep` behave as the page says",
             "`info` one line, `stat` at once, a 281-point `sweep` and its time, `hist`, "
             "`dwell`, `cad`, and `stop` in the middle of a sweep"),
    PageLine("9.3", ("p3-02",), "`pwr` matches the repeater's `power` numbers",
             "the scanner's `pwr` against the repeater's `power` for the same board",
             person="the production connector boards, a bench supply, and a run of each firmware "
                    "on the same board"),
    PageLine("9.4", ("p3-02",), "Every line is JSON, and the chatter says so",
             "a minute of output: every line parses, none carries `nan`, and the heartbeat is "
             "there"),
    PageLine("9.5", ("p3-02",), "Flash the repeater back, and nothing of the node is gone",
             "the restore: the same `Repeater ID`, the same name, a repeater's `fw.version` again"),
    PageLine("9.6", ("p3-02", "p3.5-03"), "The watchdog is live",
             "`hang`, the reset about eight seconds later, and `reset` reading `wdt` after it"),
    PageLine("9.7", ("p3.1-01",), "**The radio's ROM.**",
             "`info`'s `rom`, which decides whether any `scope` can run on this part"),
    PageLine("9.8", ("p3.1-01",), "**`scope 863 870` over USB, with the plotter.**",
             "the `ack`'s n, rbw, seg and cov, and the frames' `sps`"),
    PageLine("9.9", ("p3.1-01",), "**The baseline.**",
             "`scope 150 960 2048` on the dummy load, the NDJSON kept and the picture saved"),
    PageLine("9.10", ("p3.1-01",), "**The image check**, which is what the segments exist for",
             "one peak where the generator is, and nothing at the image",
             person="the tinySA Ultra as a generator, an attenuator, and the judgement that may "
                    "lower kScopeSegmentMhz"),
    PageLine("9.11", ("p3.1-01",), "**The site through the cavity filter.**",
             "`scope 791 960` with the baseline subtracted",
             person="the site, the antenna and the cavity filter - not a bench with a dummy load"),
    PageLine("9.12", ("p3.1-01",), "**The same `scope` from the Walter UART.**",
             "`scope` over `Serial1`, its raised interval, and ten minutes with no reset",
             person="the dongle on the link pins as a second console, and ten minutes of two "
                    "ports watched at once; this tool drives the USB console only"),
])

_section("10", "First boot and provisioning", [
    PageLine("10.1", ("p3-03", "p3.3-01"), "**A first boot is a first boot.**",
             "`erase`, a reset, a new `Repeater ID`, both `First boot:` lines, "
             "`firstboot.state`, `get dutycycle`, `get path.hash.mode`, `get radio`, `get name` "
             "and `get guest.password`"),
    PageLine("10.1a", ("p3.5-05",), "**And the Danish standard reached it.**",
             "`agc.reset.interval`, `loop.detect`, `flood.max.unscoped` and "
             "`flood.advert.interval` on the first boot, the last one against the hour this "
             "node's own key gives it"),
    PageLine("10.2", ("p3-03", "p3.3-01"), "**It runs once, and a reboot proves it.**",
             "a second boot: no `First boot:` line, `firstboot.state` never, and the two "
             "settings still as the profile left them"),
    PageLine("10.2a", ("p3.5-05",), "**And the Danish standard was saved with it.**",
             "the same four after the `reboot`, the advert interval the same hour as before it"),
    PageLine("10.3", ("p3-03",), "**A power cut in the middle leaves no half-provisioned node.**",
             "the power pulled during the first second, three or four times",
             person="a hand on the power, timed to the first second of a boot"),
    PageLine("10.4", ("p3-03",), "**Provision it at the USB port**",
             "docs/provisioning.md section 3, line for line, and read back"),
    PageLine("10.5", ("p3-03",), "**A phone then logs in with that admin password**",
             "a phone as admin, and the old password gone",
             person="a phone on the mesh - and the password this walk set is one nobody has, "
                    "which is what a bench unit is"),
    PageLine("10.6", ("p3-03",), "**Provision it over the link.**",
             "the same lines through `linkcli.py provision`, with the password printed as stars"),
    PageLine("10.7", ("p3-03",), "**And the node is the same node afterwards.**",
             "the public key and the name just set"),
])

WALKABLE = tuple(sorted(SECTIONS, key=lambda number: float(number)))


# ----------------------------------------------------------------------------- what a walk leaves

class Step(object):
    def __init__(self, line, what, status, sent=None, saw=None, expected=None, blanks=None,
                 note=None):
        self.line = line
        self.what = what
        self.status = status
        self.sent = list(sent or [])
        self.saw = saw
        self.expected = expected
        self.blanks = dict(blanks or {})
        self.note = note


class WalkResult(object):
    def __init__(self, section, config, commit, started):
        self.section = section
        self.config = config
        self.commit = commit
        self.started = started
        self.steps = []
        self.notes = []
        self.files = []

    # ---- building it

    def add(self, step):
        self.steps.append(step)
        return step

    def note(self, text):
        self.notes.append(text)

    def file(self, path, what):
        self.files.append((path, what))

    # ---- reading it

    def of(self, status):
        return [step for step in self.steps if step.status == status]

    def line_status(self, line):
        """A bullet's own status: the worst of its steps' - a differs beats a person, which beats
        a skip, which beats an ok."""
        mine = [step.status for step in self.steps if step.line is line]
        for status in (DIFFERS, PERSON, SKIPPED, OK):
            if status in mine:
                return status
        return SKIPPED

    def confirmed_lines(self):
        return [line for line in self.section.lines if self.line_status(line) == OK]

    def differing_lines(self):
        return [line for line in self.section.lines if self.line_status(line) == DIFFERS]

    @property
    def date(self):
        return self.started[:10]

    @property
    def short_commit(self):
        return (self.commit or "nocommit")[:7]

    def counts(self):
        return dict((status, len(self.of(status))) for status in (OK, DIFFERS, PERSON, SKIPPED))


def _safe(text):
    """Nothing a password passes through reaches a transcript or a file (console.redact)."""
    if text is None:
        return None
    return consolemod.redact(str(text))


def _starred(line):
    """A line that carries a password, as it is written down: the command and four stars, which
    is what tools/linkcli.py prints for the same line (docs/provisioning.md section 4)."""
    words = (line or "").split()
    if not words:
        return line
    if words[0].lower() == "password":
        return "password ****"
    if words[0].lower() == "set" and len(words) > 1 and words[1].lower() == "guest.password":
        return "set guest.password ****"
    return line


# ----------------------------------------------------------------------------- what to expect

_BAND_KEYS = ("freq", "bw", "sf", "cr", "duty_cycle")


def band_expectations(repo=REPO, band="868"):
    """`[band_<band>]` of platformio.ini: what `get radio` and `get dutycycle` must answer, read
    from the one place the build reads them too (task p3.3-01 put the duty cycle there). None
    when the file cannot be read, and then those steps are skipped rather than guessed."""
    import configparser
    parser = configparser.ConfigParser(strict=False)
    try:
        with open(os.path.join(repo, "platformio.ini"), "r", encoding="utf-8") as fh:
            parser.read_file(fh)
        section = parser["band_%s" % band]
        values = dict((key, section[key].strip()) for key in _BAND_KEYS)
        duty = "%.1f%%" % float(values["duty_cycle"])
    except (IOError, OSError, KeyError, ValueError, configparser.Error):
        return None
    return {
        "radio": "%s,%s,%s,%s" % (values["freq"], values["bw"], values["sf"], values["cr"]),
        "dutycycle": duty,
        "band": band,
    }


def advert_name(repo=REPO):
    """The node name a fresh unit carries: `-D ADVERT_NAME` of platformio.ini, which is this
    build's and not upstream's.

    Upstream's own default is `repeater` (examples/simple_repeater/MyMesh.cpp:22-23 at the pin
    81dbae59, `StrHelper::strncpy(_prefs.node_name, ADVERT_NAME, ...)` at :895); this repository
    builds with `RAK4631 Repeater`, which is what the first real board answered to `get name`
    (p3.4-02). Read from the file, so the walk cannot drift from the build; upstream's default
    when there is no such line."""
    try:
        with open(os.path.join(repo, "platformio.ini"), "r", encoding="utf-8") as fh:
            text = fh.read()
    except (IOError, OSError):
        return "repeater"
    found = re.findall(r"^\s*-D\s+ADVERT_NAME='\"(.*)\"'\s*$", text, re.M)
    return found[-1] if found else "repeater"


# `{"set <key> <value>",` of the first-boot profile's own table. A line whose value is not a
# plain word is not one this reads: the duty cycle's is a macro and the advert interval's is a
# placeholder, and those two have their own rules below and in band_expectations.
_PROFILE_SET = re.compile(r'\{"set ([a-z0-9.]+) ([A-Za-z0-9.]+)"')


def profile_expectations(repo=REPO):
    """What `get <key>` must answer for every **static** line of the first-boot profile, read out
    of the table itself (src/roles/repeater/firstboot/Profile.cpp) so that this page cannot drift
    from what a fresh unit is really told. `{}` when the file cannot be read, and then those
    steps are skipped rather than guessed."""
    try:
        with open(os.path.join(repo, "src", "roles", "repeater", "firstboot", "Profile.cpp"),
                  "r", encoding="utf-8") as fh:
            text = fh.read()
    except (IOError, OSError):
        return {}
    return dict((key, value) for key, value in _PROFILE_SET.findall(text))


_PROFILE_HOURS = re.compile(r"const int kFloodAdvertHours(Min|Span) = (\d+);")


def flood_advert_hours(pub_key_hex, repo=REPO):
    """The flood advert interval the profile derives from a node's key, in hours, or None when
    either the key or the rule cannot be read.

    The rule is `60 + (k mod 26)` with `k` the key's first byte (docs/provisioning.md section 1),
    and its two numbers are read out of src/roles/repeater/firstboot/Profile.h rather than
    written here - the same reason band_expectations reads platformio.ini."""
    key = (pub_key_hex or "").strip()
    if not re.match(r"^[0-9A-Fa-f]{2}", key):
        return None
    try:
        with open(os.path.join(repo, "src", "roles", "repeater", "firstboot", "Profile.h"),
                  "r", encoding="utf-8") as fh:
            found = dict(_PROFILE_HOURS.findall(fh.read()))
    except (IOError, OSError):
        return None
    if "Min" not in found or "Span" not in found or int(found["Span"]) <= 0:
        return None
    return int(found["Min"]) + (int(key[:2], 16) % int(found["Span"]))


def same_number(saw, expected, tol=0.001):
    """A number read back, as a number.

    Upstream keeps a coordinate as a `float` and prints every digit of it (`StrHelper::ftoa`), so
    `set lat 55.3250` reads back as `55.3250007` on a real board (p3.4-02) - the same place to
    11 cm."""
    try:
        return abs(float((saw or "").strip()) - float(str(expected).strip())) <= tol
    except ValueError:
        return False


def same_radio(saw, expected):
    """`get radio`'s four fields against `[band_868]`'s, the frequency as a number.

    Upstream keeps the frequency as a `float` and prints every digit it has
    (`StrHelper::ftoa(getFreq())`, src/helpers/CommonRadioPrefs.cpp:32-37 at the pin 81dbae59 -
    CommonCLI.cpp before repeater-v1.18.0), so the first
    real board answered `869.6179809,62.5,8,8` for the `869.618` of the band - the same frequency
    to 19 Hz, which is what a float32 can hold at 869 MHz (p3.4-02). The frequency and the
    bandwidth are compared to 1 kHz, the spreading factor and the coding rate exactly."""
    got = [part.strip() for part in (saw or "").split(",")]
    want = [part.strip() for part in (expected or "").split(",")]
    if len(got) != 4 or len(want) != 4:
        return False
    try:
        for i in (0, 1):
            if abs(float(got[i]) - float(want[i])) > 0.001:
                return False
        return [int(got[2]), int(got[3])] == [int(want[2]), int(want[3])]
    except ValueError:
        return False


# Upstream's own `clock` line, which is the only time readout the USB console has:
# `sprintf(reply, "%02d:%02d - %d/%d/%d UTC", dt.hour(), dt.minute(), dt.day(), dt.month(),
# dt.year())` (src/helpers/CommonCLI.cpp:226 at the pin 81dbae59). The seconds are not in it.
_CLOCK_LINE = re.compile(r"^(\d{1,2}):(\d{2})\s*-\s*(\d{1,2})/(\d{1,2})/(\d{4})\s*UTC$")

# What that costs a comparison: a reading is its own minute, so it sits between the true second
# and 59 seconds before it. Anything inside this of the time that was set is "the same minute".
CLOCK_MINUTE_S = 60


def clock_epoch(text):
    """Upstream's `clock` reply as an epoch, or None. The minute it prints is taken as :00, so a
    reading is up to CLOCK_MINUTE_S *behind* the second the clock really holds."""
    match = _CLOCK_LINE.match((text or "").strip())
    if not match:
        return None
    import calendar
    hour, minute, day, month, year = [int(part) for part in match.groups()]
    try:
        return calendar.timegm((year, month, day, hour, minute, 0, 0, 0, 0))
    except (ValueError, OverflowError):
        return None


def clock_is_upstreams_default(text):
    """True when `clock` answers the date VolatileRTCClock starts every boot at - 15 May 2024,
    epoch 1715770351 (src/helpers/ArduinoHelpers.h at the pin). The node has been up for a while
    by the time a walk reads it, so the day is what is held, not the second."""
    match = _CLOCK_LINE.match((text or "").strip())
    if not match:
        return False
    _hour, _minute, _day, month, year = [int(part) for part in match.groups()]
    return year == 2024 and month == 5


_FW_VERSION = re.compile(r"^twinoak-(\S+)\s+(\S+)\s+on\s+MeshCore\s+(\S+)\s+\(([0-9a-f]{8})\)$")


def parse_fw_version_line(text):
    """`> twinoak-<role> <version> on MeshCore <tag> (<pin>)` -> a dict, or None."""
    body = (text or "").strip()
    if body.startswith("> "):
        body = body[2:].strip()
    match = _FW_VERSION.match(body)
    if not match:
        return None
    return {"role": match.group(1), "version": match.group(2), "tag": match.group(3),
            "pin": match.group(4)}


def ack_fields(msg):
    """`n=250 step=28.0000kHz rbw=39.0kHz seg=1 cov=1.393` -> a dict of strings
    (docs/noisescope.md section 5: a `scope` ack carries what was *taken*)."""
    out = {}
    for token in str(msg or "").split():
        if "=" in token:
            key, value = token.split("=", 1)
            out[key] = value
    return out


_NAN = re.compile(r"(?i)(?<![0-9a-z_])nan(?![0-9a-z_])")


def nan_lines(raw):
    """Every line of a capture that carries a bare `nan`, which is not JSON and which the writer
    is held to never producing (docs/noisescope.md section 5; test/test_scanner)."""
    return [line for line in (raw or "").splitlines() if _NAN.search(line)]


def _types(batch):
    counts = {}
    for obj in batch.objects:
        counts[obj.get("t")] = counts.get(obj.get("t"), 0) + 1
    return ", ".join("%s x%d" % (t, n) for t, n in sorted(counts.items(), key=lambda kv: str(kv[0])))


def _already(batch, kind):
    """The line a command's own answer already carried, if it did: a scanner sends the ack and
    the first line of a scan in one breath, and on a real board the line comes seconds later."""
    found = batch.of_type(kind)
    return found[0] if found else None


def frame_summary(frame):
    """A scanner line, short enough for a transcript: no array of 281 numbers in a result file."""
    if not frame:
        return "(nothing)"
    out = {}
    for key, value in sorted(frame.items()):
        if isinstance(value, list):
            out[key] = "[%d values]" % len(value)
        elif isinstance(value, str) and len(value) > 24:
            out[key] = value[:21] + "..."
        else:
            out[key] = value
    return json.dumps(out, sort_keys=True)


# ----------------------------------------------------------------------------- the walker

class Walker(object):
    """One walk of one section, over the layers Bench already has, so that a test hands in the
    fake serial layer of fakes.py and the whole walk runs with no board at all."""

    def __init__(self, bench, section, image=None, scanner_image=None, collect_s=60.0,
                 baseline_s=20.0, password=None):
        self.bench = bench
        self.config = bench.config
        self.ports = bench.ports
        self.runner = bench.runner
        self.home = bench.home
        self.repo = bench.repo
        self.timeouts = bench.timeouts or Timeouts()
        self.section = SECTIONS[section]
        self.image = image
        self.scanner_image = scanner_image
        self.collect_s = float(collect_s)
        self.baseline_s = float(baseline_s)
        self._password = password
        # Wall-clock seconds, for the one walk that sets a node's clock to this machine's time
        # (section 3). ports.now() is a monotonic clock and not this.
        self.now_epoch = getattr(bench, "now_epoch", None) or (lambda: int(time.time()))
        self.expect = band_expectations(self.repo) or {}
        self.result = WalkResult(self.section, self.config, bench.commit or git_commit(self.repo),
                                 time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))

    # ---- what the session reading the console sees

    def say(self, line):
        self.bench.say(line)

    def _tell(self, step):
        self.say("  %-14s %s" % (step.status, step.what))
        for sent in step.sent:
            self.say("                 > %s" % sent)
        if step.saw:
            for one in str(step.saw).splitlines():
                self.say("                   %s" % one)
        if step.note:
            self.say("                 (%s)" % step.note)

    # ---- steps

    def safe(self, text):
        """console.redact, and then the one secret this walk knows by heart: the password it
        generated for a disposable unit is replaced wherever it appears, whoever printed it -
        upstream's own echo is redacted by its shape, and a tool that printed it some other way
        (section 10 looks) must still not be able to put it in a file."""
        text = _safe(text)
        if text and self._password:
            text = text.replace(self._password, "****")
        return text

    def mask(self, line):
        """A line as it goes into a transcript: a password line as four stars, and the secret
        this walk generated replaced anywhere else it might sit."""
        masked = _starred(line)
        if self._password:
            masked = masked.replace(self._password, "****")
        return masked

    def step(self, key, what, ok=True, sent=None, saw=None, expected=None, blanks=None, note=None,
             status=None):
        line = self.section.line(key)
        step = Step(line, what, status or (OK if ok else DIFFERS),
                    sent=[self.mask(one) for one in (sent or [])], saw=self.safe(saw),
                    expected=expected,
                    blanks=dict((k, self.safe(v)) for k, v in (blanks or {}).items()), note=note)
        self.result.add(step)
        self._tell(step)
        return step

    def person(self, key, note=None, blanks=None, saw=None):
        line = self.section.line(key)
        return self.step(key, line.about, status=PERSON, note=note or line.person, blanks=blanks,
                         saw=saw)

    def skip(self, key, why, what=None):
        return self.step(key, what or self.section.line(key).about, status=SKIPPED, note=why)

    # ---- a repeater's console

    def _open_repeater(self, app):
        port = self.ports.open_ready(app.device, 115200, self.timeouts.open_s)
        return port, RepeaterConsole(port, log=(lambda line: None),
                                     disposable=self.config.disposable, now=self.ports.now,
                                     sleep=self.ports.sleep)

    def ask(self, repeater, line, timeout_s=2.5):
        """One console line; the reply's text, redacted, and "" when nothing came back."""
        reply = repeater.send(line, timeout_s=timeout_s)
        return "" if reply.timed_out else reply.text

    def _value(self, text):
        """`> something` -> `something`, and a bare `>` -> `` (an empty setting, which is what
        `get guest.password` answers on a fresh unit); anything else as it stands."""
        body = (text or "").strip()
        return body[1:].strip() if body.startswith(">") else body

    def check_line(self, key, repeater, line, expected, what=None, blank=None, note=None,
                   same=None):
        """Send one line and hold its reply to `expected` - by string, or by `same(saw, expected)`
        where the board's own way of printing a value is not the page's (`same_radio`)."""
        saw = self.ask(repeater, line)
        value = self._value(saw)
        ok = same(value, expected) if same else value == expected
        return self.step(key, what or "`%s`" % line, ok=ok, sent=[line], saw=saw,
                         expected=expected, blanks={blank: value} if blank else None, note=note)

    def _app_port(self, what):
        app, boot = self.ports.find_board(self.config.serial, self.config.bootloader_serial)
        if app is None:
            raise cfg.Refused("the board is in its bootloader on %s: there is no console to %s "
                              "(`bench flash` an image first)" % (boot.device, what))
        return app

    # ---- the walk

    def run(self):
        getattr(self, "walk_%s" % self.section.number.replace(".", "_"))()
        return self.result

    # -- section 1 -----------------------------------------------------------

    def walk_1(self):
        app = self._app_port("walk section 1 with")
        self.result.note("The application's port is %s (USB serial number %s)."
                         % (app.device, app.serial_number))
        self.result.note("The identity is read as `bench record` reads it: the banner's "
                         "`Repeater ID:` when it can be caught, else upstream's own "
                         "`get public.key`, which is the same 32 bytes "
                         "(src/helpers/CommonCLI.cpp:750 at the pin 81dbae59). On a board that "
                         "has been up the banner is long gone - it is printed a second after boot "
                         "and only while a host holds the port - and only a board whose console "
                         "does not answer that line is reset once for it (p3.4-02).")

        # The banner is a boot line and not a command: `get public.key` is the same key and
        # needs no reset (flash.read_repeater; p3.4-02 read it off the first real board), and
        # upstream's own `reboot` is the fallback for a console that does not answer it.
        found = read_repeater(self.ports, app, lambda line: None, self.config.disposable,
                              self.timeouts, expect_banner=False, reboot_for_id=True)
        rid = found.get("repeater_id")
        self.step("1.1", "`Repeater ID:` on the console, 64 hex digits",
                  ok=bool(rid) and bool(re.match(r"^[0-9A-F]{64}$", rid or "")),
                  saw="Repeater ID: %s" % (rid or "(not caught)"),
                  expected="64 hexadecimal digits",
                  blanks={"section 1 Repeater ID": rid or "not caught"},
                  note=None if rid else "the banner was not caught, even after one `reboot`")
        if found.get("first_boot"):
            self.result.note("The banner carried %d `First boot:` line(s), so this module had "
                             "never been booted before - or had just been erased: %s"
                             % (len(found["first_boot"]), "; ".join(found["first_boot"])))

        port, repeater = self._open_repeater(app)
        try:
            repeater.drain(0.3)
            fw_text = self.ask(repeater, "sensor get fw.version")
            fw = parse_fw_version_line(fw_text)

            # `ver` is upstream's own line. What holds it is the tag our own `fw.version` names -
            # a MeshCore bump moves both together, where the page's quoted text goes stale.
            ver = self._value(self.ask(repeater, "ver"))
            tag_version = re.sub(r"^repeater-", "", fw["tag"]) if fw else None
            self.step("1.1", "`ver` says upstream's version, the one `fw.version` names",
                      ok=bool(ver) and (tag_version is None or tag_version in ver),
                      sent=["ver"], saw=ver, expected=tag_version or PAGE_VER,
                      blanks={"section 1 ver": ver},
                      note=None if ver == PAGE_VER else
                      "the page quotes `%s`; a MeshCore bump moves this line, and the page with "
                      "it" % PAGE_VER)

            self.check_line("1.1", repeater, "board", PAGE_BOARD, blank="section 1 board")
            if self.expect:
                self.check_line("1.1", repeater, "get radio", self.expect["radio"],
                                blank="section 1 get radio", same=same_radio,
                                note="[band_%s] of platformio.ini is where that comes from; the "
                                     "frequency is held as a number, because upstream prints "
                                     "every digit of the float it keeps (p3.4-02)"
                                     % self.expect["band"])
            else:
                self.skip("1.1", "platformio.ini's [band_868] could not be read, so there is "
                                 "nothing to hold `get radio` to", what="`get radio`")

            self._sensor_list(repeater)

            self.step("1.1", "`sensor get fw.version` is `twinoak-<role> <version> on MeshCore "
                             "<tag> (<pin>)`", ok=fw is not None, sent=["sensor get fw.version"],
                      saw=fw_text, blanks={"section 1 fw.version": self._value(fw_text)})

            refusal = self.ask(repeater, "sensor set fw.version x")
            self.step("1.1", "`sensor set fw.version x` is refused",
                      ok="can't find custom var" in refusal.lower(),
                      sent=["sensor set fw.version x"], saw=refusal,
                      expected="can't find custom var",
                      note="upstream's own wording for a key that took no value")

            # Task p3.3-01's line. The page has `get dutycycle` in section 10, where a first boot
            # has just run; the task asks for it here too, on a unit whose first boot was after
            # 01 landed.
            if self.expect:
                self.check_line("1.1", repeater, "get dutycycle", self.expect["dutycycle"],
                                blank="section 1 get dutycycle",
                                note="p3.3-01's line, read on a unit whose first boot was after "
                                     "it; the page has it in section 10. Upstream's own default "
                                     "is 50.0%")
        finally:
            port.close()

        self.person("1.2")
        self.person("1.3")
        self.person("1.4", blanks={"section 1 INFO.VARIANT": "not read here"})
        self.person("1.5")
        self.person("1.6", blanks={"section 1 application serial number": app.serial_number,
                                   "section 1 bootloader serial number": "not read here"},
                    saw="the application's port reports %s" % app.serial_number)
        self.skip("1.7", "`bench walk 8` is those four commands over an image, in that order - "
                         "`status`, `flash`, a console line, `restore` - and its result file is "
                         "the transcript",
                  what="`status`, `flash`, `console \"ver\"`, `restore`")
        refused = None
        try:
            console_guard("password x", self.config.disposable)
        except cfg.Refused as exc:
            refused = str(exc)
        self.person("1.8", note=self.section.line("1.8").person +
                    (". `bench console \"password x\"` was refused here with nothing opened: %s"
                     % refused if refused else ""))

    def _sensor_list(self, repeater):
        page1 = self.ask(repeater, "sensor list")
        keys = re.findall(r"^\s*([a-z._]+)=", page1, re.M)
        # Upstream writes `... next:5` after the fifth key and the reply is then cut to the 159
        # characters of its window (docs/bench.md 5.8; src/roles/repeater/app/Settings.cpp has
        # the arithmetic), so how much of the marker survives depends on the lines before it:
        # `clock.state=unset` leaves `... next:`, and `clock.state=restored` - three characters
        # longer - leaves `... ne`, which is what this walk met the first time it ran on a node
        # whose clock had been set (p3.5-06). The marker is therefore held to be a prefix of the
        # whole word, cut anywhere, and the page is told how much of it was left.
        marker_full = "... next:%d" % len(FIRST_PAGE_KEYS)
        tail = page1.rstrip("\n").split("\n")[-1] if page1 else ""
        marker = tail.startswith("...") and marker_full.startswith(tail)
        cut = marker and tail != marker_full
        self.step("1.1", "`sensor list` says `%d vars`, prints the first five and ends in "
                         "`... next:` with the digit cut" % SETTING_COUNT,
                  ok=(page1.startswith("%d vars" % SETTING_COUNT) and keys == list(FIRST_PAGE_KEYS)
                      and marker),
                  sent=["sensor list"], saw=page1,
                  expected="%d vars, then %s, then `... next:5` cut at 159 characters"
                           % (SETTING_COUNT, ", ".join(FIRST_PAGE_KEYS)),
                  blanks={"section 1 sensor list, first page":
                          "%d characters, keys %s, `%s`%s"
                          % (len(page1), ",".join(keys), tail,
                             " with the digit cut" if cut else " with its digit")},
                  note=None if cut else "the digit of `... next:5` is there, where the page says "
                                        "it is cut - a `fw.version` shorter than the one the page "
                                        "measured")
        page2 = self.ask(repeater, "sensor list 5")
        keys2 = re.findall(r"^\s*([a-z._]+)=", page2, re.M)
        self.step("1.1", "`sensor list 5` is the other five", ok=keys2 == list(SECOND_PAGE_KEYS),
                  sent=["sensor list 5"], saw=page2, expected=", ".join(SECOND_PAGE_KEYS),
                  blanks={"section 1 sensor list 5": ",".join(keys2)})

    # -- section 3 -----------------------------------------------------------

    def walk_3(self):
        """docs/bench.md section 3's console lines: what a clock does on a RAK4630 with no RTC.

        The six lines this walks are the ones a USB console can read. The node's clock is moved
        by them - a `time <epoch>` set, then a `reboot`, then a `clkreboot` - and a clock this
        firmware keeps never comes back down (ClockKeeper.h: the memories are a high-water
        mark), so the walk ends with the node `restored` and about an hour ahead of the truth.
        That is the keeper working, the Walter corrects it within seconds of EV_BOOT, and it is
        said in the result file's notes. Nothing else of the node is touched: no flash, no run,
        no erase, no identity."""
        app = self._app_port("walk section 3 with")
        self.result.note("The application's port is %s (USB serial number %s)."
                         % (app.device, app.serial_number))
        self.result.note("This walk performs two resets, both of them upstream's own console "
                         "lines (`reboot` and `clkreboot`), and no flash: no run is opened and "
                         "nothing of the node's identity is touched.")
        self.result.note("A clock this firmware keeps only goes forward (`ClockKeeper.h`), so "
                         "the node is left `restored` and about an hour ahead of the real time - "
                         "the `clkreboot` line's own margin. The Walter sets the time after "
                         "every boot (`spec/LINK-PROTOCOL.md` 5.4), and `bench walk 10`'s erase "
                         "is what takes a node back to `unset`.")

        for key in ("3.1", "3.2", "3.3", "3.4", "3.5", "3.6"):
            self.person(key)

        states = []                 # every `clock.state` this walk read, for 3.12
        port, repeater = self._open_repeater(app)
        try:
            repeater.drain(0.3)
            fw = parse_fw_version_line(self.ask(repeater, "sensor get fw.version"))
            if fw is None or fw["role"] != "repeater":
                raise cfg.Refused("`sensor get fw.version` said nothing a repeater says: "
                                  "section 3 is the repeater's clock keeper, and this board is "
                                  "not running one")

            # 3.7 - a node whose clock has never been set.
            before_state = self._value(self.ask(repeater, "sensor get clock.state"))
            states.append(before_state)
            before_clock = self._value(self.ask(repeater, "clock"))
            fresh = before_state == "unset"
            if fresh:
                self.step("3.7", "`sensor get clock.state` says `unset`", ok=True,
                          sent=["sensor get clock.state"], saw=before_state, expected="unset",
                          blanks={"section 3 clock.state, before any set": before_state})
                self.step("3.7", "upstream's own `clock` answers its built-in date in May 2024",
                          ok=clock_is_upstreams_default(before_clock), sent=["clock"],
                          saw=before_clock, expected="a date in May 2024 (epoch 1715770351)",
                          blanks={"section 3 clock, before any set": before_clock})
            else:
                why = ("this node's clock has been set before: `clock.state` says %r and `clock` "
                       "says %r. Only an `erase` takes a node back to `unset` - the keeper's "
                       "`/clock` survives a flash - and that is `bench walk 10`'s, not this "
                       "walk's. Not a differs: the line is about a node this one no longer is"
                       % (before_state, before_clock))
                self.skip("3.7", why, what="`clock.state` `unset` and upstream's own `clock` in "
                                           "May 2024")
                self.result.note("Section 3's first console line was not read: %s" % why)

            # 3.8 - upstream's own `time <epoch>`, with this machine's time.
            target = int(self.now_epoch())
            before_epoch = clock_epoch(before_clock)
            set_ok = False
            if before_epoch is not None and before_epoch > target:
                ahead = before_epoch - target
                why = ("the node's clock is already %d s ahead of this machine's, so upstream's "
                       "`time` refuses to move it (`ERR: clock cannot go backwards`, "
                       "src/helpers/CommonCLI.cpp:228-238 at the pin 81dbae59). That is what a second "
                       "walk of this section meets, an hour after the first one's `clkreboot`: "
                       "the three lines below need a node whose clock is behind the walker's"
                       % ahead)
                for key in ("3.8", "3.9", "3.10"):
                    self.skip(key, why)
                self.result.note("Section 3's `time`, `clock sync` and `reboot` lines were not "
                                 "read: %s" % why)
            else:
                reply = self.ask(repeater, "time %d" % target)
                set_ok = reply.strip().startswith("OK - clock set:")
                self.step("3.8", "`time <epoch>` with this machine's time is accepted",
                          ok=set_ok, sent=["time %d" % target], saw=reply,
                          expected="OK - clock set: <HH:MM - D/M/YYYY UTC>",
                          blanks={"section 3 time <epoch>": reply or "(nothing)"})
                after_clock = self._value(self.ask(repeater, "clock"))
                after_epoch = clock_epoch(after_clock)
                self.step("3.8", "`clock` reads back the minute that was set",
                          ok=(after_epoch is not None
                              and -CLOCK_MINUTE_S <= after_epoch - target <= CLOCK_MINUTE_S),
                          sent=["clock"], saw=after_clock,
                          expected="the same minute as %d" % target,
                          note="upstream prints the minute and not the second, so a reading sits "
                               "between its own minute and 59 s after it")
                state = self._value(self.ask(repeater, "sensor get clock.state"))
                states.append(state)
                self.step("3.8", "`clock.state` says `set` within one pass of `loop()`",
                          ok=state == "set", sent=["sensor get clock.state"], saw=state,
                          expected="set",
                          blanks={"section 3 clock.state, after the set": state},
                          note="the keeper polls for a clock somebody else moved rather than "
                               "patching upstream's CLI (`ClockKeeper::loop`)")

                # 3.9 - and `clock sync`, which the page says does the same.
                sync = self.ask(repeater, "clock sync")
                sync_state = self._value(self.ask(repeater, "sensor get clock.state"))
                states.append(sync_state)
                moved = sync.strip().startswith("OK - clock set:") and sync_state == "set"
                self.step("3.9", "`clock sync` moves the state the same way", ok=moved,
                          sent=["clock sync"],
                          saw="%s\n(`clock.state` is now %s)" % (sync or "(nothing)", sync_state),
                          expected="OK - clock set: <HH:MM - D/M/YYYY UTC>, and `clock.state` set",
                          blanks={"section 3 clock sync": sync or "(nothing)"},
                          note=None if moved else
                          "upstream's `clock sync` sets the clock to the *sender's* timestamp "
                          "plus one and refuses when that is not ahead of the node's own clock "
                          "(src/helpers/CommonCLI.cpp:210-218 at the pin 81dbae59). The USB "
                          "console has no sender and ours calls `handleCommand(0, ...)` "
                          "(src/roles/repeater/main.cpp:176), so at this port the line can only "
                          "answer what it answered; over the mesh, where an admin's message "
                          "carries a timestamp, it is the page's line. A follow-up for p1-04, "
                          "which wrote the line - never a fix here")
        finally:
            port.close()

        # 3.10 - the boot after a `reboot`.
        if set_ok:
            app = self._reset(app, "reboot")
            port, repeater = self._open_repeater(app)
            try:
                repeater.drain(0.3)
                state = self._value(self.ask(repeater, "sensor get clock.state"))
                states.append(state)
                clock = self._value(self.ask(repeater, "clock"))
                epoch = clock_epoch(clock)
                delta = None if epoch is None else epoch - target
                ok = (state == "restored" and delta is not None
                      and -CLOCK_MINUTE_S <= delta <= 2 * CLOCK_MINUTE_S)
                self.step("3.10", "`clock.state` is `restored` and the clock is a few seconds on "
                                  "from the set", ok=ok,
                          sent=["sensor get clock.state", "clock"],
                          saw="%s\n%s" % (state, clock),
                          expected="restored, and within a minute of %d" % target,
                          blanks={"section 3 after a reboot":
                                  "%s, %s (%s s from the set)"
                                  % (state, clock,
                                     "unreadable" if delta is None else "%+d" % delta)},
                          note="the file was written on the way down with `clean` set, and this "
                               "is the boot that may use it without the save interval's margin "
                               "(`ClockKeeper::saveBeforeRestart`, `begin`)")
            finally:
                port.close()

        # 3.11 - and the boot after a `clkreboot`, which sets upstream's clock back to 2024.
        port, repeater = self._open_repeater(app)
        try:
            repeater.drain(0.3)
            was = self._value(self.ask(repeater, "clock"))
            was_epoch = clock_epoch(was)
        finally:
            port.close()
        app = self._reset(app, "clkreboot")
        port, repeater = self._open_repeater(app)
        try:
            repeater.drain(0.3)
            state = self._value(self.ask(repeater, "sensor get clock.state"))
            states.append(state)
            clock = self._value(self.ask(repeater, "clock"))
            epoch = clock_epoch(clock)
            ahead = None if (epoch is None or was_epoch is None) else epoch - was_epoch
            ok = (state == "restored" and ahead is not None and ahead >= 0
                  and not clock_is_upstreams_default(clock))
            self.step("3.11", "the boot after `clkreboot` says `restored`, ahead of where the set "
                              "left it and never behind", ok=ok,
                      sent=["clock", "clkreboot", "sensor get clock.state", "clock"],
                      saw="before: %s\nafter:  %s\nclock.state: %s" % (was, clock, state),
                      expected="restored, and not behind %s" % was,
                      blanks={"section 3 after a clkreboot":
                              "%s, %s (%s s ahead of where the set left it)"
                              % (state, clock,
                                 "unreadable" if ahead is None else "%+d" % ahead)},
                      note="the margin is the save interval (3600 s, `kSaveIntervalSeconds`): "
                           "the 2024 value `clkreboot` wrote is not a sane epoch, so nothing was "
                           "saved on the way down and the file's value was not this restart's")
        finally:
            port.close()

        # 3.12 - and no reading of any of them said `rtc`.
        self.step("3.12", "`clock.state` never said `rtc`", ok="rtc" not in states,
                  saw="the words this walk read: %s" % (", ".join(states) or "(none)"),
                  expected="unset, set or restored - never rtc",
                  blanks={"section 3 the clock.state words read": ", ".join(states) or "none"},
                  note="U5 (the RV-3028 and its backup capacitors) is DNP on this batch, and a "
                       "VolatileRTCClock answering 2024 fails the keeper's sane-epoch floor "
                       "(`BoardClock.cpp` says why the question is answered from the answer)")
        self.person("3.13")

    # -- section 5 -----------------------------------------------------------

    def walk_5(self):
        """docs/bench.md section 5's two threshold lines, at the USB console.

        `power.low_mv` and `power.crit_mv` are read before anything is sent and put back as they
        were found, which is this walk's last step. Everything else in section 5 needs a meter, a
        phone, the lab supply's knob or a hand on the I2C bus."""
        app = self._app_port("walk section 5 with")
        self.result.note("The application's port is %s (USB serial number %s)."
                         % (app.device, app.serial_number))
        self.result.note("This walk performs one reset, upstream's own `reboot`, and no flash: "
                         "no run is opened and nothing of the node's identity is touched.")

        port, repeater = self._open_repeater(app)
        try:
            repeater.drain(0.3)
            fw = parse_fw_version_line(self.ask(repeater, "sensor get fw.version"))
            if fw is None or fw["role"] != "repeater":
                raise cfg.Refused("`sensor get fw.version` said nothing a repeater says: "
                                  "section 5's thresholds are the repeater's, and this board is "
                                  "not running one")
            found_low = self._value(self.ask(repeater, "sensor get power.low_mv"))
            found_crit = self._value(self.ask(repeater, "sensor get power.crit_mv"))
            self.result.note("Found `power.low_mv` %s and `power.crit_mv` %s. Both are put back "
                             "as they were found, which is this walk's last step."
                             % (found_low, found_crit))

            for key in ("5.1", "5.1a", "5.2", "5.3", "5.4"):
                self.person(key)

            # 5.5 - settable, listed, and still there after a restart.
            reply = self.ask(repeater, "sensor set power.low_mv 3600")
            self.step("5.5", "`sensor set power.low_mv 3600` answers `ok`",
                      ok=reply.strip() == "ok", sent=["sensor set power.low_mv 3600"], saw=reply,
                      expected="ok",
                      note="upstream's own word for a `sensor set` the table took "
                           "(src/helpers/CommonCLI.cpp:308-318 at the pin 81dbae59)")
            page2 = self.ask(repeater, "sensor list 5")
            self.step("5.5", "`sensor list 5` shows `power.low_mv=3600`",
                      ok="power.low_mv=3600" in page2, sent=["sensor list 5"], saw=page2,
                      expected="power.low_mv=3600 on the second page")
        finally:
            port.close()

        app = self._reset(app, "reboot")
        port, repeater = self._open_repeater(app)
        try:
            repeater.drain(0.3)
            after = self._value(self.ask(repeater, "sensor get power.low_mv"))
            self.step("5.5", "it is still 3600 after a restart", ok=after == "3600",
                      sent=["reboot", "sensor get power.low_mv"], saw=after, expected="3600",
                      blanks={"section 5 power.low_mv after the restart": after},
                      note="our own preferences file and not upstream's /prefs.json "
                           "(src/common/power/Prefs.h)")

            # 5.6 - and a pair this firmware will not take. The page's three values are
            # refusals of a node whose `crit` is 3300; on a node holding another one, some of
            # them are pairs this firmware accepts, and the line is then not this node's.
            refusals = [(3350, "less than 100 mV above `crit` (kHysteresisMv)"),
                        (2400, "below `crit`"),
                        (4400, "over the 4300 mV ceiling (kThresholdCeilingMv)")]
            if found_crit != PAGE_CRIT_MV:
                self.skip("5.6", "this node's `power.crit_mv` is %s and the page's three values "
                                 "are refusals of a node whose is %s: `3350` and `2400` are "
                                 "judged against `crit` (`thresholdsAcceptable`, "
                                 "src/common/power/BatteryWatch.cpp), so on this node they are "
                                 "not all refusals. Put `power.crit_mv` back to %s to walk this "
                                 "line" % (found_crit, PAGE_CRIT_MV, PAGE_CRIT_MV))
                refusals = []
            for mv, why in refusals:
                line = "sensor set power.low_mv %d" % mv
                reply = self.ask(repeater, line)
                back = self._value(self.ask(repeater, "sensor get power.low_mv"))
                refused = "can't find custom var" in reply.lower()
                self.step("5.6", "`%s` is refused - %s - and nothing changed" % (line, why),
                          ok=refused and back == after,
                          sent=[line, "sensor get power.low_mv"],
                          saw="%s\n(`power.low_mv` is still %s)" % (reply or "(nothing)", back),
                          expected="can't find custom var, and power.low_mv still %s" % after,
                          blanks={"section 5 the refusal of %d" % mv: reply or "(nothing)"})

            self.person("5.7")
            self.skip("5.8", "`bench walk 1` reads the same two pages at the same console - its "
                             "line 1.1 holds `10 vars`, the five keys of each page and the cut "
                             "digit of `... next:5` - and the `cli` half of this line is the "
                             "link's, which this tool does not speak",
                      what="upstream's paging of the ten keys")

            # 5.9 - the lab supply's, but the three `get`s of the line are free.
            pwr = {}
            for key in ("pwrmgt.bootmv", "pwrmgt.support", "pwrmgt.bootreason"):
                pwr[key] = self._value(self.ask(repeater, "get %s" % key))
            self.person("5.9",
                        saw="\n".join("get %s -> %s" % (key, pwr[key]) for key in sorted(pwr)),
                        blanks={"section 5 get pwrmgt.bootmv": pwr["pwrmgt.bootmv"],
                                "section 5 get pwrmgt.support": pwr["pwrmgt.support"],
                                "section 5 get pwrmgt.bootreason": pwr["pwrmgt.bootreason"]})

            # And back where they were found, which is this walk's last step.
            put = self._put_thresholds_back(repeater, found_low, found_crit)
            self.step("5.5", "`power.low_mv` and `power.crit_mv` are back at the values this "
                             "walk found", ok=put["ok"], sent=put["sent"], saw=put["saw"],
                      expected="power.low_mv %s, power.crit_mv %s" % (found_low, found_crit),
                      blanks={"section 5 the thresholds this walk found, and left":
                              "power.low_mv %s, power.crit_mv %s" % (found_low, found_crit)},
                      note=None if put["ok"] else
                      "THE THRESHOLDS ARE NOT WHAT THIS WALK FOUND: put them back by hand "
                      "(`sensor set power.low_mv %s`, `sensor set power.crit_mv %s`)"
                      % (found_low, found_crit))
        finally:
            port.close()

    def _put_thresholds_back(self, repeater, low, crit):
        """Both thresholds as they were found. A pair is refused as a pair
        (`thresholdsAcceptable`, src/common/power/BatteryWatch.cpp), so the order can matter:
        whichever order leaves both values where they were found is kept, and a walk that cannot
        put them back says so in capitals."""
        sent, saw = [], []
        orders = ((("power.low_mv", low), ("power.crit_mv", crit)),
                  (("power.crit_mv", crit), ("power.low_mv", low)))
        for order in orders:
            for key, value in order:
                line = "sensor set %s %s" % (key, value)
                reply = self.ask(repeater, line)
                sent.append(line)
                saw.append("%s -> %s" % (line, reply or "(nothing)"))
            now_low = self._value(self.ask(repeater, "sensor get power.low_mv"))
            now_crit = self._value(self.ask(repeater, "sensor get power.crit_mv"))
            saw.append("power.low_mv %s, power.crit_mv %s" % (now_low, now_crit))
            if (now_low, now_crit) == (low, crit):
                return {"ok": True, "sent": sent, "saw": "\n".join(saw)}
        return {"ok": False, "sent": sent, "saw": "\n".join(saw)}

    def _reset(self, app, line):
        """Upstream's own `reboot` or `clkreboot` at the console, and the application's port back.

        The board takes its USB device down with the reset (console.send_reset), so the handle
        dies rather than falling quiet, and the port is not back before about 1.4 s (p3.4-02)."""
        port, repeater = self._open_repeater(app)
        try:
            repeater.send_reset(line)
        finally:
            port.close()
        self.ports.sleep(self.timeouts.settle_s)
        again = self.ports.wait_for(
            lambda info: info.is_application and same_serial(info.serial_number,
                                                             self.config.serial),
            self.timeouts.application_port_s)
        if again is None:
            raise cfg.Refused("the board did not come back after `%s`: section %s cannot go on"
                              % (line, self.section.number))
        return again

    # -- section 8 -----------------------------------------------------------

    def walk_8(self):
        path = self.image or own_image_of("repeater", self.config.board, self.repo)
        if not os.path.isfile(path):
            raise cfg.Refused("there is no image to walk section 8 with: %s is not there "
                              "(tools/verify.ps1 builds it, or name one with --image)" % path)
        image = self.bench.inspect(path)                 # every guard, before any port is opened
        desc = image.descriptor
        self.step("8.1", "the descriptor in the file, read before it is flashed", ok=True,
                  saw=image.summary(),
                  blanks={"section 8 role": desc["role"], "section 8 board": desc["board"],
                          "section 8 band": desc["band"], "section 8 version": desc["version"],
                          "section 8 offset": "0x%X" % image.offset},
                  note="spec/tools/manifestref.py's own find_descriptor, which is what "
                       "`manifestref.py descriptor <file>` runs; exactly one descriptor, or the "
                       "image would have been refused before this step")
        raw_bin = os.path.join(os.path.dirname(os.path.abspath(path)), "firmware.bin")
        if os.path.isfile(raw_bin):
            import manifestref
            with open(raw_bin, "rb") as fh:
                offset, from_bin = manifestref.find_descriptor(fh.read())
            self.step("8.1", "the raw `firmware.bin` beside it carries the same descriptor",
                      ok=from_bin == desc and offset == image.offset,
                      saw="%s at 0x%X" % (json.dumps(from_bin, sort_keys=True), offset),
                      note="the page's own command is run on the `.bin`; the package carries the "
                           "same bytes")

        run = self._open_a_run()
        said = self.bench.flash_locked(image, "walk 8", run=run)
        self.step("8.3", "the console's `fw.version` after the flash is the descriptor's version",
                  ok=said.get("version") == desc["version"] and said.get("role") == desc["role"],
                  saw="the board says %s %s" % (said.get("role"), said.get("version")),
                  expected="%s %s" % (desc["role"], desc["version"]),
                  blanks={"section 8 the console's fw.version":
                          "%s %s" % (said.get("role"), said.get("version"))})
        self._hello(desc)

        port, repeater = self._open_repeater(self._app_port("read `ver` after the flash"))
        try:
            repeater.drain(0.3)
            ver = self._value(self.ask(repeater, "ver"))
            self.step("8.3", "upstream's own `ver` line is unchanged", ok=bool(ver), sent=["ver"],
                      saw=ver, blanks={"section 8 ver": ver})
        finally:
            port.close()

        self.person("8.4")
        self._restore("8.3", "the board back as this walk found it")

    def _hello(self, desc):
        """`hello` over the link, when a dongle is in the config and on this machine."""
        if not self.config.dongle_serial:
            return self.skip("8.2", "no `dongle_serial` in bench.local.json: `hello` needs the "
                                    "3.3 V USB-UART dongle on the link pins (docs/bench.md "
                                    "section 2)")
        dongle = self.ports.find_serial(self.config.dongle_serial)
        if dongle is None:
            return self.skip("8.2", "the dongle (serial %s) is not on this machine"
                                    % self.config.dongle_serial)
        code, output = self.runner.run(
            [self.bench.python, os.path.join(self.repo, "tools", "linkcli.py"), "--port",
             dongle.device, "hello"], timeout_s=30)
        found = parse_linkcli(output)
        slot = re.search(r"^slot\s+(\d+)", output or "", re.M)
        board = re.search(r"^board\s+(\S+)", output or "", re.M)
        # `band` is the fourth of the four strings a release's image is chosen by, and HELLO has
        # carried it since box link 1.1 (spec/LINK-PROTOCOL.md 5.1, p3.8-01). It is checked the
        # same way as the other three: against the descriptor in the file that was just flashed,
        # so what the unit says and what the image says cannot differ unnoticed.
        return self.step("8.2", "`hello` over the link says the descriptor's role, board, band "
                                "and fw_version, and `slot 0`",
                         ok=(code == 0 and found.get("role") == desc["role"]
                             and found.get("version") == desc["version"]
                             and board is not None and board.group(1) == desc["board"]
                             and found.get("band") == desc["band"]
                             and slot is not None and slot.group(1) == "0"),
                         sent=["linkcli.py --port %s hello" % dongle.device], saw=output,
                         expected="role %s, board %s, band %s, fw_version %s, slot 0"
                                  % (desc["role"], desc["board"], desc["band"], desc["version"]),
                         blanks={"section 8 hello":
                                 "role %s, board %s, band %s, fw_version %s, slot %s"
                                 % (found.get("role"), board and board.group(1),
                                    found.get("band"), found.get("version"),
                                    slot and slot.group(1))})

    # -- section 9 -----------------------------------------------------------

    def walk_9(self):
        path = self.scanner_image or own_image_of("scanner", self.config.board, self.repo)
        if not os.path.isfile(path):
            raise cfg.Refused("there is no scanner image to walk section 9 with: %s is not there "
                              "(tools/verify.ps1 builds it, or name one with --scanner-image)"
                              % path)
        image = self.bench.inspect(path)
        if image.role != "scanner":
            raise cfg.Refused("%s is a %s image: section 9 is the scanner's round trip, and the "
                              "repeater is what goes back on afterwards"
                              % (os.path.basename(path), image.role))

        run = self._open_a_run()
        before = {"repeater_id": run.repeater_id, "name": run.name, "role": run.role,
                  "version": run.version}
        said = self.bench.flash_locked(image, "walk 9", run=run)
        boot = said.get("boot") or {}
        info = said.get("info") or {}
        ver = boot.get("ver", info.get("ver"))
        reset = boot.get("reset", info.get("reset"))
        radio = boot.get("radio_ok", info.get("radio_ok"))
        radio_ok = radio in (True, 1, "1", "true")
        # The `boot` line is printed within a second of the reset and only while a host holds
        # the port (Adafruit_USBD_CDC::write drops bytes while tud_cdc_connected() is false), so a
        # host that opens the port when it enumerates cannot catch it: the first real board gave
        # none (p3.4-02). `info` answers the same five fields and `radio_ok` with them, which is
        # what this line is about.
        fields = boot or info
        notes = [] if radio_ok else [
            "radio_ok is not true: the power-enable pin (SX126X_POWER_EN, P1.05) is the first "
            "thing to look at - this port drives it high where NoiseScope never did "
            "(tasks/phase-3/reports/02.md)"]
        if not boot:
            notes.append("the `boot` line was not caught - it is printed within a second of the "
                         "reset and only while a host holds the port - so `info`'s own `fw`, "
                         "`role`, `board`, `ver`, `reset` and `radio_ok` are what is held here "
                         "(p3.4-02)")
        self.step("9.1", "the `boot` line: fw `noisescope`, role `scanner`, board `%s`, and "
                         "`radio_ok` (or `info`'s, when the line was not caught)"
                         % self.config.board,
                  ok=(fields.get("fw") == "noisescope" and fields.get("role") == "scanner"
                      and fields.get("board") == self.config.board and radio_ok),
                  saw=json.dumps(fields, sort_keys=True) if fields else
                      "(neither a `boot` line nor an `info` line)",
                  expected="fw noisescope, role scanner, board %s, radio_ok true"
                           % self.config.board,
                  blanks={"section 9 ver": ver, "section 9 reset": reset,
                          "section 9 radio_ok": radio},
                  note="; ".join(notes) or None)

        port = self.ports.open_ready(self._app_port("talk to the scanner").device, 115200,
                                     self.timeouts.open_s)
        try:
            scanner = ScannerConsole(port, log=(lambda line: None),
                                     disposable=self.config.disposable, now=self.ports.now,
                                     sleep=self.ports.sleep)
            rom = self._walk_9_console(scanner, info)
        finally:
            port.close()

        # The watchdog, which the scanner can now be *told* to stop its loop for (p3.5-03): the
        # last thing done to the scanner, because it ends in a reset and because a board that has
        # just come back from one is exactly what the restore after it should find.
        self._walk_9_watchdog()

        for key in ("9.3", "9.10", "9.11", "9.12"):
            self.person(key)

        # The page's own order puts the repeater back here; it is run last of all the steps
        # because nothing after it would have a scanner to talk to (the task's own order too).
        self._restore("9.5", "%s back on the board, and nothing of the node gone"
                      % (before["role"] or "what was found"), before=before)
        self.result.note("The `rom` this part reported was %r; the plugin's is %r."
                         % (rom, PLUGIN_ROM))

    def _walk_9_console(self, scanner, flashed_info):
        info = flashed_info
        if not info:
            _ack, batch = scanner.send("info", timeout_s=3.0)
            found = batch.of_type("info")
            info = found[0] if found else {}
        self.step("9.2", "`info` answers one line", ok=bool(info), sent=["info"],
                  saw=frame_summary(info))

        _ack, batch = scanner.send("stat", timeout_s=3.0)
        stats = batch.of_type("stat")
        self.step("9.2", "`stat` answers one immediately", ok=bool(stats), sent=["stat"],
                  saw=frame_summary(stats[0] if stats else None))

        began = self.ports.now()
        ack, batch = scanner.send("sweep", timeout_s=3.0)
        sweep = _already(batch, "sweep") or scanner.first("sweep", 30.0)
        took = self.ports.now() - began
        points = (sweep or {}).get("n")
        self.step("9.2", "`sweep` is acked and answers a `sweep` line of 281 points",
                  ok=bool(ack and ack.get("ok")) and points == 281, sent=["sweep"],
                  saw="%s\n%s" % (frame_summary(ack), frame_summary(sweep)), expected="n 281",
                  blanks={"section 9 sweep n": points, "section 9 sweep rbw": (sweep or {}).get("rbw"),
                          "section 9 sweep time": "%.1f s" % took})

        _ack, batch = scanner.send("hist 869.4 869.8 25", timeout_s=5.0)
        hist = batch.of_type("hist") or [one for one in [scanner.first("hist", 10.0)] if one]
        err = batch.of_type("err")
        self.step("9.2", "`hist 869.4 869.8 25` answers lines, or an `err` (the RAM patch)",
                  ok=bool(hist or err), sent=["hist 869.4 869.8 25"],
                  saw=("%d `hist` line(s), the first %s" % (len(hist), frame_summary(hist[0])))
                      if hist else frame_summary(err[0] if err else None),
                  blanks={"section 9 hist": ("%d lines" % len(hist)) if hist else
                                            ((err[0].get("msg") if err else "nothing"))})
        scanner.send("stop", timeout_s=3.0)

        _ack, batch = scanner.send("dwell 869.618 30", timeout_s=3.0)
        dwell = _already(batch, "dwell") or scanner.first("dwell", 20.0)
        self.step("9.2", "`dwell 869.618 30` answers a window", ok=bool(dwell),
                  sent=["dwell 869.618 30"], saw=frame_summary(dwell),
                  blanks={"section 9 dwell": frame_summary(dwell)})
        scanner.send("stop", timeout_s=3.0)

        _ack, batch = scanner.send("cad", timeout_s=5.0)
        cad = _already(batch, "cad") or scanner.first("cad", 15.0)
        self.step("9.2", "`cad` answers a detection ratio", ok=bool(cad), sent=["cad"],
                  saw=frame_summary(cad),
                  blanks={"section 9 cad": "det %s of %s"
                                           % ((cad or {}).get("det"), (cad or {}).get("n"))})

        scanner.send("sweep", timeout_s=3.0)
        began = self.ports.now()
        stopped, _batch = scanner.send("stop", timeout_s=5.0)
        waited = self.ports.now() - began
        self.step("9.2", "`stop` in the middle of a sweep is answered between steps",
                  ok=bool(stopped and stopped.get("ok")) and waited < 3.0, sent=["sweep", "stop"],
                  saw="%s after %.2f s" % (frame_summary(stopped), waited),
                  expected="an ack in well under one pass (7 s)",
                  blanks={"section 9 stop": "%.2f s" % waited})

        batch, raw = scanner.collect_raw(self.collect_s)
        bad = nan_lines(raw)
        heartbeats = batch.of_type("stat")
        self.step("9.4", "every line of %.0f s of output parses, and none carries `nan`"
                         % self.collect_s,
                  ok=batch.unparsed_count == 0 and not bad,
                  saw="%d JSON line(s) (%s), %d that did not parse, %d with `nan`"
                      % (len(batch.objects), _types(batch), batch.unparsed_count, len(bad)),
                  blanks={"section 9 a minute's output":
                          "%d JSON lines, %d not JSON, %d with `nan`"
                          % (len(batch.objects), batch.unparsed_count, len(bad))},
                  note=("what did not parse: %s" % self.safe(" | ".join(batch.unparsed[:3])))
                       if batch.unparsed else None)
        self.step("9.4", "the `stat` heartbeat kept coming, ten seconds apart",
                  ok=len(heartbeats) >= max(1, int(self.collect_s // 10) - 1),
                  saw="%d `stat` line(s) in %.0f s" % (len(heartbeats), self.collect_s),
                  expected="one every 10 s",
                  blanks={"section 9 heartbeats": "%d in %.0f s"
                                                  % (len(heartbeats), self.collect_s)})

        rom = info.get("rom")
        self.step("9.7", "`info`'s `rom` is the part the plugin is made for", ok=rom == PLUGIN_ROM,
                  sent=["info"], saw="rom %r" % rom, expected=PLUGIN_ROM,
                  blanks={"section 9 rom": rom},
                  note=None if rom == PLUGIN_ROM else
                  "the fast sweep is not walkable on this part: `scope` refuses and nothing is "
                  "uploaded. `sweep`, `hist`, `dwell` and `cad` are unaffected")
        if rom != PLUGIN_ROM:
            why = ("`info` says rom %r and the plugin is for %r: the first line of *The fast "
                   "sweep* stops the rest of it" % (rom, PLUGIN_ROM))
            self.skip("9.8", why)
            self.skip("9.9", why)
            return rom
        self._walk_9_scope(scanner)
        return rom

    def _walk_9_watchdog(self):
        """`hang`, and what comes back: the loop stops, the loop watchdog resets the part about
        eight seconds later, and `boot` says `wdt` (docs/bench.md section 9, *The watchdog is
        live*; docs/noisescope.md sections 5 and 6).

        Until p3.5-03 this was a person's line - *a session cannot cause one* - because nothing in
        the firmware could be asked to hang. The hardware watchdog is at a timeout a USB update
        fits in now and the 8 s check is software, so what is walked here is the software half;
        the hardware one is the backstop for a part whose interrupts have stopped too, and
        provoking that is still nobody's."""
        app = self._app_port("ask the scanner to hang")
        port = self.ports.open_ready(app.device, 115200, self.timeouts.open_s)
        asked, ack = None, None
        try:
            scanner = ScannerConsole(port, log=(lambda line: None),
                                     disposable=self.config.disposable, now=self.ports.now,
                                     sleep=self.ports.sleep)
            ack, _batch = scanner.send("hang", timeout_s=3.0)
            asked = self.ports.now()
        except (IOError, OSError) as exc:
            # Not expected - the ack leaves before the loop goes quiet - but a port that dies
            # under the line is the reset, exactly as RepeaterConsole.send_reset reads it.
            self.say("                   (the port went away with the line: %s)"
                     % str(exc).split(".")[0])
            asked = self.ports.now()
        finally:
            port.close()
        if ack is None or not ack.get("ok", False):
            return self.step("9.6", "`hang` is acked, and then the loop stops", ok=False,
                             sent=["hang"],
                             saw="no `ack` for `hang` came back" if ack is None
                                 else frame_summary(ack),
                             expected="an ack naming the deadline the unit will reset after",
                             note="an image without the `hang` command cannot walk this line: it "
                                  "is a person's again, with an induced hang (docs/noisescope.md "
                                  "section 5)")

        # The reset takes the port with it, and the application's comes back about 1.4 s later.
        self.ports.wait_until_gone(lambda info: info.device == app.device,
                                   self.timeouts.application_port_s)
        again = self.ports.wait_for(
            lambda info: info.is_application and same_serial(info.serial_number,
                                                             self.config.serial),
            self.timeouts.application_port_s)
        took = self.ports.now() - asked
        if again is None:
            return self.step(
                "9.6", "the unit comes back from the watchdog reset", ok=False, sent=["hang"],
                saw="the application's port did not come back within %.0f s of `hang`"
                    % self.timeouts.application_port_s,
                expected="a reset and a boot",
                note="the board may be in its bootloader: `bench status`, and `bench flash` or a "
                     "double-tap")

        port = self.ports.open_ready(again.device, 115200, self.timeouts.open_s)
        try:
            scanner = ScannerConsole(port, log=(lambda line: None),
                                     disposable=self.config.disposable, now=self.ports.now,
                                     sleep=self.ports.sleep)
            # The `boot` line is a race on a real board (section 0a); `info` carries the same
            # `reset` and the same `wdt`, so it is what the line is held to when the race is lost.
            fields = scanner.first("boot", self.timeouts.banner_s)
            how = "the `boot` line"
            if not fields:
                _ack, batch = scanner.send("info", timeout_s=3.0)
                found = batch.of_type("info")
                fields = found[0] if found else (scanner.first("info", 2.0) or {})
                how = "`info` (the `boot` line was not caught)"
        finally:
            port.close()
        reset = (fields or {}).get("reset")
        half = (fields or {}).get("wdt")
        return self.step("9.6", "`hang` stops the loop and the watchdog resets the unit, which "
                                "comes back saying `reset` `wdt`",
                         ok=reset == "wdt", sent=["hang", "info"],
                         saw="%s after %.1f s: %s" % (how, took, frame_summary(fields)),
                         expected="reset wdt (and `wdt` `loop`, the software half)",
                         blanks={"section 9 watchdog reset": reset,
                                 "section 9 watchdog half": half or "(not said)",
                                 "section 9 watchdog, time to the port": "%.1f s" % took},
                         note=None if half == "loop" else
                         "`wdt` says %r where the loop check's own reset says `loop`: a hardware "
                         "watchdog reset here would mean the loop check did not fire first, and an "
                         "empty one that the marker did not survive "
                         "(src/roles/scanner/loopwdt.ld)" % (half,))

    def _walk_9_scope(self, scanner):
        ack, batch = scanner.send("scope 863 870", timeout_s=5.0)
        fields = ack_fields((ack or {}).get("msg"))
        frames = list(batch.of_type("scope"))
        deadline = self.ports.now() + 10.0
        while len(frames) < 3 and self.ports.now() < deadline:
            frame = scanner.first("scope", 2.0)
            if frame is None:
                break
            frames.append(frame)
        rates = [frame.get("sps") for frame in frames if frame.get("sps") is not None]
        fastest = max(rates) / 10.0 if rates else None
        self.step("9.8", "`scope 863 870`: the ack says what was taken, and frames arrive",
                  ok=bool(ack and ack.get("ok")) and bool(frames), sent=["scope 863 870"],
                  saw="%s\n%d frame(s), %s"
                      % (frame_summary(ack), len(frames),
                         ("sps up to %.1f a second" % fastest) if fastest else "no `sps` yet"),
                  blanks={"section 9 scope n": fields.get("n"),
                          "section 9 scope rbw": fields.get("rbw"),
                          "section 9 scope seg": fields.get("seg"),
                          "section 9 scope cov": fields.get("cov"),
                          "section 9 scope sps": ("%.1f" % fastest) if fastest else None},
                  note="the plotter sends the same command and draws it; what is held here is the "
                       "ack's own fields and that frames come at all "
                       "(`waterfall_plot.py --port <port> --scope 863 870` is the picture)")
        scanner.send("stop", timeout_s=3.0)

        if self.config.rf != "dummy-load":
            return self.skip("9.9", "the baseline is a capture of the radio's own spurs and of "
                                    "the matching network: it needs the 50 ohm dummy load on the "
                                    "antenna port, and bench.local.json says rf is %r"
                                    % self.config.rf)
        out_dir = os.path.join(self.repo, "out", "bench")
        os.makedirs(out_dir, exist_ok=True)
        stem = "baseline-150-960-%s-%s" % (_stamp(self.result.started), self.result.short_commit)
        ndjson = os.path.join(out_dir, stem + ".ndjson")
        ack, batch = scanner.send("scope 150 960 2048", timeout_s=8.0)
        more, raw = scanner.collect_raw(self.baseline_s)
        scanner.send("stop", timeout_s=3.0)
        frames = batch.of_type("scope") + more.of_type("scope")
        # A `scope` that stopped itself says why on the same console, and the reason belongs in
        # the result file rather than only in the NDJSON: the first real board answered
        # `scope setup failed (-707) at 212.6880` and sent no frame at all (p3.4-02).
        errors = [obj.get("msg") for obj in batch.objects + more.objects
                  if obj.get("t") == "err" and obj.get("msg")]
        with open(ndjson, "w", encoding="utf-8") as fh:
            for obj in batch.objects:
                fh.write(json.dumps(obj, sort_keys=True) + "\n")
            if raw:
                fh.write(raw if raw.endswith("\n") else raw + "\n")
        self.result.file(os.path.relpath(ndjson, self.repo),
                         "the baseline's NDJSON, which `--baseline` subtracts from a later scope")
        picture = os.path.join(out_dir, stem + ".png")
        code, _output = self.runner.run(
            [self.bench.python, os.path.join(self.repo, "tools", "noisescope", "waterfall_plot.py"),
             "--file", ndjson, "--save", picture], timeout_s=300)
        drawn = code == 0 and os.path.isfile(picture)
        if drawn:
            self.result.file(os.path.relpath(picture, self.repo),
                             "the baseline's picture (`waterfall_plot.py --save`)")
        return self.step("9.9", "the baseline: `scope 150 960 2048` on the dummy load, kept",
                         ok=bool(ack and ack.get("ok")) and bool(frames),
                         sent=["scope 150 960 2048"],
                         saw="%s\n%d frame(s) in %.0f s -> %s%s"
                             % (frame_summary(ack), len(frames), self.baseline_s,
                                _rel(ndjson, self.repo),
                                "".join("\nerr: %s" % self.safe(msg) for msg in errors)),
                         blanks={"section 9 baseline": "%d frames, %s"
                                                       % (len(frames), stem + ".ndjson"),
                                 "section 9 peaks seen on the dummy load":
                                 "the picture is the answer and a session cannot read a peak off "
                                 "it"},
                         note=("the picture is %s" % _rel(picture, self.repo)) if drawn else
                              ("the picture was not drawn (waterfall_plot.py exited %d; it needs "
                               "the matplotlib of tools/noisescope/requirements.txt). The NDJSON "
                               "is kept, and `--file %s --save <png>` draws it later"
                               % (code, _rel(ndjson, self.repo))))

    # -- section 10 ----------------------------------------------------------

    def walk_10(self):
        if not self.config.disposable:
            raise cfg.Refused("section 10 erases the file system, which takes the node's identity "
                              "with it: it is walked only with \"identity\": \"disposable\" in "
                              "bench.local.json, and this bench says %r" % self.config.identity)
        run = load_run(self.home, self.config.serial)
        if run is not None:
            raise cfg.Refused("a run is open for this board (%s): `bench restore` it first. An "
                              "erase would make the Repeater ID that run recorded one no restore "
                              "could ever check again." % run.describe())
        app = self._app_port("walk section 10 with")

        port, repeater = self._open_repeater(app)
        try:
            repeater.drain(0.3)
            fw = parse_fw_version_line(self.ask(repeater, "sensor get fw.version"))
            if fw is None or fw["role"] != "repeater":
                raise cfg.Refused("`sensor get fw.version` said nothing a repeater says: section "
                                  "10 is the repeater's first boot, and this board is not running "
                                  "one")
            was = self._value(self.ask(repeater, "get public.key"))
            erased = self.ask(repeater, "erase", timeout_s=10.0)
            self.step("10.1", "`erase` wipes the file system", ok="OK" in erased, sent=["erase"],
                      saw=erased, expected="File system erase: OK")
        finally:
            port.close()

        banner = self._reboot_and_read(app)
        rid = banner.get("repeater_id")
        where = banner.get("id_from")
        self.step("10.1", "the node's key is a new one after the erase",
                  ok=bool(rid) and rid != (was or "").upper(),
                  saw="%s %s" % ("`get public.key`" if where else "Repeater ID:",
                                 rid or "(not caught)"),
                  expected="64 hex digits, and not the ones before the erase",
                  blanks={"section 10 the new Repeater ID": rid},
                  note="the banner's `Repeater ID:` line was not caught, so the key is "
                       "upstream's own `get public.key` - the same 32 bytes (p3.4-02)"
                       if where else None)
        first = banner.get("first_boot") or []
        if first:
            self.step("10.1", "both `First boot:` lines are printed", ok=len(first) == 2,
                      saw="\n".join(first),
                      expected="the random admin password, and the profile's 2 of 2 lines",
                      blanks={"section 10 First boot": "%d line(s)" % len(first)})
        else:
            # What the first real board showed (p3.4-02): the boot lines go out before a host can
            # hold the port, so no run of this tool can see them. What they left behind is the
            # step above (a new key) and `firstboot.state` below.
            self.person("10.1", note="the two `First boot:` lines are printed about a second "
                                     "after the reset and only while a host holds the port; the "
                                     "port is not back before 1.4 s, so a terminal already open "
                                     "across a reset button is the only way to read them. What "
                                     "they left behind is checked here: a new key, and "
                                     "`firstboot.state` = done",
                        saw="(the boot lines were gone before the port was back)",
                        blanks={"section 10 First boot": "not readable by `tools/bench`"})

        port, repeater = self._open_repeater(app)
        try:
            repeater.drain(0.3)
            self.check_line("10.1", repeater, "sensor get firstboot.state", "done",
                            blank="section 10 firstboot.state")
            if self.expect:
                self.check_line("10.1", repeater, "get dutycycle", self.expect["dutycycle"],
                                blank="section 10 get dutycycle",
                                note="the band's, from [band_%s]; upstream's own default is 50.0%%"
                                     % self.expect["band"])
                self.check_line("10.1", repeater, "get radio", self.expect["radio"],
                                blank="section 10 get radio", same=same_radio)
            self.check_line("10.1", repeater, "get path.hash.mode", "1",
                            blank="section 10 path.hash.mode")
            self.check_line("10.1", repeater, "get name", advert_name(self.repo),
                        blank="section 10 get name",
                        note="`-D ADVERT_NAME` of platformio.ini; upstream's own default is "
                             "`repeater`")
            guest = self.ask(repeater, "get guest.password")
            self.step("10.1", "`get guest.password` is empty", ok=self._value(guest) == "",
                      sent=["get guest.password"], saw=guest or "(empty)", expected="(empty)",
                      blanks={"section 10 guest.password":
                              "empty" if self._value(guest) == "" else "not empty"})
            self._danish_standard("10.1a", repeater, rid, "")
        finally:
            port.close()

        second = self._reboot_and_read(app)
        self.step("10.2", "the second boot prints no `First boot:` line",
                  ok=not second.get("first_boot"),
                  saw="\n".join(second.get("first_boot") or []) or "(no `First boot:` line)",
                  blanks={"section 10 the second boot's Repeater ID": second.get("repeater_id")})
        port, repeater = self._open_repeater(app)
        try:
            repeater.drain(0.3)
            self.check_line("10.2", repeater, "sensor get firstboot.state", "never",
                            blank="section 10 firstboot.state, second boot")
            self.check_line("10.2", repeater, "get path.hash.mode", "1")
            if self.expect:
                self.check_line("10.2", repeater, "get dutycycle", self.expect["dutycycle"])
            self._danish_standard("10.2a", repeater, rid, ", second boot")
        finally:
            port.close()

        self.person("10.3")
        self._provision(app)
        self.person("10.5")
        self._identity_afterwards(app, rid)

    def _danish_standard(self, key, repeater, rid, when):
        """The Danish standard's four lines read back (p3.5-05, docs/bench.md section 10).

        The three static values come out of the profile's own table and the fourth out of the
        node's key, so neither this file nor the page carries a number the firmware could change
        under it: a line taken off the table is a *skipped* step saying so, and a line whose value
        changed is checked against the new value."""
        wanted = profile_expectations(self.repo)
        for name in ("agc.reset.interval", "loop.detect", "flood.max.unscoped"):
            if name not in wanted:
                self.skip(key, "src/roles/repeater/firstboot/Profile.cpp has no static `set %s` "
                               "line, so there is nothing here to expect" % name,
                          what="`get %s`" % name)
                continue
            self.check_line(key, repeater, "get %s" % name, wanted[name],
                            blank="section 10 %s%s" % (name, when))
        hours = flood_advert_hours(rid, self.repo)
        if hours is None:
            self.skip(key, "no `Repeater ID` was read, so the hour this node's own key gives it "
                           "cannot be computed", what="`get flood.advert.interval`")
            return
        self.check_line(key, repeater, "get flood.advert.interval", str(hours),
                        blank="section 10 flood.advert.interval%s" % when,
                        note="60 + (0x%s mod 26) from the key this boot made; upstream's own "
                             "default is 47 hours for every unit" % rid[:2].lower())

    def _reboot_and_read(self, app):
        """Upstream's own `reboot`, then the banner. It is the only reset this tool performs, and
        the clock keeper's file is written on the way down (docs/bench.md section 3)."""
        port, repeater = self._open_repeater(app)
        try:
            repeater.send_reset("reboot")
        finally:
            port.close()
        self.ports.sleep(self.timeouts.settle_s)
        again = self.ports.wait_for(
            lambda info: info.is_application and same_serial(info.serial_number, self.config.serial),
            self.timeouts.application_port_s)
        if again is None:
            raise cfg.Refused("the board did not come back after `reboot`: section 10 cannot go on")
        port, repeater = self._open_repeater(again)
        try:
            banner = repeater.read_banner(self.timeouts.banner_s)
            if not banner.get("repeater_id"):
                # The banner is printed about a second after the reset and only while a host
                # holds the port, and the port is not back before 1.4 s: on a real board the
                # lines are gone before anything can read them (p3.4-02). The key itself is
                # still there - `get public.key`, upstream's own line for the same 32 bytes.
                key = self._value(self.ask(repeater, "get public.key"))
                if re.match(r"^[0-9A-Fa-f]{64}$", key or ""):
                    banner["repeater_id"] = key.upper()
                    banner["id_from"] = "get public.key"
            return banner
        finally:
            port.close()

    def password(self):
        """A password nobody keeps: generated here, sent once, written down nowhere. The unit is a
        bench unit (the task's own words), and this is the only copy there ever was."""
        if self._password is None:
            import secrets
            self._password = secrets.token_urlsafe(18)
        return self._password

    # docs/provisioning.md section 3, with a bench unit's own values. The reply to expect, or
    # None where upstream's wording is not quoted anywhere and only "not an error" is held.
    PROVISION = (
        ("set name bench-walk", "OK"),
        ("set lat 55.3250", "OK"),
        ("set lon 10.4900", "OK"),
        ("set owner.info a bench unit|tools/bench walk", "OK"),
        ("set guest.password hello", "OK"),
        ("region default dk", None),
        ("region save", "OK"),
    )

    def _provision(self, app):
        """docs/provisioning.md section 3 at the console, or section 4 over the link when the
        dongle is there - the task's "through `linkcli.py provision` (if the dongle is there) or
        the console", and never both."""
        dongle = (self.ports.find_serial(self.config.dongle_serial)
                  if self.config.dongle_serial else None)
        if dongle is not None:
            self.skip("10.4", "the dongle is on this machine, so the same lines go over the link "
                              "- the page's line below - and not twice")
            return self._provision_over_the_link(dongle)
        self.skip("10.6", "no dongle on this machine (bench.local.json's `dongle_serial`): the "
                          "same lines went in at the USB port, above")
        port, repeater = self._open_repeater(app)
        try:
            repeater.drain(0.3)
            sent, saw, bad = [], [], []
            for line, expected in self.PROVISION:
                reply = self.ask(repeater, line)
                sent.append(line)
                saw.append("%s -> %s" % (_starred(line), self.safe(reply) or "(nothing)"))
                if expected is None:
                    if not reply or reply.strip().lower().startswith("err"):
                        bad.append(line)
                elif expected not in reply:
                    bad.append(line)
            accepted = self._set_the_password(repeater)
            self.step("10.4", "docs/provisioning.md section 3, line for line, at the USB port",
                      ok=not bad and accepted, sent=sent + ["password ****"], saw="\n".join(saw),
                      blanks={"section 10 provisioned": "%d line(s) and the password, at the "
                                                        "console" % len(self.PROVISION)},
                      note=("these were not taken: %s" % ", ".join(bad)) if bad else
                           "the admin password was generated here, sent once and written down "
                           "nowhere: this is a bench unit (docs/provisioning.md section 5)")
            for line, expected, same in (("get name", "bench-walk", None),
                                         ("get lat", "55.3250", same_number),
                                         ("get owner.info", "a bench unit|tools/bench walk", None)):
                self.check_line("10.4", repeater, line, expected, blank="section 10 %s" % line,
                                same=same,
                                note="upstream prints every digit of the float it keeps"
                                     if same else None)
            scope = self.ask(repeater, "region default")
            self.step("10.4", "`region default` reads back", ok=bool(scope),
                      sent=["region default"], saw=scope,
                      blanks={"section 10 region default": scope})
        finally:
            port.close()

    def _set_the_password(self, repeater):
        """`password <generated>`: refused outright unless the bench is disposable (console.py),
        and its reply redacted before it reaches anything at all (console.redact)."""
        reply = self.ask(repeater, "password %s" % self.password())
        return consolemod.PASSWORD_ECHO_SHOWN in (reply or "")

    def _provision_over_the_link(self, dongle):
        work = tempfile.mkdtemp(prefix="twinoak-bench-provision-")
        try:
            path = os.path.join(work, "unit.txt")
            with open(path, "w", encoding="utf-8") as fh:
                fh.write("# a bench unit, provisioned by `bench walk 10`\n")
                for line, _expected in self.PROVISION:
                    fh.write(line + "\n")
                fh.write("password %s\n" % self.password())
            code, output = self.runner.run(
                [self.bench.python, os.path.join(self.repo, "tools", "linkcli.py"), "--port",
                 dongle.device, "provision", path], timeout_s=120)
            leaked = self.password() in (output or "")
            return self.step("10.6", "the same lines through `linkcli.py provision`, the password "
                                     "as stars",
                             ok=code == 0 and not leaked and "****" in (output or ""),
                             sent=["linkcli.py --port %s provision <a temporary file>"
                                   % dongle.device],
                             saw=self.safe(output),
                             expected="every line taken, `password ****`, `accepted`",
                             blanks={"section 10 provisioned": "%d line(s) and the password, over "
                                                               "the link" % len(self.PROVISION)},
                             note="the file was written to a temporary directory and removed with "
                                  "it; the password was generated here and is written down "
                                  "nowhere" if not leaked else
                                  "THE PASSWORD APPEARED IN THE TOOL'S OUTPUT - a finding against "
                                  "tools/linkcli.py, and the reason this walk looks")
        finally:
            shutil.rmtree(work, ignore_errors=True)

    def _identity_afterwards(self, app, rid):
        port, repeater = self._open_repeater(app)
        try:
            repeater.drain(0.3)
            key = self._value(self.ask(repeater, "get public.key"))
            name = self._value(self.ask(repeater, "get name"))
        finally:
            port.close()
        self.step("10.7", "the node is the same node: `get public.key` is the `Repeater ID:` of "
                          "the first boot, and the name is the one just set",
                  ok=bool(key) and key.upper() == (rid or "").upper(),
                  sent=["get public.key", "get name"], saw="%s, name %r" % (key, name),
                  expected=rid, blanks={"section 10 public key": key, "section 10 name": name},
                  note="MeshMaster needs this key and the admin password; the password this walk "
                       "set is one nobody has, which is what a bench unit is "
                       "(docs/provisioning.md section 5)")

    # ---- the run, and the restore that closes it

    def _repeater_image(self):
        """This walk's repeater image, inspected, or None when it is not there or a guard refuses
        it. It is what a walk leaves on the board - section 9's own last line included - so it is
        what a board found with nothing but its bootloader records (runs.record_board)."""
        path = self.image or own_image_of("repeater", self.config.board, self.repo)
        if not os.path.isfile(path):
            return None
        try:
            return self.bench.inspect(path)
        except cfg.Refused:
            return None

    def _open_a_run(self):
        """What is on the board now, recorded before the first flash of this walk (runs.py).

        A board that shows only its bootloader - a new unit, with its base and no application -
        can say nothing, so the run records the repeater image this walk puts back instead
        (p3.5-01); without it the restore at the end has nothing to flash and the line differs,
        which is what the first board on this bench did (p3.4-02)."""
        run = load_run(self.home, self.config.serial)
        if run is None:
            run = record_board(self.config, self.ports, self.runner, self.say, self.home,
                               found_image=self.image, timeouts=self.timeouts,
                               python=self.bench.python, repo=self.repo,
                               bootloader_image=self._repeater_image())
            self.result.note("This walk opened the run: %s" % run.describe())
        else:
            self.result.note("A run was already open for this board: %s" % run.describe())
        return run

    def _restore(self, key, what, before=None):
        run = load_run(self.home, self.config.serial)
        try:
            path, how = choose_restore_image(run, self.config.board, self.repo)
        except RestoreNotPossible as exc:
            return self.step(key, what, ok=False, saw="RESTORE NOT POSSIBLE: %s" % exc)
        image = self.bench.inspect(path)
        try:
            code = self.bench.restore_locked(run, image, how)
        except (FlashError, cfg.Refused) as exc:
            # A restore that could not flash is a *differs* and not the end of the walk: the
            # result file of everything before it is the evidence (p3.4-02, where a scanner's
            # watchdog ended two restores and took the walk's result file with them).
            return self.step(key, what, ok=False, sent=["bench restore"],
                             saw="RESTORE FAILED: %s" % exc,
                             blanks={"section %s the restore" % self.section.number: "failed"})
        said = self.bench.last_restore or {}
        ok = code == cfg.EXIT_OK
        checks = []
        for field, label in (("repeater_id", "Repeater ID"), ("name", "name"),
                             ("version", "fw.version")):
            if (before or {}).get(field):
                same = (said.get(field) or None) == before[field]
                checks.append("%s %s" % (label, "matches" if same else
                                         "is %r and was %r" % (said.get(field), before[field])))
                ok = ok and same
        return self.step(key, what, ok=ok, sent=["bench restore"],
                         saw="%s%s" % (self.bench.last_restore_line or "(no last line)",
                                       ("\n" + "; ".join(checks)) if checks else ""),
                         blanks={"section %s the restore" % self.section.number:
                                 self.bench.last_restore_line or "none"})


def _stamp(started):
    return started.replace("-", "").replace(":", "").replace("T", "-").rstrip("Z")


def _rel(path, repo):
    return os.path.relpath(path, repo).replace("\\", "/")


# ----------------------------------------------------------------------------- the result file

def result_markdown(result):
    """The walk as the file under out/bench/: every step, every blank, and what a person owes."""
    section = result.section
    counts = result.counts()
    lines = []
    say = lines.append
    say("# bench walk section %s - %s" % (section.number, section.title))
    say("")
    say("Walked **%s** at commit `%s` by `tools/bench/walk.py` (`bench walk %s`)."
        % (result.started, result.commit or "unknown", section.number))
    config = result.config
    say("The bench: board `%s`, serial `%s`, rf `%s`, identity `%s`, dongle `%s`."
        % (config.board, config.serial, config.rf, config.identity,
           config.dongle_serial or "none"))
    say("")
    say("**%d ok, %d differs, %d need a person, %d skipped** over %d step(s) and the %d line(s) "
        "of docs/bench.md section %s."
        % (counts[OK], counts[DIFFERS], counts[PERSON], counts[SKIPPED], len(result.steps),
           len(section.lines), section.number))
    say("")
    for note in result.notes:
        say("- %s" % note)
    if result.notes:
        say("")
    say("| the line | what was checked | status |")
    say("|---|---|---|")
    for step in result.steps:
        say("| %s %s | %s | %s |" % (step.line.key, "/".join(step.line.tasks),
                                     step.what.replace("|", "\\|"), step.status))
    say("")
    say("## The steps")
    for line in section.lines:
        mine = [step for step in result.steps if step.line is line]
        if not mine:
            continue
        say("")
        say("### %s - %s (`%s`)" % (line.key, line.about, "`, `".join(line.tasks)))
        say("")
        say("> %s" % line.anchor)
        say("")
        say("**%s.**" % result.line_status(line))
        for step in mine:
            say("")
            say("- **%s** - %s" % (step.status, step.what))
            for sent in step.sent:
                say("  - sent: `%s`" % sent)
            if step.saw:
                say("  - saw:")
                say("")
                say("    ```")
                for one in str(step.saw).splitlines():
                    say("    %s" % one)
                say("    ```")
            if step.expected is not None:
                say("  - expected: `%s`" % step.expected)
            if step.note:
                say("  - %s" % step.note)
    blanks = [(key, value) for step in result.steps for key, value in sorted(step.blanks.items())]
    say("")
    say("## The blanks of docs/bench.md, filled in")
    say("")
    if blanks:
        for key, value in blanks:
            say("- **%s** -> %s"
                % (key, "______ (not read here)" if value in (None, "") else value))
    else:
        say("- none: nothing this walk checked has a blank in it.")
    person = result.of(PERSON)
    say("")
    say("## Still a person's")
    say("")
    if person:
        for step in person:
            say("- **%s (%s)** %s - %s" % (step.line.key, "/".join(step.line.tasks), step.what,
                                           step.note or "a person's"))
    else:
        say("- nothing: every line of this section is a script's.")
    differs = result.of(DIFFERS)
    if differs:
        say("")
        say("## What differs - a follow-up for the task that wrote the line, never a fix here")
        say("")
        for step in differs:
            say("- **%s (%s)** %s: saw %s, expected `%s`"
                % (step.line.key, "/".join(step.line.tasks), step.what,
                   ("`%s`" % str(step.saw).replace("\n", " / ")) if step.saw else "nothing",
                   step.expected))
    if result.files:
        say("")
        say("## What else this walk left on disk")
        say("")
        for path, what in result.files:
            say("- `%s` - %s" % (path.replace("\\", "/"), what))
    say("")
    return "\n".join(lines)


def result_path(result, out_dir):
    return os.path.join(out_dir, "walk-%s-%s-%s.md" % (result.section.number,
                                                       _stamp(result.started),
                                                       result.short_commit))


def write_result(result, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    path = result_path(result, out_dir)
    text = result_markdown(result)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)
    return path, text


# ----------------------------------------------------------------------------- ticking the page

def bullet_bounds(text, anchor):
    """(start, end) of the `- [ ] ...` or `- [x] ...` bullet of docs/bench.md that carries
    `anchor`, or None when the anchor is not there or is not unique - a walk whose page has moved
    ticks nothing."""
    where = text.find(anchor)
    if where < 0 or text.find(anchor, where + 1) >= 0:
        return None
    start = text.rfind("\n- ", 0, where)
    if start < 0:
        return None
    start += 1
    end = len(text)
    for probe in (text.find("\n- ", where), text.find("\n\n", where), text.find("\n#", where)):
        if 0 <= probe < end:
            end = probe
    return start, end


def tick_page(text, result):
    """docs/bench.md with every line this walk confirmed ticked `[x] ... (bench, <date>,
    <commit>)`. Returns (the page, the keys ticked, the keys that were ticked already, the keys it
    could not find).

    A line that is already `- [x] ` is *already ticked* and not missing: walking a section twice
    changes nothing (p3.3-03's decision 1), and the words should say that rather than that the
    page lost a line it did not lose (tasks/phase-3.4/FOLLOWUPS.md, p3.5-01)."""
    ticked, already, missing = [], [], []
    tag = " (bench, %s, %s)" % (result.date, result.short_commit)
    for line in result.confirmed_lines():
        bounds = bullet_bounds(text, line.anchor)
        if bounds is None:
            missing.append(line.key)
            continue
        start, end = bounds
        bullet = text[start:end]
        if bullet.startswith("- [x] "):
            already.append(line.key)
            continue
        if not bullet.startswith("- [ ] "):
            missing.append(line.key)
            continue
        body = "- [x] " + bullet[len("- [ ] "):]
        if tag.strip() not in body:
            body = body.rstrip() + tag
        text = text[:start] + body + text[end:]
        ticked.append(line.key)
    return text, ticked, already, missing


def followups_for(result, repo=REPO):
    """{the path of a phase's FOLLOWUPS.md: the item(s) to add} for every line that differs. A
    differs is a finding for the task that wrote the line and never a fix in the walking task."""
    out = {}
    for line in result.differing_lines():
        steps = [step for step in result.steps if step.line is line and step.status == DIFFERS]
        for task in line.tasks:
            match = re.match(r"^p([0-9.]+)-(\d+)$", task)
            if not match:
                continue
            path = os.path.join(repo, "tasks", "phase-%s" % match.group(1), "FOLLOWUPS.md")
            item = ["- [ ] **[%s] `docs/bench.md` %s differs from the board** (a bench walk of %s "
                    "at `%s`). %s" % (match.group(2), line.key, result.date, result.short_commit,
                                      line.about)]
            for step in steps:
                item.append("  %s: saw `%s`, expected `%s`."
                            % (step.what,
                               str(step.saw).replace("\n", " / ") if step.saw else "nothing",
                               step.expected))
            item.append("  The walk's result file under `out/bench/` has the transcript.")
            out.setdefault(path, []).append("\n".join(item))
    return out


def write_followups(result, repo=REPO):
    """Each differing line, appended to the follow-ups of the task that wrote it."""
    written = []
    for path, items in sorted(followups_for(result, repo).items()):
        if not os.path.isfile(path):
            continue
        with open(path, "r", encoding="utf-8") as fh:
            text = fh.read()
        block = "\n## From a bench walk of section %s (%s)\n\n%s\n" % (result.section.number,
                                                                      result.date,
                                                                      "\n".join(items))
        # newline="": the repository is LF everywhere, in the working tree too
        # (.gitattributes), and a tool that writes a page on Windows must not leave it CRLF.
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(text.rstrip("\n") + "\n" + block)
        written.append(path)
    return written


# ----------------------------------------------------------------------------- the command

def run_walk(bench, args):
    """`bench walk <section>`: one lock, one walk, one result file. Exit 1 when a step differs -
    a board that did not do what the page says, which is this tool's exit 1."""
    section = str(args.section)
    if section not in SECTIONS:
        raise cfg.Refused("there is no walk for section %s: docs/bench.md's walkable sections are "
                          "%s (the rest need a radio, a phone, a scope probe or the roof)"
                          % (section, ", ".join(WALKABLE)))
    bench.require_board()
    walker = Walker(bench, section, image=args.image, scanner_image=args.scanner_image,
                    collect_s=args.collect, baseline_s=args.baseline,
                    password=getattr(bench, "walk_password", None))
    bench.say("walk section %s - %s" % (walker.section.number, walker.section.title))
    with BoardLock(bench.config.serial, "walk %s" % section, probe=bench.probe, home=bench.home,
                   log=bench.say):
        result = walker.run()
    path, _text = write_result(result, os.path.join(bench.repo, "out", "bench"))
    counts = result.counts()
    bench.say("walked section %s: %d ok, %d differs, %d need a person, %d skipped -> %s"
              % (section, counts[OK], counts[DIFFERS], counts[PERSON], counts[SKIPPED],
                 _rel(path, bench.repo)))
    if args.tick:
        page = os.path.join(bench.repo, "docs", "bench.md")
        with open(page, "r", encoding="utf-8") as fh:
            text = fh.read()
        new, ticked, already, missing = tick_page(text, result)
        if new != text:
            # newline="", for the reason write_followups gives: LF on disk too.
            with open(page, "w", encoding="utf-8", newline="") as fh:
                fh.write(new)
        bench.say("ticked in docs/bench.md: %s" % (", ".join(ticked) or "nothing"))
        if already:
            bench.say("already ticked in docs/bench.md, so left as they are: %s" % ", ".join(already))
        if missing:
            bench.say("not found in docs/bench.md, so not ticked: %s" % ", ".join(missing))
        for written in write_followups(result, bench.repo):
            bench.say("a differs was written into %s" % _rel(written, bench.repo))
    else:
        confirmed = [line.key for line in result.confirmed_lines()]
        if confirmed:
            bench.say("to tick %s in docs/bench.md: the same walk with --tick"
                      % ", ".join(confirmed))
    return cfg.EXIT_FAILED if counts[DIFFERS] else cfg.EXIT_OK
