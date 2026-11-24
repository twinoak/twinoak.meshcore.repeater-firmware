#!/usr/bin/env python3
"""The echoes, asked of the board - or of the unit, as a NoiseScope session's program.

    python tools/noisescope/hunt.py --label bellevue-carrier --carrier 869.618
    python tools/noisescope/hunt.py --label weaker --seconds 20
    python tools/noisescope/hunt.py --list my-lines.txt --label mine
    python tools/noisescope/hunt.py --sessions tools/noisescope/lists/garden-b.txt

**`--sessions` touches nothing.** The echoes have only ever been seen on the unit at Bellevue, so
the rest of the hunt is made there and not on a bench (tasks/phase-3.16/README.md, *The garden
instead of the bench*). A session is not a serial port: its program is at most six typed steps of
at most forty characters, it goes out over LTE and it comes back as a file of lines an hour later.
`--sessions` turns a list into those programs and **checks every rule the two repositories that
take them have** before anybody types one: exit 0 when every session of the list would be taken,
2 when one would not, and no port is opened and nothing of tools/bench is asked for anything on
the way. `tools/noisescope/echoes.py --steps` is what reads the download afterwards.

What it does, in order, for each line of its list: send it on the scanner's console, keep
everything the board says for `--seconds` on the **same open port**, write that to
`out/bench/echoes/<label>/NN-<name>.ndjson`, send `stop`, and run
tools/noisescope/echoes.py over the capture. Then one table for the whole run, and
`out/bench/echoes/<label>/run.json` with every scope of it.

**The board is touched through tools/bench and nothing else** (`tasks/RUNBOOK.md` §2): this
imports tools/bench/bench.py and uses its port finding, its lock per board and its
`ScannerConsole` - the console that refuses to send a password and never prints one. No port is
opened here by hand, nothing is flashed, nothing is erased and `bench restore` is not run. With no
`bench.local.json`, or no port with its serial number, it exits **3, *no bench***, which is not a
failure: the task that would have run it says *Not verified here*
(`docs/bench.md` §0a).

The list. The default is the questions of phase 3.16's task 01 - where the echoes are, whether
they move with the segments, whether the interval is MHz, points or time, how long a tail is, what
a sweep costs, and the same once more with boosted gain off. A line that is not a `scope` is sent
and acked and captures nothing, which is how `boost off` gets into the middle of the list.
`--list <file>` takes another: one line per row, `name = <the line>`, `#` for a comment.

Every `scope` of the list is asked for with **interval 0 and `last`** - a frame per sweep, and
each frame one sweep rather than a max-hold of several - because an echo is to be counted in the
sweep it was made in and a max-hold over a hundred sweeps hides how often it is there.

Exit codes are tools/bench's: 0 done, 1 the board did not answer as asked, 2 refused by a guard,
3 no bench, 4 the board is held by another process.
"""

import argparse
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
BENCH = os.path.join(REPO, "tools", "bench")
for path in (HERE, BENCH):
    if path not in sys.path:
        sys.path.insert(0, path)

import echoes  # noqa: E402

import bench as bench_tool  # noqa: E402  (tools/bench/bench.py: the one way to a board)
import config as cfg  # noqa: E402
from console import ScannerConsole  # noqa: E402


# The carrier the pile's bench has on the antenna port: a plain carrier from the tinySA Ultra on
# the neighbour's own frequency, at about -30 dBm into the radio (tasks/phase-3.16/README.md,
# *Before the run*).
DEFAULT_CARRIER_MHZ = 869.618

# The questions of tasks/phase-3.16/01-the-echoes-are-measured.md, deliverable 6, as lines. `c` is
# the carrier above; every span is written out so that this list is what a person reads and types.
#
#   (name, the line, what it answers)
DEFAULT_LIST = [
    ("wide-467",
     "scope 779.618 959.618 0 467 0 last",
     "where they are, as the unit took it on 23 November: 180 MHz at 467 kHz, six segments"),
    ("narrow-58",
     "scope 863.118 876.118 250 58.6 0 last",
     "and 13 MHz at 58.6 kHz around the carrier: one segment"),
    ("wide-467-shifted",
     "scope 772.618 952.618 0 467 0 last",
     "do they move with the segments? the same 180 MHz begun 7 MHz lower"),
    ("span-60",
     "scope 839.618 899.618 0 467 0 last",
     "60 MHz around the carrier: two segments"),
    ("span-31",
     "scope 854.118 885.118 0 467 0 last",
     "31 MHz around the carrier: one segment, nearly all of what one may measure"),
    ("span-20",
     "scope 859.618 879.618 0 467 0 last",
     "20 MHz around the carrier: one segment"),
    ("wide-467-halved",
     "scope 779.618 959.618 772 467 0 last",
     "is the interval MHz, points or time? the same span with the step halved"),
    ("wide-234",
     "scope 779.618 959.618 0 234.3 0 last",
     "the same span at 234.3 kHz"),
    ("wide-117",
     "scope 779.618 959.618 0 117.3 0 last",
     "at 117.3 kHz"),
    ("wide-58",
     "scope 779.618 959.618 0 58.6 0 last",
     "and at 58.6 kHz, where the points clamp at 2048 and the step is wider than the filter"),
    ("boost-off",
     "boost off",
     "the RX path the repeater does not use, for the last line"),
    ("wide-467-boost-off",
     "scope 779.618 959.618 0 467 0 last",
     "the first line again with boosted gain off"),
    ("boost-on",
     "boost on",
     "and boosted gain back on, which is how the scanner is left"),
]


# ------------------------------------------------------------- what a NoiseScope session takes
#
# Read on 24 November 2026; **these are other repositories' numbers** and not choices of this
# tool's, which is why they are here, once, with their sources beside them:
#
#   * a program is 1 to 6 steps; a step lasts 5 to 300 seconds, and all of them 600 at most;
#   * a step's command is 1 to 40 characters of `a-z`, `0-9`, space and the full stop, and its
#     first word is one of `stat sweep scope hist dwell cad`;
#   * the program as it goes out is at most 384 bytes of
#     `{"slot":0,"steps":[{"command":"...","seconds":N},...]}`;
#   * the Walter keeps 65 536 bytes of lines a step and ends a step that fills it `full`; the
#     server keeps 512 kB and 4 000 lines a session. Both are checked, from the frames a step's
#     seconds and interval would bring and the bytes the firmware's own `scopeFrameBytes()` says
#     a frame of that many points is.
#
# meshmaster.dk `src/MeshMaster.Contracts/NoiseScope.cs`, `NoiseScopeProgram`; the Walter's
# `components/core/include/core/update/scanner_session.h`: `kStepsMax` 6, `kCommandMax` 40.
# Neither repository is this task's to read or to change (tasks/RUNBOOK.md: they are read-only
# here), so a number of theirs that moves moves in this block and nowhere else.
SESSION_STEPS_MIN = 1
SESSION_STEPS_MAX = 6
SESSION_SECONDS_MIN = 5
SESSION_SECONDS_MAX = 300
SESSION_TOTAL_SECONDS_MAX = 600
SESSION_COMMAND_MAX = 40
SESSION_COMMAND_WORDS = ("stat", "sweep", "scope", "hist", "dwell", "cad")
SESSION_COMMAND_CHARS = "abcdefghijklmnopqrstuvwxyz0123456789 ."
SESSION_PROGRAM_BYTES_MAX = 384
SESSION_SLOT = 0
WALTER_STEP_BYTES = 65536

# And what the server keeps of a whole session. Counted here over the `scope` frames alone,
# because they are the only lines this tool can size: a session's `ack`s and its `stat` heartbeat
# are a few kilobytes more over six steps, which is inside the margin every list of
# tools/noisescope/lists/ has (the widest comes to a third of the bytes).
#
# **Neither of these two can be reached before the Walter's**: six steps of 65 536 bytes is
# 393 216, and six steps of as many of the shortest frames as 65 536 bytes hold is 1 452 lines.
# They are checked all the same, because a number of another repository's that moves should move
# here and not be left out as unreachable - and `test_hunt.py` holds the implication itself, so a
# Walter that one day keeps more of a step cannot quietly start overrunning the server.
SESSION_SERVER_BYTES = 512 * 1024
SESSION_SERVER_LINES = 4000

# The port a session's lines come out on: the scanner's management UART, 115200 baud, where a
# `scope` frame's interval has a floor and the interval the line asked for is not always the one
# it gets (`scopeIntervalFloorMs()`, docs/noisescope.md section 5).
WALTER_BAUD = 115200

# 85 seconds a step when a list does not say (tasks/phase-3.16/README.md, *The garden instead of
# the bench*).
SESSION_SECONDS_DEFAULT = 85

# What a session costs Bellevue besides its steps: two flashes and the proof. The README's figure
# is about 27 minutes for a program of nine minutes of steps, so the rest of it is this.
SESSION_OFF_MESH_OVERHEAD_S = 18 * 60

# The line of a list that begins a new session, and the one that sets the seconds after it.
SESSION_MARK = "---"
SESSION_SECONDS_KEY = "seconds"


class Step(object):
    """One typed step of a session's program: the seconds and the command, as the form takes them."""

    def __init__(self, name, command, seconds, about=""):
        self.name = name
        self.command = command
        self.seconds = seconds
        self.about = about
        self.request = echoes.parse_scope(command)

    @property
    def typed(self):
        """The one line meshmaster.dk's form takes: the seconds, then the command."""
        return "%d %s" % (self.seconds, self.command)

    def as_step(self):
        return {"command": self.command, "seconds": self.seconds}


class Session(object):
    """One program: a name, what it asks, and up to six steps."""

    def __init__(self, name, about=""):
        self.name = name
        self.about = about
        self.steps = []

    @property
    def seconds(self):
        return sum(step.seconds for step in self.steps)

    def program(self):
        """The bytes as they go out, built from the same JSON the contract names."""
        return json.dumps({"slot": SESSION_SLOT,
                           "steps": [step.as_step() for step in self.steps]},
                          separators=(",", ":"))

    def off_mesh_seconds(self):
        return SESSION_OFF_MESH_OVERHEAD_S + self.seconds


def step_lines(step):
    """How many lines a step brings back and how many bytes they are: (frames, each, total).

    (None, None, None) for a step this tool cannot size - anything but a `scope`, and a `scope`
    whose numbers the firmware refuses. For a `scope`: the frames come from **the seconds and the
    interval**, where the interval is the one the line asked for or the port's floor, whichever is
    longer, because that is the one the `ack` says it took; and a frame's bytes come from the
    plan's points, by the firmware's own `scopeFrameBytes()` - 762 at 386 points and 582 at 250
    since phase 3.16's task 03 put `us`, `usx` and `dev` on the line (its report, *What a frame
    says now*). That length is the **most** a frame can be, newline included, which is the side to
    be wrong on: a step over the Walter's 65 536 bytes is cut off `full` and a session that is cut
    off costs Bellevue the whole half hour again.
    """
    request = step.request
    if request is None or request.plan is None:
        return (None, None, None)
    plan = request.plan
    interval_ms = max(request.interval_ms, echoes.interval_floor_ms(plan.points, WALTER_BAUD))
    if interval_ms <= 0:
        return (None, None, None)
    each = echoes.frame_bytes(plan.points)
    # Frames at 0, one interval, two intervals... inside the step's own seconds.
    frames = int(math.ceil(step.seconds * 1000.0 / interval_ms))
    return (frames, each, frames * each)


def read_sessions(path):
    """A list as sessions. `hunt.py`'s own format with two more lines in it.

    `name = <the line>` is a step and `#` a comment, as `read_list()` reads them. A line
    `--- <name>` begins a new session, and `seconds = N` sets the seconds of the steps after it
    (SESSION_SECONDS_DEFAULT when it is never said). Steps before the first `---` belong to a
    session named after the file. **The one comment line directly over a step says what that step
    asks** and is printed beside it, and the one over a `---` says what the session asks; a step
    with a comment of two lines gets the second of them, so the lists keep theirs to one.

    A garden step keeps **its own interval and aggregate**: the lines are not rewritten to
    interval 0 and `last` the way `--list` asks for them on a bench, because the signal that
    lights the echoes is a neighbour's traffic rather than a generator, so a frame is the strongest
    reading of a second and not of one sweep.
    """
    sessions = []
    current = None
    seconds = SESSION_SECONDS_DEFAULT
    about = ""
    with open(path, "r", encoding="utf-8") as handle:
        for number, text in enumerate(handle, 1):
            text = text.strip()
            if text.startswith("#"):
                about = text.lstrip("#").strip()
                continue
            if not text:
                continue
            if text.startswith(SESSION_MARK):
                current = Session(text[len(SESSION_MARK):].strip() or "session-%d" % number,
                                  about=about)
                sessions.append(current)
                about = ""
                continue
            if "=" in text:
                name, line = text.split("=", 1)
                name, line = name.strip(), line.strip()
            else:
                name, line = "step-%02d" % number, text
            if name == SESSION_SECONDS_KEY:
                seconds = int(line)
                continue
            if not line:
                continue
            if current is None:
                current = Session(os.path.splitext(os.path.basename(path))[0], about=about)
                sessions.append(current)
            current.steps.append(Step(name, line, seconds, about=about))
            about = ""
    if not sessions:
        raise cfg.Refused("%s has no step in it" % path)
    return sessions


def refusals_of(session):
    """Every rule of *what a session takes* this session breaks, each naming its own number."""
    out = []
    if not SESSION_STEPS_MIN <= len(session.steps) <= SESSION_STEPS_MAX:
        out.append("%s: %d step(s), and a program is %d to %d"
                   % (session.name, len(session.steps), SESSION_STEPS_MIN, SESSION_STEPS_MAX))
    for number, step in enumerate(session.steps, 1):
        where = "%s, step %d (%r)" % (session.name, number, step.command)
        for char in step.command:
            if char not in SESSION_COMMAND_CHARS:
                out.append("%s: it carries %r, and a step's command is `a-z`, `0-9`, space and "
                           "the full stop only" % (where, char))
                break
        if not step.command:
            out.append("%s: no command at all, and a step's command is 1 to %d characters"
                       % (where, SESSION_COMMAND_MAX))
        elif len(step.command) > SESSION_COMMAND_MAX:
            out.append("%s: %d characters, and a step's command is at most %d"
                       % (where, len(step.command), SESSION_COMMAND_MAX))
        first = step.command.split()[0] if step.command.split() else ""
        if first not in SESSION_COMMAND_WORDS:
            out.append("%s: its first word is %r, and a step's command begins with one of %s"
                       % (where, first, " ".join(SESSION_COMMAND_WORDS)))
        if not SESSION_SECONDS_MIN <= step.seconds <= SESSION_SECONDS_MAX:
            out.append("%s: %d seconds, and a step lasts %d to %d"
                       % (where, step.seconds, SESSION_SECONDS_MIN, SESSION_SECONDS_MAX))
        frames, each, total = step_lines(step)
        if total is not None and total > WALTER_STEP_BYTES:
            out.append("%s: %d frame(s) of %d bytes is %d, and the Walter keeps %d bytes of "
                       "lines a step (a step that fills it ends `full`)"
                       % (where, frames, each, total, WALTER_STEP_BYTES))
    frames = [step_lines(step) for step in session.steps]
    lines = sum(one[0] for one in frames if one[0] is not None)
    back = sum(one[2] for one in frames if one[2] is not None)
    if back > SESSION_SERVER_BYTES:
        out.append("%s: %d bytes of frames over %d step(s), and the server keeps %d a session"
                   % (session.name, back, len(session.steps), SESSION_SERVER_BYTES))
    if lines > SESSION_SERVER_LINES:
        out.append("%s: %d frames over %d step(s), and the server keeps %d lines a session"
                   % (session.name, lines, len(session.steps), SESSION_SERVER_LINES))
    if session.seconds > SESSION_TOTAL_SECONDS_MAX:
        out.append("%s: %d seconds over %d step(s), and a program lasts %d at most"
                   % (session.name, session.seconds, len(session.steps),
                      SESSION_TOTAL_SECONDS_MAX))
    program = session.program()
    if len(program.encode("utf-8")) > SESSION_PROGRAM_BYTES_MAX:
        out.append("%s: %d bytes as it goes out, and a program is at most %d"
                   % (session.name, len(program.encode("utf-8")), SESSION_PROGRAM_BYTES_MAX))
    return out


def session_as_dict(session):
    program = session.program()
    out = {
        "name": session.name,
        "about": session.about,
        "steps": [],
        "seconds": session.seconds,
        "program": program,
        "program_bytes": len(program.encode("utf-8")),
        "program_bytes_max": SESSION_PROGRAM_BYTES_MAX,
        "off_mesh_seconds": session.off_mesh_seconds(),
        "frames": sum(one[0] for one in (step_lines(step) for step in session.steps)
                      if one[0] is not None),
        "bytes_back": sum(one[2] for one in (step_lines(step) for step in session.steps)
                          if one[2] is not None),
        "bytes_back_max": SESSION_SERVER_BYTES,
        "frames_max": SESSION_SERVER_LINES,
        "paste": [step.typed for step in session.steps],
        "refusals": refusals_of(session),
    }
    for number, step in enumerate(session.steps, 1):
        frames, each, total = step_lines(step)
        plan = None if step.request is None else step.request.plan
        out["steps"].append({
            "step": number,
            "name": step.name,
            "command": step.command,
            "seconds": step.seconds,
            "typed": step.typed,
            "about": step.about,
            "points": None if plan is None else plan.points,
            "segments": None if plan is None else plan.segments,
            "interval_ms": None if step.request is None else step.request.interval_ms,
            "frames": frames,
            "frame_bytes": each,
            "bytes_back": total,
            "bytes_back_max": WALTER_STEP_BYTES,
        })
    out["taken"] = not out["refusals"]
    return out


def print_sessions(path, sessions, out=sys.stdout, as_json=False):
    """Every session of a list, as the form takes it - or the rule it breaks. 0 or EXIT_REFUSED."""
    rows = [session_as_dict(session) for session in sessions]
    if as_json:
        out.write(json.dumps({"list": path, "sessions": rows,
                              "taken": all(row["taken"] for row in rows)},
                             indent=2, sort_keys=True) + "\n")
        return cfg.EXIT_OK if all(row["taken"] for row in rows) else cfg.EXIT_REFUSED

    out.write("%s: %d session(s)\n" % (path, len(rows)))
    for row in rows:
        out.write("\n%s%s\n" % (row["name"], " - " + row["about"] if row["about"] else ""))
        out.write("  paste into the form (%d step(s), %d s, %d of %d bytes as it goes out):\n\n"
                  % (len(row["steps"]), row["seconds"], row["program_bytes"],
                     row["program_bytes_max"]))
        for step in row["steps"]:
            out.write("    %s\n" % step["typed"])
        out.write("\n  %-5s %-8s %-7s %-5s %-10s %-7s %-20s %s\n"
                  % ("step", "seconds", "points", "seg", "interval", "frames", "bytes back",
                     "what it asks"))
        for step in row["steps"]:
            out.write("  %-5d %-8d %-7s %-5s %-10s %-7s %-20s %s\n"
                      % (step["step"], step["seconds"], _said(step["points"]),
                         _said(step["segments"]),
                         "-" if step["interval_ms"] is None else "%d ms" % step["interval_ms"],
                         _said(step["frames"]),
                         ("-" if step["bytes_back"] is None
                          else "%d of %d" % (step["bytes_back"], step["bytes_back_max"])),
                         step["about"] or step["name"]))
        out.write("\n  the program as it goes out:\n    %s\n" % row["program"])
        out.write("  what comes back: %d frame(s) of %d, %d bytes of %d the server keeps\n"
                  % (row["frames"], row["frames_max"], row["bytes_back"],
                     row["bytes_back_max"]))
        out.write("  Bellevue off the mesh: about %d minutes - %d for the two flashes and the "
                  "proof, %.1f of steps\n"
                  % (int(math.floor(row["off_mesh_seconds"] / 60.0 + 0.5)),
                     SESSION_OFF_MESH_OVERHEAD_S // 60, row["seconds"] / 60.0))
        for refusal in row["refusals"]:
            out.write("  REFUSED %s\n" % refusal)
    taken = all(row["taken"] for row in rows)
    out.write("\n%s\n" % ("every session of this list would be taken as it is written"
                          if taken else
                          "this list would not be taken: %d refusal(s) above"
                          % sum(len(row["refusals"]) for row in rows)))
    return cfg.EXIT_OK if taken else cfg.EXIT_REFUSED


def _said(value):
    return "-" if value is None else str(value)


def read_list(path):
    """`name = <line>` a row, `#` a comment. Returns the same triples DEFAULT_LIST has."""
    out = []
    with open(path, "r", encoding="utf-8") as handle:
        for number, text in enumerate(handle, 1):
            text = text.strip()
            if not text or text.startswith("#"):
                continue
            if "=" in text:
                name, line = text.split("=", 1)
                name, line = name.strip(), line.strip()
            else:
                name, line = "line-%02d" % number, text
            if not line:
                continue
            out.append((name, line, "from %s" % os.path.basename(path)))
    if not out:
        raise cfg.Refused("%s has no line in it" % path)
    return out


class Hunt(object):
    """One run of a list against the board, through a tools/bench `Bench`."""

    def __init__(self, bench, out=sys.stdout, repo=None):
        self.bench = bench
        self.out = out
        self.repo = repo or getattr(bench, "repo", REPO)

    def relative(self, path):
        """A path as a reader of the table should see it: under the repository it was made in."""
        try:
            return os.path.relpath(path, self.repo).replace("\\", "/")
        except ValueError:      # another drive on Windows
            return path.replace("\\", "/")

    def say(self, line):
        self.out.write(line + "\n")
        self.out.flush()

    def run(self, lines, label, seconds, carrier_mhz, directory):
        self.bench.load()
        config = self.bench.config
        app, boot = self.bench.require_board()
        if app is None:
            raise cfg.Refused("the board is in its bootloader on %s: there is no console to talk "
                              "to (`bench flash` the scanner first)" % boot.device)
        if not os.path.isdir(directory):
            os.makedirs(directory)
        self.say("hunt: %s, %s on %s, rf %s" % (label, config.board, app.device, config.rf))
        self.say("  %d line(s), %.0f s each, into %s"
                 % (len(lines), seconds, self.relative(directory)))

        captures = []
        with self.bench.lock("hunt"):
            port = self.bench.ports.open_ready(app.device, 115200, self.bench.timeouts.open_s)
            try:
                console = ScannerConsole(port, log=(lambda s: None),
                                         disposable=config.disposable,
                                         now=self.bench.ports.now, sleep=self.bench.ports.sleep)
                # `info` answers an `info` line and no ack, so the send waits out its timeout
                # (tools/bench/walk.py reads it the same way). The run file keeps it: a table of
                # echoes is worth what the image and the ROM under it are worth.
                _ack, batch = console.send("info", timeout_s=3.0)
                found = batch.of_type("info")
                info = found[0] if found else None
                if info:
                    self.say("  info: %s %s, rom %r, boost %s"
                             % (info.get("fw"), info.get("ver"), info.get("rom"),
                                info.get("boost")))
                for number, (name, line, about) in enumerate(lines, 1):
                    captures.append(self._one(console, number, name, line, about, seconds,
                                              directory))
            finally:
                port.close()

        return self._table(label, lines, captures, carrier_mhz, directory, info)

    def _one(self, console, number, name, line, about, seconds, directory):
        """One line: sent, then everything that follows it on the same open port."""
        is_scope = line.split()[0].lower() == "scope"
        ack, batch = console.send(line)
        ok = ack is None or bool(ack.get("ok", True))
        self.say("  %2d %-20s %s" % (number, name, line))
        if ack is not None:
            self.say("     ack %s%s" % ("ok " if ok else "NOT ok ", ack.get("msg", "")))
        else:
            self.say("     (no ack)")
        record = {"name": name, "line": line, "about": about, "ok": ok,
                  "ack": ack, "path": None}
        if not is_scope or not ok:
            return record
        # The frames arrive after the ack on this same port: a port opened again for them has
        # already missed the first ones (p3.5-02's probes, tools/bench/bench.py's own note).
        _more, raw = console.collect_raw(float(seconds))
        path = os.path.join(directory, "%02d-%s.ndjson" % (number, name))
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            # The ack first, as JSON again, so that the capture carries the plan the board took;
            # then the lines as the board sent them, byte for byte.
            handle.write(json.dumps(ack, sort_keys=True) + "\n")
            for obj in batch.objects:
                if obj is not ack:
                    handle.write(json.dumps(obj, sort_keys=True) + "\n")
            handle.write(raw)
        record["path"] = path
        # The mode streams until it is stopped: every `scope` of the list gets its own `stop`, so
        # that the frames of one line are not still arriving while the next is sent.
        console.send("stop")
        return record

    def _table(self, label, lines, captures, carrier_mhz, directory, info):
        """echoes.py over every capture, one table for the run, and run.json beside them."""
        run = {"label": label, "carrier_mhz": carrier_mhz, "info": info, "lines": []}
        failed = 0
        every = []      # every scope of the run, for the one question the run exists to answer
        self.say("")
        self.say("%-20s %-6s %-6s %-5s %-5s %-8s %s"
                 % ("line", "scopes", "echoes", "seg", "sps", "carrier", "the interval of each"))
        for record in captures:
            row = dict(record)
            row.pop("path", None)
            row["capture"] = None if not record["path"] else self.relative(record["path"])
            if not record["ok"]:
                failed += 1
            if record["path"]:
                scopes, skipped, bad = echoes.read_capture(record["path"])
                scopes = [scope for scope in scopes if scope.frames]
                readings = [echoes.ScopeReading(scope, carrier_mhz=carrier_mhz)
                            for scope in scopes]
                unit, line = echoes.verdict(readings)
                row["skipped_lines"] = skipped
                row["unreadable_frames"] = bad
                row["follows"] = unit
                row["verdict"] = line
                row["scopes"] = [reading.as_dict() for reading in readings]
                every.extend(readings)
                found = sum(len(reading.echoes) for reading in readings)
                segs = ",".join(str(reading.plan.segments) for reading in readings) or "-"
                sps = ",".join("?" if reading.scope.sps is None
                               else "%.0f" % reading.scope.sps for reading in readings) or "-"
                self.say("%-20s %-6d %-6d %-5s %-5s %-8s %s"
                         % (record["name"], len(readings), found, segs, sps,
                            _carrier(readings), " | ".join(_interval(one) for one in readings)))
                if not readings:
                    failed += 1
                    self.say("%-20s %s" % ("", "no frame came back for this line"))
            else:
                row["scopes"] = []
                self.say("%-20s %-6s %-6s %-5s %-5s %-8s %s"
                         % (record["name"], "-", "-", "-", "-", "-",
                            "not a `scope`" if record["ok"] else "the board refused the line"))
            run["lines"].append(row)

        # The one question the list exists to answer: the scopes of the whole run read together.
        # A line on its own cannot answer it - inside one step and one bandwidth MHz, points and
        # microseconds are the same number (echoes.py's docstring).
        unit, line = echoes.verdict(every)
        run["follows"] = unit
        run["verdict"] = line
        self.say("")
        self.say("what they follow, over the %d scope(s) of this run: %s" % (len(every), line))

        path = os.path.join(directory, "run.json")
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(run, handle, indent=2, sort_keys=True)
            handle.write("\n")
        self.say("the captures and %s are in %s"
                 % (os.path.basename(path), self.relative(directory)))
        return cfg.EXIT_OK if failed == 0 else cfg.EXIT_FAILED


def _interval(reading):
    """One scope's interval in the three units, short enough for a row of the table."""
    if not reading.echoes:
        return "no echo"
    if not any(reading.interval.values()):
        return "%d echo(es), no interval" % len(reading.echoes)
    parts = []
    for unit in echoes.UNITS:
        found = reading.interval[unit]
        parts.append("?" if not found
                     else "%s %s" % (echoes.number_in(found[0], unit), echoes.UNIT_NAMES[unit]))
    return "every " + " = ".join(parts)


def _carrier(readings):
    """Where the carrier's own peak landed, in points from the point it was asked for."""
    offsets = [one.carrier_offset_points for one in readings
               if one.carrier_offset_points is not None]
    if not offsets:
        return "?"
    return ",".join("%+d" % one for one in offsets)


def build_parser():
    parser = argparse.ArgumentParser(prog="hunt", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--label", default="echoes",
                        help="what this run is called, and the directory it goes in "
                             "(default: echoes)")
    parser.add_argument("--carrier", type=float, default=DEFAULT_CARRIER_MHZ, metavar="MHZ",
                        help="the frequency the generator is on (default %.3f)"
                             % DEFAULT_CARRIER_MHZ)
    parser.add_argument("--seconds", type=float, default=10.0,
                        help="how long each `scope` is captured for (default 10)")
    parser.add_argument("--list", default=None, dest="list_file", metavar="FILE",
                        help="another list of lines (`name = <line>` a row)")
    parser.add_argument("--out", default=None,
                        help="where the captures go (default out/bench/echoes/<label>)")
    parser.add_argument("--repo", default=REPO, help="the repository root (default: this checkout)")
    parser.add_argument("--sessions", default=None, dest="sessions_file", metavar="FILE",
                        help="a list as NoiseScope session programs, checked against every rule "
                             "a session has and printed to paste into meshmaster.dk's form. No "
                             "board is touched and no port is opened")
    parser.add_argument("--json", action="store_true", help="--sessions, as JSON")
    return parser


def main(argv=None, bench=None, out=sys.stdout):
    args = build_parser().parse_args(argv)
    # The garden's half of this tool, and the whole of it: no bench is loaded, no port is opened
    # and nothing of tools/bench is asked for anything. A list is read, priced and printed.
    if args.sessions_file:
        try:
            sessions = read_sessions(args.sessions_file)
        except cfg.Refused as exc:
            out.write("refused: %s\n" % exc)
            return cfg.EXIT_REFUSED
        except (IOError, OSError, ValueError) as exc:
            out.write("refused: %s cannot be read as a list of sessions (%s)\n"
                      % (args.sessions_file, exc))
            return cfg.EXIT_REFUSED
        return print_sessions(args.sessions_file, sessions, out=out, as_json=args.json)
    bench = bench or bench_tool.Bench(repo=args.repo)
    hunt = Hunt(bench, out=out, repo=args.repo)
    directory = args.out or os.path.join(args.repo, "out", "bench", "echoes", args.label)
    try:
        lines = read_list(args.list_file) if args.list_file else list(DEFAULT_LIST)
        return hunt.run(lines, args.label, args.seconds, args.carrier, directory)
    except cfg.NoBench as exc:
        hunt.say("no bench: %s" % exc)
        return cfg.EXIT_NO_BENCH
    except cfg.Refused as exc:
        hunt.say("refused: %s" % exc)
        return cfg.EXIT_REFUSED
    except bench_tool.Locked as exc:
        hunt.say("locked: %s" % exc)
        return cfg.EXIT_LOCKED


if __name__ == "__main__":
    sys.exit(main())
