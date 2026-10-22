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


# ----------------------------------------------------------------------------- the firmware's
#                                                                               own numbers

# VolatileRTCClock's built-in date, which every boot of a board with no RTC starts at: 15 May
# 2024 (MeshCore's src/helpers/ArduinoHelpers.h at the pin 81dbae59).
UPSTREAM_CLOCK_EPOCH = 1715770351
# The clock keeper's: how far ahead of the file a restored clock is put when the file's value was
# not that restart's own second, and the floor below which no epoch is believed
# (src/roles/repeater/clock/ClockKeeper.h, kSaveIntervalSeconds and kSaneEpochFloor).
SAVE_INTERVAL_S = 3600
SANE_EPOCH_FLOOR = 1767225600
SANE_EPOCH_CEILING = 4102444800
# The battery thresholds' window and the hysteresis between them
# (src/common/power/BatteryWatch.h; thresholdsAcceptable is reproduced below).
THRESHOLD_FLOOR_MV = 2500
THRESHOLD_CEILING_MV = 4300
HYSTERESIS_MV = 100


def sane_epoch(epoch):
    return SANE_EPOCH_FLOOR <= epoch < SANE_EPOCH_CEILING


def thresholds_acceptable(low_mv, crit_mv):
    """src/common/power/BatteryWatch.cpp, line for line: both inside the window, and low far
    enough above critical that their hysteresis bands do not overlap."""
    if crit_mv < THRESHOLD_FLOOR_MV or low_mv > THRESHOLD_CEILING_MV:
        return False
    if low_mv <= crit_mv:
        return False
    return (low_mv - crit_mv) > HYSTERESIS_MV


# ----------------------------------------------------------------------------- the consoles


class FakeRepeaterFirmware(object):
    """Upstream's console, as far as the bench needs it - and as far as the walks of
    docs/bench.md sections 1 and 10 ask it (walk.py): the two `sensor list` pages with upstream's
    own 134-byte cut, the settings a `get` reads back, and a first boot after an `erase`."""

    # The ten settings, in the order upstream lists them (src/roles/repeater/app/Settings.cpp;
    # test/test_settings measures the paging this reproduces).
    SETTINGS = ("fw.version", "walter.wake", "link.errors", "walter.seen", "clock.state",
                "power.low_mv", "power.crit_mv", "obs.wake", "client.refused", "firstboot.state")

    # The six preferences the first-boot profile changes, as upstream's own `MyMesh` constructor
    # leaves them (examples/simple_repeater/MyMesh.cpp:891-911 and the NodePrefs defaults of
    # src/helpers/CommonCLI.h at the pin 81dbae59; `loop.detect` is `minimal` since
    # repeater-v1.18.0, where it was `off`). An `erase` puts these back, so that the boot
    # after it is really a first boot and the walk's `get`s mean something.
    UPSTREAM_DEFAULTS = {"path.hash.mode": "0", "dutycycle": "50.0%",
                         "agc.reset.interval": "0", "loop.detect": "minimal",
                         "flood.max.unscoped": "64", "flood.advert.interval": "47"}

    def __init__(self, repeater_id, version="0.0.0-dev", name="repeater",
                 upstream="repeater-v1.18.0 (81dbae59)", answers=None, first_boot=False,
                 settings=None, prints_banner=True, epoch=None, clock_state="unset",
                 clock_file=None, rtc_epoch=None, clock_sync_sender=0, low_mv=3500,
                 crit_mv=3300, forgets_thresholds=False, forgets_clock=False,
                 refuses_profile=False):
        self.repeater_id = repeater_id
        # False: the boot lines went out before any host held the port, which is what a real
        # board does - the port is not back before 1.4 s (p3.4-02). The boot still happened.
        self.prints_banner = prints_banner
        self.version = version
        self.name = name
        self.upstream = upstream
        self.password = "not-a-password"
        self.guest_password = ""
        self.answers = dict(answers or {})
        self.received = []
        self.rebooted = 0
        self.erased = 0
        self.firstboot_state = "done" if first_boot else "never"
        self._first_boot_pending = bool(first_boot)
        self.settings = {"lat": "0.0000", "lon": "0.0000", "owner.info": "", "repeat": "on",
                         "region.default": "dk", "pwrmgt.support": "supported",
                         "pwrmgt.bootmv": "0 mV",
                         "pwrmgt.bootreason": "Reset: SREQ; Shutdown: None"}
        # A node that has been provisioned already: the profile's own six, as it left them.
        self.apply_first_boot_profile()
        self.settings.update(settings or {})
        # The clock, as the keeper has it on a board with no RTC
        # (src/roles/repeater/clock/ClockKeeper.h; docs/bench.md section 3 is the walk):
        # upstream's own volatile clock, our file beside it, and the state word `clock.state`
        # answers. `rtc_epoch` is a hardware RTC that answers - None on this batch, where U5 is
        # DNP - and `clock_sync_sender` is the timestamp upstream's `clock sync` would use, which
        # at the USB console is 0 because ours calls handleCommand(0, ...).
        self.epoch = UPSTREAM_CLOCK_EPOCH if epoch is None else int(epoch)
        self.clock_state = clock_state
        self.clock_file = clock_file        # (epoch, clean) or None
        self.rtc_epoch = rtc_epoch
        self.clock_sync_sender = clock_sync_sender
        # The two thresholds, and whether they survive a restart (our own preferences file,
        # src/common/power/Prefs.h). False is a board that forgot them, which is a differs.
        self.low_mv = int(low_mv)
        self.crit_mv = int(crit_mv)
        self.forgets_thresholds = forgets_thresholds
        # True: a board with no clock keeper at all - every reset comes up at upstream's 2024
        # default, which is the whole of what docs/bench.md section 3 exists to catch.
        self.forgets_clock = forgets_clock
        # True: a board whose first boot left upstream's defaults in place - a pin that does not
        # know a key, or a profile that never ran. docs/bench.md section 10 is what catches it.
        self.refuses_profile = refuses_profile

    # What the first-boot profile's six lines leave behind, read back the way a `get` reads them
    # (src/roles/repeater/firstboot/Profile.cpp, docs/provisioning.md section 1). The advert
    # interval is **derived**: 60 + (the key's first byte mod 26), mirrored here in Python the way
    # restart() and boot() mirror the clock keeper - so a walk that computes the same number from
    # the `Repeater ID` is checking the firmware's rule and not a constant this file handed it.
    PROFILE_HOURS_MIN = 60
    PROFILE_HOURS_SPAN = 26

    def apply_first_boot_profile(self):
        self.settings["path.hash.mode"] = "1"
        self.settings["dutycycle"] = "10.0%"
        self.settings["agc.reset.interval"] = "4"
        self.settings["loop.detect"] = "moderate"
        self.settings["flood.max.unscoped"] = "15"
        self.settings["flood.advert.interval"] = "%d" % (
            self.PROFILE_HOURS_MIN + (int(self.repeater_id[:2], 16) % self.PROFILE_HOURS_SPAN))

    def banner(self):
        out = "Repeater ID: %s\r\n" % self.repeater_id
        if self._first_boot_pending:
            self._first_boot_pending = False
            self.firstboot_state = "done"
            # The profile runs whether or not a host is holding the port to read the two lines
            # below (on a real board nothing is - p3.4-02), which is why this is here and not
            # behind `prints_banner`.
            if not self.refuses_profile:
                self.apply_first_boot_profile()
            out += ("First boot: a random admin password was set (not shown; set yours with "
                    "'password <new>')\r\n")
            out += ("First boot: the profile ran 6 of 6 lines (sensor get firstboot.state says "
                    "'done'; docs/provisioning.md is the rest)\r\n")
        else:
            self.firstboot_state = "never"
        return out if self.prints_banner else ""

    def fw_version(self):
        return "twinoak-repeater %s on MeshCore %s" % (self.version, self.upstream)

    # --- the clock (docs/bench.md section 3) ----------------------------------------------------

    def clock_text(self):
        """Upstream's own `clock` reply: `%02d:%02d - %d/%d/%d UTC`, the second not in it
        (src/helpers/CommonCLI.cpp:226 at the pin 81dbae59)."""
        import time as _time
        when = _time.gmtime(self.epoch)
        return "%02d:%02d - %d/%d/%d UTC" % (when.tm_hour, when.tm_min, when.tm_mday,
                                             when.tm_mon, when.tm_year)

    def set_clock(self, epoch):
        """Upstream's `time <epoch>` and `clock sync`, and what the keeper's poll makes of them:
        the clock only goes forward, the state becomes `set`, and both memories are written."""
        if epoch <= self.epoch:
            return "(ERR: clock cannot go backwards)"
        self.epoch = epoch
        if self.clock_state != "rtc":
            self.clock_state = "set"
        if sane_epoch(epoch):
            self.clock_file = (epoch, False)
        return "OK - clock set: %s" % self.clock_text()

    def restart(self):
        """A reset: ClockKeeper::saveBeforeRestart on the way down - the file with `clean` set,
        unless the clock is not a sane epoch, which is what `clkreboot` leaves behind - and
        ClockKeeper::begin on the way up."""
        if sane_epoch(self.epoch):
            self.clock_file = (self.epoch, True)
        if self.forgets_clock:
            self.clock_file = None
        if self.forgets_thresholds:
            self.low_mv, self.crit_mv = 3500, 3300
        self.boot()

    def boot(self):
        """ClockKeeper::begin's order: a hardware RTC that answers with a sane time, else the
        file (its own second when the flag says this restart wrote it, else plus the save
        interval), else upstream's default. The flag is good for exactly one boot."""
        if self.rtc_epoch is not None and sane_epoch(self.rtc_epoch):
            self.epoch = self.rtc_epoch
            self.clock_state = "rtc"
            return
        if self.clock_file is not None:
            epoch, clean = self.clock_file
            self.epoch = epoch if clean else epoch + SAVE_INTERVAL_S
            self.clock_state = "restored"
            self.clock_file = (epoch, False)
            return
        self.epoch = UPSTREAM_CLOCK_EPOCH
        self.clock_state = "unset"

    def sensor_value(self, key):
        if key == "fw.version":
            return self.fw_version()
        if key == "firstboot.state":
            return self.firstboot_state
        if key == "clock.state":
            return self.clock_state
        if key == "power.low_mv":
            return "%d" % self.low_mv
        if key == "power.crit_mv":
            return "%d" % self.crit_mv
        return {"walter.wake": "0", "link.errors": "0", "walter.seen": "never",
                "obs.wake": "0", "client.refused": "0"}.get(key, "")

    def sensor_list(self, start=0):
        """Upstream's own listing: `<n> vars`, a line per setting while fewer than 134 bytes have
        been written, and `... next:<i>` when it stopped early - then the whole reply cut to the
        159 characters of its window, which is what loses the digit (docs/provisioning.md
        section 3; src/roles/repeater/app/Settings.cpp has the arithmetic)."""
        out = "%d vars\n" % len(self.SETTINGS)
        i = start
        while i < len(self.SETTINGS) and len(out) < 134:
            out += "%s=%s\n" % (self.SETTINGS[i], self.sensor_value(self.SETTINGS[i]))
            i += 1
        if i < len(self.SETTINGS):
            out += "... next:%d" % i
        else:
            out = out[:-1]
        return out[:159]

    def handle(self, line):
        """The reply text for one line, or None for a line that answers nothing."""
        self.received.append(line)
        if line in self.answers:
            return self.answers[line]
        if line == "ver":
            return "v1.18.0 (Build: 15 Oct 2026)"
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
        if line == "sensor list":
            return self.sensor_list(0)
        if line.startswith("sensor list "):
            try:
                return self.sensor_list(int(line.split()[-1]))
            except ValueError:
                return "Unknown command: %s" % line
        if line.startswith("sensor get "):
            key = line[len("sensor get "):]
            if key in self.SETTINGS:
                return "> %s" % self.sensor_value(key)
            return "can't find custom var"
        if line.startswith("sensor set "):
            rest = line[len("sensor set "):]
            key, _, value = rest.partition(" ")
            if key in ("power.low_mv", "power.crit_mv") and value.isdigit():
                mv = int(value)
                low = mv if key == "power.low_mv" else self.low_mv
                crit = mv if key == "power.crit_mv" else self.crit_mv
                if mv <= 0xFFFF and thresholds_acceptable(low, crit):
                    self.low_mv, self.crit_mv = low, crit
                    return "ok"
            # Upstream's one other word: a key it does not have, and a value this table refused.
            return "can't find custom var"
        if line == "clock":
            return self.clock_text()
        if line == "clock sync":
            # Upstream sets the clock to the *sender's* timestamp plus one and refuses when that
            # is not ahead of the node's own (CommonCLI.cpp:210-218 at the pin). The USB console
            # has no sender: src/roles/repeater/main.cpp calls handleCommand(0, ...).
            if self.clock_sync_sender > self.epoch:
                return self.set_clock(self.clock_sync_sender + 1)
            return "ERR: clock cannot go backwards"
        if line.startswith("time "):
            try:
                return self.set_clock(int(line[len("time "):].strip()))
            except ValueError:
                return "(ERR: clock cannot go backwards)"
        if line == "get public.key":
            return "> %s" % self.repeater_id.lower()
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
            self.repeater_id = "%02X" % (self.erased % 256) + self.repeater_id[2:]
            self._first_boot_pending = True
            # The file system is gone, so the preferences are gone with it: the next boot finds
            # upstream's own defaults and is a first boot (src/roles/repeater/app/FirstBoot.h).
            self.settings.update(self.UPSTREAM_DEFAULTS)
            return "File system erase: OK"
        if line in ("reboot", "clkreboot"):
            self.rebooted += 1
            if line == "clkreboot":
                # Upstream sets its clock back to the 2024 default and then reboots
                # (CommonCLI.cpp:198-201 at the pin); the keeper's file is what saves the node.
                self.epoch = UPSTREAM_CLOCK_EPOCH
            return None
        if line == "region default":
            return "> %s" % self.settings.get("region.default", "")
        if line.startswith("region default "):
            self.settings["region.default"] = line[len("region default "):]
            return " default scope is now %s" % self.settings["region.default"]
        if line == "region save":
            return "OK"
        if line.startswith("set "):
            rest = line[len("set "):]
            key, _, value = rest.partition(" ")
            if key in self.settings:
                self.settings[key] = value
                return "OK"
            return "Unknown command: %s" % line
        if line.startswith("get "):
            key = line[len("get "):]
            if key in self.settings:
                return "> %s" % self.settings[key]
            return "Unknown command: %s" % line
        return "Unknown command: %s" % line


class FakeScannerFirmware(object):
    """NoiseScope's console, as far as the bench needs it (docs/noisescope.md section 5) - and as
    far as the walk of docs/bench.md section 9 asks it (walk.py): an ack and then the line a scan
    produces, with every field of the page's own examples, and the `stat` heartbeat every ten
    seconds whether anybody asked or not."""

    HEARTBEAT_S = 10.0

    def __init__(self, version="0.0.0-dev", rom="SX1261 V2D 2D02", radio_ok=True,
                 devid="deadbeef01020304", extra_lines=None, sweep_points=281, scope_frames=3,
                 scope_sps=476, answers_data=True, chatter=None, prints_banner=True,
                 reset="soft", wdt="", hangs=True):
        self.version = version
        # What `boot` and `info` say the last reset was, and which watchdog it was when the word
        # is `wdt` (docs/noisescope.md section 5). `hang` moves both, which is what the walk of
        # section 9's watchdog line reads back (p3.5-03).
        self.reset = reset
        self.wdt = wdt
        # False: an image from before p3.5-03, which has no `hang` command - the walk's line is
        # then a person's again and says so.
        self.hangs = hangs
        self.rom = rom
        self.radio_ok = radio_ok
        self.devid = devid
        self.received = []
        self.extra_lines = list(extra_lines or [])  # lines pushed after the next command
        self.chatter = list(chatter or [])          # lines pushed with the next heartbeat, which
                                                    # is what a line nobody asked for looks like
        self.sweep_points = sweep_points
        self.scope_frames = scope_frames
        self.scope_sps = scope_sps
        self.answers_data = answers_data            # False: acks and no scan lines at all
        # False: the `boot` line went out before any host held the port, which is what a real
        # board does (p3.4-02) - `info` is then the only place its fields are.
        self.prints_banner = prints_banner
        self.seq = 0
        self._next_stat = None

    def identity(self):
        return {"fw": "noisescope", "ver": self.version, "role": "scanner", "board": "rak4630-single",
                "hw": "rak4630", "radio": "sx1262"}

    def banner(self):
        if not self.prints_banner:
            return ""
        boot = dict(self.identity())
        boot.update({"t": "boot", "reset": self.reset, "wdt": self.wdt,
                     "radio_ok": self.radio_ok, "rtc": 0, "ina": 0, "ts": 0})
        return json.dumps(boot) + "\n"

    def _stat(self):
        return {"t": "stat", "ts": 0, "ms": 12345, "up": 12, "mode": "monitor",
                "home": 869.618, "rssi": {"n": 9900, "avg": -114.2, "max": -99.5,
                                          "min": -116.0, "dc": 0.03, "th": -106.0}}

    def heartbeat(self, now):
        """The `stat` every ten seconds, which a scanner sends on its own. None until it is due;
        the first one is due ten seconds after the port was first read."""
        if self._next_stat is None:
            self._next_stat = now + self.HEARTBEAT_S
            return None
        if now < self._next_stat:
            return None
        self._next_stat = now + self.HEARTBEAT_S
        out = [json.dumps(self._stat())] + self.chatter
        self.chatter = []
        return "".join(line + "\n" for line in out)

    def _next(self):
        self.seq += 1
        return {"seq": self.seq, "ts": 0, "ms": 1000 * self.seq}

    def _levels(self, n):
        import base64
        return base64.b64encode(bytes(bytearray(n))).decode("ascii")

    def _scan_lines(self, word, words):
        """What a scan answers after its ack: the page's own examples, as many as it would send
        before anybody could call `stop`."""
        if not self.answers_data:
            return []
        if word == "sweep":
            line = {"t": "sweep", "f0": 863.0, "f1": 870.0, "step": 25.0, "rbw": 29.3,
                    "dwell": 20, "n": self.sweep_points, "unit": "dBm",
                    "avg": [-115.2] * self.sweep_points, "max": [-108.9] * self.sweep_points}
            line.update(self._next())
            return [line]
        if word == "hist":
            out = []
            for freq in (869.4, 869.8):
                line = {"t": "hist", "f": freq, "rbw": 29.3, "n": 2048, "b0": -11, "bw_db": 4,
                        "bins": [0] * 33}
                line.update(self._next())
                out.append(line)
            return out
        if word == "dwell":
            line = {"t": "dwell", "f": 869.618, "win": 1000, "n": 990, "avg": -113.8,
                    "max": -100.1, "min": -115.9, "dc": 0.0404, "th": -105.9}
            line.update(self._next())
            return [line]
        if word == "cad":
            line = {"t": "cad", "f": 869.618, "sf": 8, "bw": 62.5, "n": 100, "det": 17}
            line.update(self._next())
            return [line]
        if word == "scope":
            points = 250
            if len(words) > 3:
                try:
                    points = int(words[3]) or 250
                except ValueError:
                    points = 250
            out = []
            for _frame in range(self.scope_frames):
                line = {"t": "scope", "f0": 863.014, "step": 28.0, "rbw": 39.0, "n": points,
                        "cov": 1.393, "agg": "max", "sweeps": 6, "sps": self.scope_sps,
                        "lv": self._levels(points)}
                line.update(self._next())
                out.append(line)
            return out
        return []

    def handle(self, line):
        """The lines the scanner answers with, as text."""
        self.received.append(line)
        words = line.split()
        word = words[0].lower() if words else ""
        out = []
        if word == "info":
            info = dict(self.identity())
            info.update({"t": "info", "build": "Oct 18 2026", "devid": self.devid,
                         "reset": self.reset, "wdt": self.wdt,
                         "rtc": 0, "ina": 0, "radio_ok": 1 if self.radio_ok else 0, "boost": 1,
                         "auto_min": 10, "rom": self.rom,
                         "home": {"f": 869.618, "bw": 62.5, "sf": 8, "cr": 8}})
            out.append(json.dumps(info))
        elif word == "stat":
            out.append(json.dumps(self._stat()))
        elif word == "hang" and self.hangs:
            # The loop stops, the loop watchdog resets the part about eight seconds later, and the
            # unit comes back saying `wdt` (docs/noisescope.md sections 5 and 6). The ack is out
            # before the loop goes quiet - the firmware waits 50 ms for it - so the port is taken
            # away afterwards: FakePort does that, and this is what the next boot will say it was.
            out.append(json.dumps({"t": "ack", "cmd": word, "ok": True,
                                   "msg": "the loop stops for 12000 ms; the loop watchdog resets"
                                          " the unit after 8000 ms of it"}))
            self.reset, self.wdt = "wdt", "loop"
        elif word == "scope" and self.rom != "SX1261 V2D 2D02":
            # The plugin is a program for the radio's own core: another ROM is a refusal, and
            # nothing is uploaded (docs/bench.md section 9, *The fast sweep*, first line).
            out.append(json.dumps({"t": "ack", "cmd": word, "ok": False,
                                   "msg": "radio rom is %r, the sweep plugin needs SX1261 V2D 2D02"
                                          % self.rom}))
        elif word in ("sweep", "hist", "dwell", "cad", "stop", "scope", "home", "boost", "auto", "time",
                      "help"):
            msg = "started"
            if word == "scope":
                msg = ("n=%d step=28.0000kHz rbw=39.0kHz seg=1 cov=1.393 interval=0ms sweep=86ms"
                       % (int(words[3]) if len(words) > 3 and words[3].isdigit() and int(words[3])
                          else 250))
            out.append(json.dumps({"t": "ack", "cmd": word, "ok": True, "msg": msg}))
            out.extend(json.dumps(one) for one in self._scan_lines(word, words))
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
                 enumerate_polls=2, bootloader_serial=None, port_dies_on_reset=True,
                 boot_pid=RAK_BOOT_PID):
        self.serial = serial
        # A real board takes its USB device down with the reset, so the host's open handle dies
        # rather than falling quiet (p3.4-02; console.send_reset). False is a host that saw the
        # port only go silent.
        self.port_dies_on_reset = port_dies_on_reset
        self.bootloader_serial = bootloader_serial or serial
        # 0x0029 is the bootloader after a double-tap; 0x002A is the serial-only DFU the
        # 1200-baud touch asks for, which is what the first board on this bench came up as
        # (boards/rak4631.json, p3.4-02).
        self.boot_pid = boot_pid
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
            return [PortInfo(self.boot_device, self.bootloader_serial, RAK_VID, self.boot_pid, "RAK4631 bootloader")]
        return []

    def _transition(self, to):
        self._next_mode = to
        self._polls_left = self.enumerate_polls
        self.mode = "absent"
        if to == "application":
            # A boot, and so a banner waiting for whoever opens the port next. It is a boot that
            # arms it and not a host letting go of the port: after an `erase` the `First boot:`
            # lines belong to the boot after it (docs/bench.md section 10), so a port opened only
            # to send `reboot` must not be given them.
            self._fresh = True

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


DEAD_PORT = ("Cannot configure port, something went wrong. Original message: OSError(22, 'A device "
             "which does not exist was specified', None, 433)")


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
        self.dead = False       # the reset took the USB device away under this handle
        if not self.is_boot and baud == 115200:
            # the banner a freshly booted board printed: only if the board just came up
            if board.app_port is None and board._fresh:
                self._out += board.firmware.banner().encode("utf-8")
            board.app_port = self
            board._fresh = False

    def write(self, data):
        if self.is_boot:
            return
        if self.dead:
            raise IOError(DEAD_PORT)
        self._in += data
        fw = self.board.firmware
        if isinstance(fw, FakeRepeaterFirmware):
            # echo as typed, as upstream's console does
            self._out += data.replace(b"\r", b"\r\n")
            while b"\r" in self._in:
                line, self._in = self._in.split(b"\r", 1)
                text = line.decode("utf-8", errors="replace")
                reply = fw.handle(text)
                if text in ("reboot", "clkreboot"):
                    fw.restart()
                    self.board._fresh = True
                    self.board.app_port = None
                    self.board.reboot()
                    if self.board.port_dies_on_reset:
                        self.dead = True
                    return
                if reply is not None:
                    self._out += ("  -> %s\r\n" % reply).encode("utf-8")
        else:
            while b"\n" in self._in:
                line, self._in = self._in.split(b"\n", 1)
                text = line.decode("utf-8", errors="replace").strip()
                if text:
                    self._out += fw.handle(text).encode("utf-8")
                    # `hang` is the scanner's one line that ends in a reset. The ack is already in
                    # self._out and the caller reads it - a real board sends it and then goes
                    # quiet - so this handle is left alive and only the board moves.
                    if text.split()[0].lower() == "hang" and getattr(fw, "hangs", False):
                        self.board._fresh = True
                        self.board.app_port = None
                        self.board.reboot()

    def _heartbeat(self):
        """A scanner talks when nobody asked: its `stat` every ten seconds."""
        beat = getattr(self.board.firmware, "heartbeat", None)
        if beat is not None:
            line = beat(self.ports.now())
            if line:
                self._out += line.encode("utf-8")

    def read(self, n, timeout_s):
        if self.dead:
            raise IOError(DEAD_PORT)
        if not self._out:
            self._heartbeat()
        if self._out:
            out, self._out = self._out[:n], self._out[n:]
            return out
        self.ports.clock.sleep(timeout_s)
        self._heartbeat()
        if self._out:
            out, self._out = self._out[:n], self._out[n:]
            return out
        return b""

    def close(self):
        if self.closed:
            return
        self.closed = True
        if not self.is_boot and self.baud == 1200 and self.board.mode == "application":
            self.board.touched()
        if self.board.app_port is self:
            self.board.app_port = None


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


class SlowToOpenPorts(FakePorts):
    """A board whose application port is listed before it can be opened.

    Windows does that with a CDC port that has just enumerated: the first flash of the first
    RAK4630 on this bench had the application come up as COM30 and the open of it fail with
    `FileNotFoundError(2)` while the port was in the list (p3.4-02). The first `fails` opens of the
    application's port after a flash raise that error here - or, with `which="boot"`, of the
    bootloader's port after the touch, which is the port nrfutil is handed. `refused` counts
    them."""

    def __init__(self, boards=None, others=None, clock=None, fails=2, which="app"):
        super().__init__(boards=boards, others=others, clock=clock)
        self.fails = fails
        self.which = which          # "app": after the flash; "boot": the DFU port after the touch
        self.refused = 0

    def open(self, device, baud):
        board = self.boards[0] if self.boards else None
        wanted = board and (board.app_device if self.which == "app" else board.boot_device)
        ready = board and (bool(board.flashes) if self.which == "app" else board.mode == "bootloader")
        if board is not None and device == wanted and ready and self.refused < self.fails:
            self.refused += 1
            self.opened.append((device, baud))
            raise IOError("could not open port %r: FileNotFoundError(2, 'The system cannot find the "
                          "file specified.', None, 2)" % device)
        return super().open(device, baud)


# ----------------------------------------------------------------------------- the tools


class FakeRunner(object):
    """A subprocess runner that runs nothing. adafruit-nrfutil becomes a DFU on the fake board;
    linkcli.py answers out of a table. `calls` is every argv ever given."""

    def __init__(self, boards=None, nrfutil_exit=0, linkcli_output=None, new_firmware=None,
                 linkcli_exit=0, nrfutil_exits=None):
        self.boards = list(boards or [])
        self.nrfutil_exit = nrfutil_exit
        # One code per nrfutil call, in order, for the retry of a DFU that did not take (p3.4-02);
        # when it runs out, `nrfutil_exit` is the answer again.
        self.nrfutil_exits = list(nrfutil_exits or [])
        # How many of the first nrfutil calls fail the way the real one does: the traceback of a
        # write the device did not recognise, and **exit 0** (p3.4-02).
        self.nrfutil_fails_silently = 0
        self.linkcli_output = linkcli_output or ""
        self.linkcli_exit = linkcli_exit
        self.new_firmware = new_firmware
        self.calls = []

    def run(self, argv, timeout_s=None):
        self.calls.append(list(argv))
        joined = " ".join(os.path.basename(str(a)) for a in argv)
        if "adafruit-nrfutil.py" in joined:
            if self.nrfutil_fails_silently > 0:
                self.nrfutil_fails_silently -= 1
                port = argv[argv.index("-p") + 1]
                return 0, ("Upgrading target on %s. Flow control is disabled, Single bank, "
                           "Touch disabled\nFailed to upgrade target. Error is: WriteFile "
                           "failed (PermissionError(13, 'The device does not recognize the "
                           "command', None, 22))\nPossible causes:\n- Target is not in DFU "
                           "mode." % port)
            code = self.nrfutil_exits.pop(0) if self.nrfutil_exits else self.nrfutil_exit
            if code != 0:
                return code, ("Failed to upgrade target. Error is: WriteFile failed "
                              "(PermissionError(13, 'The device does not recognize the command', "
                              "None, 22))")
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
