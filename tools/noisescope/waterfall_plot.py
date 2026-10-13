#!/usr/bin/env python3
"""
NoiseScope waterfall / dwell / scope plotter.

Reads NoiseScope NDJSON either live from a serial port or from a saved log
file, and renders:

  * "scope" lines -> live spectrum over waterfall (the fast sweep; --scope)
  * "sweep" lines -> spectrum waterfall (avg or max per pass)
  * "hist"  lines -> waterfall assembled from spectral-scan histograms
  * "dwell" lines -> RSSI-vs-time strip chart for a single frequency

Examples:
  python waterfall_plot.py --port COM7                     # live view
  python waterfall_plot.py --port /dev/ttyACM0 --metric max
  python waterfall_plot.py --file site-log.ndjson --save site.png
  mosquitto_sub -t 'site/+/scanner/#' | python waterfall_plot.py --stdin

  # the fast sweep: the tool sends the command itself, and `stop` on exit
  python waterfall_plot.py --port COM7 --scope 863 870
  python waterfall_plot.py --port COM7 --scope 150 960 --points 2048 \\
      --mark 869.4,869.65 --range -130 -60
  python waterfall_plot.py --port COM7 --scope 791 960 \\
      --baseline dummyload.ndjson --save site.png

Requires: pyserial, matplotlib, numpy  (pip install -r requirements.txt)

Moved here from the hardware repository's software/firmware/noisescope-rak4630/tools/
at commit b798aab043281fa4c9b17802a164fa94be12e206, with the firmware it reads
(src/roles/scanner/, docs/noisescope.md). The `sweep`, `hist` and `dwell`
halves are that file byte for byte; `scope` was added by phase 3.1's task 01,
and so was the one thing those three now share with it - the home channel,
read off an `info` line and drawn as a marker.
"""

import argparse
import base64
import collections
import json
import sys
import time
import warnings

import numpy as np
import matplotlib
import matplotlib.pyplot as plt

HIST_B0 = -11.0     # dBm of first histogram bin
HIST_STEP = -4.0    # dB per bin


def hist_to_dbm(bins, metric):
    """Collapse one 33-bin RSSI histogram to a single dBm value."""
    counts = np.asarray(bins, dtype=float)
    total = counts.sum()
    if total <= 0:
        return np.nan
    levels = HIST_B0 + HIST_STEP * np.arange(len(counts))
    if metric == "max":
        # strongest bin with at least 1% occupancy (ignores lone outliers)
        idx = np.nonzero(counts >= max(1.0, 0.01 * total))[0]
        return levels[idx.min()] if idx.size else np.nan
    return float((levels * counts).sum() / total)


# ----------------------------------------------------------------------------
# scope: the fast sweep (phase 3.1's task 01)
# ----------------------------------------------------------------------------

SCOPE_NO_READING = 255      # the level byte for "the radio read nothing here"


def scope_levels_to_dbm(encoded, n):
    """One `lv` field -> an array of dBm, with NaN where there was no reading.

    The frame carries one byte per point, base64-encoded, each byte -2 x dBm as
    the SX126x reports it (0.5 dB steps). 255 is "no reading" and becomes NaN,
    which is what the rest of this tool already means by "no number".
    """
    try:
        raw = np.frombuffer(base64.b64decode(encoded), dtype=np.uint8)
    except (ValueError, TypeError):
        return None
    if raw.size < n:
        return None
    out = -0.5 * raw[:n].astype(float)
    out[raw[:n] == SCOPE_NO_READING] = np.nan
    return out


class ScopeView:
    """The rows of a `scope` stream, for the spectrum and the waterfall.

    One frequency axis at a time: a frame whose span, step or point count
    differs from the rows already held starts a new picture, because the
    settings changed under us (a second `scope` command, or an interval the
    Walter's UART imposed).
    """

    def __init__(self, keep, live=True):
        self.keep = keep
        self.live = live
        self.rows = collections.deque()     # (seconds, dBm array), oldest first
        self.config = None                  # (f0, step, n)
        self.freqs = None
        self.cov = 1.0
        self.rbw = 0.0
        self.agg = ""
        self.sps = 0.0
        self.hold = None                    # max hold since the settings changed
        self.mean_sum = None                # and an average, in the power domain
        self.mean_n = 0

    def feed(self, obj):
        n = int(obj.get("n", 0))
        f0 = obj.get("f0")
        step = obj.get("step")
        if not n or f0 is None or step is None:
            return False
        dbm = scope_levels_to_dbm(obj.get("lv", ""), n)
        if dbm is None:
            return False
        config = (f0, step, n)
        if config != self.config:
            self.config = config
            self.freqs = f0 + np.arange(n) * step / 1000.0
            self.rows.clear()
            self.hold = None
            self.mean_sum = None
            self.mean_n = 0
        self.cov = float(obj.get("cov") or 0.0)
        self.rbw = float(obj.get("rbw") or 0.0)
        self.agg = obj.get("agg", "")
        self.sps = float(obj.get("sps") or 0) / 10.0
        # Live: when the frame arrived. A replay: the device's own millis, which
        # is monotonic per boot and is what spaces the rows of a saved log.
        when = time.monotonic() if self.live else float(obj.get("ms", 0)) / 1000.0
        self.rows.append((when, dbm))
        while len(self.rows) > self.keep:
            self.rows.popleft()
        self.hold = dbm.copy() if self.hold is None else np.fmax(self.hold, dbm)
        # The average is in the power domain, as the device's own `mean`
        # aggregate is: averaging decibels understates a burst.
        with np.errstate(over="ignore"):
            power = np.nan_to_num(np.power(10.0, dbm / 10.0))
        self.mean_sum = power if self.mean_sum is None else self.mean_sum + power
        self.mean_n += 1
        return True

    def average(self):
        if self.mean_sum is None or self.mean_n == 0:
            return None
        with np.errstate(divide="ignore", invalid="ignore"):
            return 10.0 * np.log10(self.mean_sum / self.mean_n)

    def matrix(self):
        """Newest row first, and how many seconds ago each row arrived."""
        if not self.rows:
            return None, None
        now = time.monotonic() if self.live else self.rows[-1][0]
        ages = np.array([now - t for t, _ in self.rows], dtype=float)[::-1]
        mat = np.vstack([v for _, v in self.rows][::-1])
        return mat, ages


def scope_baseline(path):
    """The per-frequency median of a capture's `scope` rows, for --baseline.

    A dummy-load capture holds the receiver's own spurs and the shape of the
    front end; subtracting it leaves the site, which is what section 7 of
    docs/noisescope.md means by "deltas are the signal".
    """
    view = ScopeView(keep=1 << 30, live=False)
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            if obj.get("t") == "scope":
                view.feed(obj)
    mat, _ = view.matrix()
    if mat is None:
        sys.exit(f"no scope lines in {path}")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")      # an all-NaN column is a legitimate gap
        median = np.nanmedian(mat, axis=0)
    return view.freqs, median


def scope_command(args):
    """The `scope` command line this run asks the device for."""
    f0, f1 = args.scope
    # The arguments are positional, so a later one needs the earlier ones; 0 is
    # "the device's default" for all three numbers (docs/noisescope.md).
    tail = [str(int(args.points or 0)), f"{args.rbw or 0:g}",
            str(int(args.interval or 0)), args.agg]
    while tail and tail[-1] in ("max", "0"):
        tail.pop()
    return " ".join(["scope", f"{f0:g}", f"{f1:g}"] + tail)


def scope_scale(mat, args):
    """The colour scale: --range, or the 5th to 99.5th percentile of the rows."""
    if args.range:
        return args.range[0], args.range[1]
    finite = mat[np.isfinite(mat)]
    if finite.size == 0:
        return -135.0, -70.0
    lo, hi = np.percentile(finite, [5.0, 99.5])
    if hi - lo < 10.0:                       # a flat picture still needs a scale
        hi = lo + 10.0
    return float(lo), float(hi)


def render_scope(view, args, fig, home=None):
    """The spectrum on top, the waterfall below, on one frequency axis."""
    mat, ages = view.matrix()
    if mat is None:
        return None
    hold = view.hold
    average = view.average()
    unit = "dBm"
    if args.baseline_values is not None:
        base_freqs, base = args.baseline_values
        shift = np.interp(view.freqs, base_freqs, base, left=np.nan, right=np.nan)
        mat = mat - shift
        hold = hold - shift if hold is not None else None
        average = average - shift if average is not None else None
        unit = "dB over baseline"

    fig.clf()
    top, bottom = fig.subplots(2, 1, sharex=True, height_ratios=[1.0, 1.6])

    if hold is not None:
        top.plot(view.freqs, hold, lw=0.8, color="#c44", label="max hold")
    if average is not None:
        top.plot(view.freqs, average, lw=0.8, color="#888", label="average")
    top.plot(view.freqs, mat[0], lw=1.2, color="#247", label="now")
    top.set_ylabel(f"Level ({unit})")
    top.grid(True, alpha=0.25, lw=0.5)
    top.legend(fontsize=8, loc="upper right", ncol=3)
    title = (f"NoiseScope scope - {view.freqs[0]:.3f} to {view.freqs[-1]:.3f} MHz, "
             f"{len(view.freqs)} points, RBW {view.rbw:.1f} kHz, "
             f"{view.agg or '?'}, {view.sps:.1f} sweeps/s")
    if args.baseline_values is not None:
        title += " (baseline subtracted)"
    top.set_title(title, fontsize=10)

    vmin, vmax = scope_scale(mat, args)
    # Cell edges: one more than the number of columns, and one more than the
    # number of rows. The newest row is at the top, so the time axis counts
    # backwards from now.
    df = (view.freqs[1] - view.freqs[0]) if len(view.freqs) > 1 else 0.025
    f_edges = np.append(view.freqs - df / 2, view.freqs[-1] + df / 2)
    if len(ages) > 1:
        mid = (ages[1:] + ages[:-1]) / 2.0
        y_edges = np.concatenate(([max(0.0, 2 * ages[0] - mid[0])], mid,
                                  [2 * ages[-1] - mid[-1]]))
    else:
        y_edges = np.array([max(ages[0] - 0.1, 0.0), ages[0] + 0.1])
    im = bottom.pcolormesh(f_edges, y_edges, mat, cmap="viridis", vmin=vmin, vmax=vmax,
                           shading="flat")
    bottom.set_ylim(y_edges[-1], y_edges[0])      # newest at the top
    bottom.set_xlabel("Frequency (MHz)")
    bottom.set_ylabel("Seconds ago")
    bottom.grid(False)
    cb = fig.colorbar(im, ax=[top, bottom], pad=0.01)
    cb.set_label(unit)

    # Where the reading has gaps - the step wider than the receiver bandwidth -
    # part of the span was never looked at. The device says so with `cov`, and a
    # picture that did not say it would be a lie.
    if 0.0 < view.cov < 1.0:
        for ax in (top, bottom):
            ax.axvspan(f_edges[0], f_edges[-1], color="#999999", alpha=0.18, zorder=0)
        top.text(0.01, 0.03, f"cov {view.cov:.2f}: gaps between points", fontsize=8,
                 color="#444", transform=top.transAxes)

    marks = list(args.mark or [])
    if home is not None:
        marks.append(home)
    for mhz in marks:
        if view.freqs[0] <= mhz <= view.freqs[-1]:
            for ax in (top, bottom):
                ax.axvline(mhz, color="#e07000", lw=0.8, ls="--", alpha=0.8, zorder=3)
    return fig


# ----------------------------------------------------------------------------
# sweep / hist / dwell
# ----------------------------------------------------------------------------


class Collector:
    """Accumulates NDJSON lines into waterfall rows / dwell series."""

    def __init__(self, metric, scope=None):
        self.metric = metric
        self.scope = scope      # a ScopeView, or None when `scope` is not wanted
        self.home = None        # the home channel, off an `info` line
        self.rows = []          # list of (timestamp_label, freqs, values)
        self.hist_row = {}      # freq -> value, flushed on wrap-around
        self.hist_last_f = None
        self.dwell_t = []
        self.dwell_avg = []
        self.dwell_max = []

    def _label(self, obj):
        ts = obj.get("ts", 0)
        if ts:
            return time.strftime("%H:%M:%S", time.localtime(ts))
        return f'{obj.get("ms", 0) / 1000.0:.0f}s'

    def feed(self, line):
        line = line.strip()
        if not line.startswith("{"):
            return False
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            return False
        t = obj.get("t")
        if t == "info":
            # The home channel, for a marker on every picture: what the repeater
            # of this band listens on, which is the frequency that matters.
            home = obj.get("home") or {}
            self.home = home.get("f")
            return False
        if t == "scope":
            return self.scope is not None and self.scope.feed(obj)
        if t == "sweep":
            n = obj["n"]
            freqs = obj["f0"] + np.arange(n) * obj["step"] / 1000.0
            vals = np.asarray(obj[self.metric if self.metric in obj else "avg"], dtype=float)
            self.rows.append((self._label(obj), freqs, vals))
            return True
        if t == "hist":
            f = obj["f"]
            if self.hist_last_f is not None and f <= self.hist_last_f and self.hist_row:
                self._flush_hist(obj)
            self.hist_row[f] = hist_to_dbm(obj["bins"], self.metric)
            self.hist_last_f = f
            return True
        if t == "dwell":
            self.dwell_t.append(obj.get("ts") or obj.get("ms", 0) / 1000.0)
            self.dwell_avg.append(obj["avg"])
            self.dwell_max.append(obj["max"])
            return True
        return False

    def _flush_hist(self, obj):
        freqs = np.array(sorted(self.hist_row))
        vals = np.array([self.hist_row[f] for f in freqs])
        self.rows.append((self._label(obj), freqs, vals))
        self.hist_row = {}

    def waterfall(self):
        if not self.rows:
            return None, None, None
        freqs = self.rows[-1][1]
        mat = np.full((len(self.rows), len(freqs)), np.nan)
        for i, (_, f, v) in enumerate(self.rows):
            if len(v) == len(freqs):
                mat[i, :] = v
        labels = [r[0] for r in self.rows]
        return freqs, mat, labels


def render(col, args, fig=None):
    # A scope stream is its own picture, and it is the one this run asked for.
    if col.scope is not None and col.scope.rows:
        if fig is None:
            fig = plt.figure(figsize=(11, 8))
        return render_scope(col.scope, args, fig, col.home)

    freqs, mat, labels = col.waterfall()
    has_dwell = len(col.dwell_t) > 1
    n_plots = (1 if mat is not None else 0) + (1 if has_dwell else 0)
    if n_plots == 0:
        return None

    if fig is None:
        fig = plt.figure(figsize=(11, 7))
    fig.clf()
    axes = fig.subplots(n_plots, 1, squeeze=False)[:, 0]
    ax_i = 0

    if mat is not None:
        ax = axes[ax_i]; ax_i += 1
        # cell edges: one more than the number of columns/rows
        df = (freqs[1] - freqs[0]) if len(freqs) > 1 else 0.025
        f_edges = np.append(freqs - df / 2, freqs[-1] + df / 2)
        im = ax.pcolormesh(f_edges, np.arange(mat.shape[0] + 1), mat,
                           cmap="viridis", vmin=args.vmin, vmax=args.vmax,
                           shading="flat")
        ax.set_xlabel("Frequency (MHz)")
        ax.set_ylabel("Sweep")
        step = max(1, len(labels) // 12)
        ax.set_yticks(np.arange(len(labels))[::step] + 0.5)
        ax.set_yticklabels(labels[::step], fontsize=7)
        ax.set_title(f"NoiseScope waterfall ({args.metric} RSSI)", fontsize=11)
        ax.grid(False)
        for mhz in list(args.mark or []) + ([col.home] if col.home else []):
            if f_edges[0] <= mhz <= f_edges[-1]:
                ax.axvline(mhz, color="#e07000", lw=0.8, ls="--", alpha=0.8)
        cb = fig.colorbar(im, ax=ax, pad=0.01)
        cb.set_label("dBm")

    if has_dwell:
        ax = axes[ax_i]
        t = np.asarray(col.dwell_t, dtype=float)
        t = t - t[0]
        ax.plot(t, col.dwell_max, lw=1, label="max", color="#c44")
        ax.plot(t, col.dwell_avg, lw=1.5, label="avg", color="#247")
        ax.set_xlabel("Time (s)")
        ax.set_ylabel("RSSI (dBm)")
        ax.set_title("Dwell", fontsize=11)
        ax.legend(fontsize=8, loc="upper right")
        ax.grid(True, alpha=0.25, lw=0.5)

    fig.tight_layout()
    return fig


def marks_of(text):
    """--mark 869.4,869.65"""
    if not text:
        return []
    return [float(w) for w in text.replace(" ", "").split(",") if w]


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--port", help="serial port (COM7, /dev/ttyACM0, ...)")
    src.add_argument("--file", help="saved NDJSON log")
    src.add_argument("--stdin", action="store_true", help="read NDJSON from stdin")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--metric", choices=["avg", "max"], default="avg")
    p.add_argument("--vmin", type=float, default=-135.0, help="colour scale floor (dBm)")
    p.add_argument("--vmax", type=float, default=-70.0, help="colour scale ceiling (dBm)")
    p.add_argument("--save", help="write PNG here (on exit for live mode)")
    scope = p.add_argument_group(
        "the fast sweep", "scope <f0> <f1>: the in-radio sweep, spectrum over waterfall. With"
        " --port the command is sent for you, and `stop` on exit.")
    scope.add_argument("--scope", nargs=2, type=float, metavar=("F0", "F1"),
                       help="span in MHz, 150 <= f0 < f1 <= 960")
    scope.add_argument("--points", type=int, help="16..2048 (default: the device's)")
    scope.add_argument("--rbw", type=float, help="resolution bandwidth in kHz (default: the"
                                                 " device's, from the step)")
    scope.add_argument("--interval", type=int, help="ms between frames, 0 = one a sweep")
    scope.add_argument("--agg", choices=["max", "mean", "last"], default="max",
                       help="what a frame holds of the sweeps since the last one")
    scope.add_argument("--range", nargs=2, type=float, metavar=("LO", "HI"),
                       help="fixed colour scale, in dBm - or in dB over the baseline when"
                            " --baseline is given (default: the 5th to 99.5th percentile of the"
                            " rows held)")
    scope.add_argument("--mark", type=marks_of, metavar="MHZ[,MHZ...]",
                       help="vertical markers, e.g. band edges 869.4,869.65")
    scope.add_argument("--baseline", help="an NDJSON capture (a 50 ohm dummy load) whose"
                                         " per-frequency median is subtracted")
    scope.add_argument("--rows", type=int, default=240, help="waterfall rows kept (default 240)")
    scope.add_argument("--fps", type=float, default=8.0, help="redraws a second (default 8)")
    args = p.parse_args()

    args.baseline_values = scope_baseline(args.baseline) if args.baseline else None
    view = ScopeView(args.rows, live=bool(args.port or args.stdin)) if \
        (args.scope or args.file or args.stdin or args.baseline) else None
    col = Collector(args.metric, view)

    if args.file:
        with open(args.file, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                col.feed(line)
        if col.hist_row:
            col._flush_hist({"ts": 0, "ms": 0})
        fig = render(col, args)
        if fig is None:
            sys.exit("no scope/sweep/hist/dwell lines found in file")
        if args.save:
            fig.savefig(args.save, dpi=150)
            print(f"saved {args.save}")
        else:
            plt.show()
        return

    port = None
    if args.stdin:
        readline = sys.stdin.readline
    else:
        import serial  # pyserial
        port = serial.Serial(args.port, args.baud, timeout=0.05)
        readline = lambda: port.readline().decode("utf-8", errors="replace")
        # `info` for the home channel's marker, and the mode this run wants. The
        # device answers an `ack` naming what it took - the interval it raised
        # the frames to on a 115200 UART, among other things - which is chatter
        # worth printing.
        port.write(b"info\n")
        if args.scope:
            command = scope_command(args)
            print(f"> {command}")
            port.write((command + "\n").encode("ascii"))

    plt.ion()
    fig = plt.figure(figsize=(11, 8))
    print("listening... Ctrl-C to stop")
    next_draw = 0.0
    dirty = False
    try:
        while True:
            line = readline()
            if not line:
                plt.pause(0.02)
            elif not line.strip().startswith("{"):
                if line.strip():
                    print(line.rstrip())
            elif col.feed(line):
                dirty = True
            # The picture has its own frame rate, whatever the arrival rate: a
            # scope at fifty sweeps a second must not be redrawn fifty times.
            now = time.monotonic()
            if dirty and now >= next_draw:
                render(col, args, fig)
                plt.pause(0.001)
                next_draw = now + (1.0 / args.fps if args.fps > 0 else 0.0)
                dirty = False
    except KeyboardInterrupt:
        pass
    finally:
        if port is not None and args.scope:
            try:
                port.write(b"stop\n")       # the plugin never outlives the mode
                port.flush()
            except Exception:               # a port already gone is not a failure here
                pass
        if args.save and (col.rows or col.dwell_t or view.rows):
            render(col, args, fig)
            fig.savefig(args.save, dpi=150)
            print(f"\nsaved {args.save}")


if __name__ == "__main__":
    main()
