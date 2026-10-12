#!/usr/bin/env python3
"""
NoiseScope waterfall / dwell plotter.

Reads NoiseScope NDJSON either live from a serial port or from a saved log
file, and renders:

  * "sweep" lines -> spectrum waterfall (avg or max per pass)
  * "hist"  lines -> waterfall assembled from spectral-scan histograms
  * "dwell" lines -> RSSI-vs-time strip chart for a single frequency

Examples:
  python waterfall_plot.py --port COM7                     # live view
  python waterfall_plot.py --port /dev/ttyACM0 --metric max
  python waterfall_plot.py --file site-log.ndjson --save site.png
  mosquitto_sub -t 'site/+/scanner/#' | python waterfall_plot.py --stdin

Requires: pyserial, matplotlib, numpy  (pip install -r requirements.txt)

Moved here from the hardware repository's software/firmware/noisescope-rak4630/tools/
at commit b798aab043281fa4c9b17802a164fa94be12e206, with the firmware it reads
(src/roles/scanner/, docs/noisescope.md). Apart from these four lines it is that file
byte for byte: the line format it parses has not changed.
"""

import argparse
import json
import sys
import time

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


class Collector:
    """Accumulates NDJSON lines into waterfall rows / dwell series."""

    def __init__(self, metric):
        self.metric = metric
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
    args = p.parse_args()

    col = Collector(args.metric)

    if args.file:
        with open(args.file, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                col.feed(line)
        if col.hist_row:
            col._flush_hist({"ts": 0, "ms": 0})
        fig = render(col, args)
        if fig is None:
            sys.exit("no sweep/hist/dwell lines found in file")
        if args.save:
            fig.savefig(args.save, dpi=150)
            print(f"saved {args.save}")
        else:
            plt.show()
        return

    if args.stdin:
        stream = sys.stdin
        readline = stream.readline
    else:
        import serial  # pyserial
        ser = serial.Serial(args.port, args.baud, timeout=1)
        readline = lambda: ser.readline().decode("utf-8", errors="replace")

    plt.ion()
    fig = plt.figure(figsize=(11, 7))
    print("listening... Ctrl-C to stop")
    try:
        while True:
            line = readline()
            if not line:
                plt.pause(0.05)
                continue
            if not line.strip().startswith("{"):
                if line.strip():
                    print(line.rstrip())
                continue
            if col.feed(line):
                render(col, args, fig)
                plt.pause(0.01)
    except KeyboardInterrupt:
        pass
    finally:
        if args.save and (col.rows or col.dwell_t):
            render(col, args, fig)
            fig.savefig(args.save, dpi=150)
            print(f"\nsaved {args.save}")


if __name__ == "__main__":
    main()
