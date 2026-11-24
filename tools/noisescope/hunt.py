#!/usr/bin/env python3
"""The echoes, asked of the board: a list of `scope` lines run, captured and read.

    python tools/noisescope/hunt.py --label bellevue-carrier --carrier 869.618
    python tools/noisescope/hunt.py --label weaker --seconds 20
    python tools/noisescope/hunt.py --list my-lines.txt --label mine

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
    return parser


def main(argv=None, bench=None, out=sys.stdout):
    args = build_parser().parse_args(argv)
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
