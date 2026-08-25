#!/usr/bin/env python3
"""Parse raw psql baseline outputs, compute recall@k and latency stats.

Raw format (psql -A -t with \\timing on): result ids one per line, then
'Time: X ms' after each statement; 'SET' command tags and timing lines of
SET statements are tolerated and skipped.
"""
import argparse
import csv
import glob
import os
import re
import statistics

TIME_RE = re.compile(r"^Time:\s+([0-9.]+)\s*ms")


def read_ivecs(path):
    import numpy as np
    a = np.fromfile(path, dtype=np.int32)
    d = int(a[0])
    return a.reshape(-1, d + 1)[:, 1:]


def parse_raw(path, topk, nq):
    # psql prints a statement's rows, then its 'Time: X ms' line; SET command
    # tags and their timing lines leave zero pending ids and are skipped.
    # OpenTenBase's datanode appends the ORDER BY distance as a 2nd column, so
    # data lines look like "id|distance" — take the first field as the id.
    queries = []  # list of (ids, time_ms)
    cur_ids = []
    for line in open(path):
        line = line.strip()
        m = TIME_RE.match(line)
        if m:
            if cur_ids:  # accept variable result count (ivfflat+inner-product can return < topk)
                queries.append((cur_ids, float(m.group(1))))
            cur_ids = []
            continue
        first = line.split("|")[0]
        if first.isdigit():
            cur_ids.append(int(first))
    if len(queries) != nq:
        raise SystemExit(f"{path}: parsed {len(queries)} queries, expected {nq} — check raw output")
    return queries


def stats(times):
    ts = sorted(times)
    n = len(ts)
    return {
        "mean_ms": round(statistics.mean(ts), 3),
        "p50_ms": round(ts[int(n * 0.50) - 1 if n >= 2 else 0], 3),
        "p95_ms": round(ts[min(n - 1, int(n * 0.95))], 3),
        "p99_ms": round(ts[min(n - 1, int(n * 0.99))], 3),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--truth", required=True, help="sift_groundtruth.ivecs")
    ap.add_argument("--results-dir", required=True)
    ap.add_argument("--lists", type=int, default=1000)
    ap.add_argument("--nq", type=int, default=100)
    ap.add_argument("--topk", type=int, default=10)
    args = ap.parse_args()

    truth = read_ivecs(args.truth)[: args.nq]  # 0-based ids into base set

    rows = []
    for path in sorted(glob.glob(os.path.join(args.results_dir, "raw_*.txt"))):
        name = os.path.basename(path)[4:-4]  # strip raw_ .txt
        qs = parse_raw(path, args.topk, args.nq)
        correct = 0
        for i, (ids, _) in enumerate(qs):
            tset = set(int(x) + 1 for x in truth[i][: args.topk])  # -> 1-based table ids
            correct += len(set(ids) & tset)
        recall = correct / (args.nq * args.topk)
        times = [t for _, t in qs]
        s = stats(times)
        s.update({
            "setting": name,
            "lists": args.lists,
            "recall@K": round(recall, 4),
            "qps": round(1000.0 / s["mean_ms"], 1),
        })
        rows.append(s)

    if not rows:
        raise SystemExit("no raw_*.txt files found")

    # headline metrics first: Recall@K, P50, P95, QPS
    cols = ["setting", "recall@K", "p50_ms", "p95_ms", "qps", "mean_ms", "p99_ms", "lists"]
    out_csv = os.path.join(args.results_dir, "summary.csv")
    with open(out_csv, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        w.writerows(rows)

    widths = {c: max(len(c), *(len(f"{r[c]:.4g}" if isinstance(r[c], float) else str(r[c])) for r in rows)) for c in cols}
    print(" | ".join(c.ljust(widths[c]) for c in cols))
    print("-|-".join("-" * widths[c] for c in cols))
    for r in rows:
        print(" | ".join((f"{r[c]:.4g}" if isinstance(r[c], float) else str(r[c])).ljust(widths[c]) for c in cols))
    print(f"\nwrote {out_csv}")


if __name__ == "__main__":
    main()
