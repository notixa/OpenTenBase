#!/usr/bin/env python3
"""Convert SIFT1M fvecs/ivecs to pgvector-friendly CSV.

Outputs (in --out dir):
  base.csv    : id<TAB>[v1,v2,...]   ids 1..1,000,000 (id-1 == index used by ground truth)
  queries.csv : id<TAB>[v1,v2,...]   first --nq queries, ids 1..N
Floats are printed with %.9g (exact float32 round-trip).
"""
import argparse
import sys
import numpy as np


def read_fvecs(path):
    a = np.fromfile(path, dtype=np.int32)
    if a.size == 0:
        raise ValueError(f"empty file: {path}")
    d = int(a[0])
    if a.size % (d + 1) != 0:
        raise ValueError(f"{path}: size {a.size} not multiple of {d + 1}")
    mat = a.reshape(-1, d + 1)
    if not (mat[:, 0] == d).all():
        raise ValueError(f"{path}: inconsistent dims")
    return mat[:, 1:].copy().view(np.float32)


def fmt_rows(ids, vecs, out):
    with open(out, "w") as f:
        for i, v in zip(ids, vecs):
            s = ",".join(f"{x:.9g}" for x in v)
            f.write(f"{i}\t[{s}]\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sift-dir", required=True, help="dir containing sift_base.fvecs etc.")
    ap.add_argument("--out", required=True)
    ap.add_argument("--nq", type=int, default=100)
    args = ap.parse_args()

    base = read_fvecs(f"{args.sift_dir}/sift/sift_base.fvecs")
    query = read_fvecs(f"{args.sift_dir}/sift/sift_query.fvecs")
    print(f"base: {base.shape}, query: {query.shape}", file=sys.stderr)
    assert base.shape[1] == query.shape[1], f"dim mismatch: base={base.shape[1]} query={query.shape[1]}"

    fmt_rows(np.arange(1, len(base) + 1), base, f"{args.out}/base.csv")
    fmt_rows(np.arange(1, args.nq + 1), query[: args.nq], f"{args.out}/queries.csv")
    print(f"wrote {args.out}/base.csv and {args.out}/queries.csv (nq={args.nq})", file=sys.stderr)


if __name__ == "__main__":
    main()
