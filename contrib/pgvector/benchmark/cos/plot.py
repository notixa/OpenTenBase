#!/usr/bin/env python3
"""Plot the IVFFlat baseline recall-latency curve from summary.csv (07_report.py output).

Produces two panels:
  left : recall@k vs latency (log-x) — the classic ANN tradeoff curve, each
         point labelled with its probes value; the exact seq-scan reference is
         shown as a star.
  right: recall@k and latency each vs probes (log-x) — how both respond to the
         ivfflat.probes knob.

Usage: ./08_plot.py [summary.csv] [--out results/baseline_curve.png]
"""
import argparse
import csv
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def load(path):
    rows = list(csv.DictReader(open(path)))
    if not rows:
        raise SystemExit(f"{path}: empty")
    return rows


def split(rows):
    seq, idx = None, []
    for r in rows:
        s = r["setting"]
        if s == "seq":
            seq = r
        elif s.startswith("l"):
            probes = int(s.split("_p")[1])
            idx.append((probes, r))
    idx.sort()
    return seq, idx


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="?", default="results/summary.csv")
    ap.add_argument("--out", default=None)
    ap.add_argument("--topk", type=int, default=10)
    args = ap.parse_args()

    rows = load(args.csv)
    seq, idx = split(rows)
    if not idx:
        raise SystemExit(f"{args.csv}: no ivfflat rows found")

    # group by lists (supports multiple lists in one summary)
    groups = {}
    for probes, r in idx:
        groups.setdefault(r["lists"], []).append((probes, r))

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5.5))
    cmap = plt.get_cmap("tab10")

    for gi, (lists, pts) in enumerate(sorted(groups.items())):
        color = cmap(gi % 10)
        probes = [p for p, _ in pts]
        recall = [float(r["recall@K"]) for _, r in pts]
        mean_ms = [float(r["mean_ms"]) for _, r in pts]
        label = f"ivfflat lists={lists}"

        ax1.plot(mean_ms, recall, marker="o", ms=5, color=color, label=label)
        for p, x, y in zip(probes, mean_ms, recall):
            ax1.annotate(str(p), (x, y), textcoords="offset points",
                         xytext=(6, 2), fontsize=7, color=color)

        ax2.plot(probes, recall, marker="o", ms=4, color=color,
                 label=f"recall@{args.topk} (lists={lists})")
        ax2.plot(probes, mean_ms, marker="s", ms=4, color=color, ls="--",
                 label=f"mean latency ms (lists={lists})")

    if seq is not None:
        ax1.scatter([float(seq["mean_ms"])], [float(seq["recall@K"])],
                    marker="*", s=220, color="red", zorder=5,
                    label=f"exact seq scan ({float(seq['mean_ms']):.0f} ms)")

    ax1.set_xscale("log")
    ax1.set_xlabel("mean latency (ms, log scale)")
    ax1.set_ylabel(f"recall@{args.topk}")
    ax1.set_title("recall vs latency tradeoff")
    ax1.grid(True, which="both", ls=":", alpha=0.5)
    ax1.legend(fontsize=8)

    ax2.set_xscale("log")
    ax2.set_xlabel("ivfflat.probes (log scale)")
    ax2.set_ylabel("value")
    ax2.set_title("recall & latency vs probes")
    ax2.grid(True, which="both", ls=":", alpha=0.5)
    ax2.legend(fontsize=8)

    fig.tight_layout()

    out = args.out or os.path.join(os.path.dirname(os.path.abspath(args.csv)),
                                   "baseline_curve.png")
    fig.savefig(out, dpi=130)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
