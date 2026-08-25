#!/usr/bin/env python3
"""Convert SIFT1M fvecs/ivecs to pgvector CSV + ivecs truth.

Usage: convert.py <sift_dir> <csv_dir> [nq]

Outputs (in csv_dir):
  base.csv    : id<TAB>[v1,v2,...]   id = 1..N (neighbor idx i maps to table id i+1)
  queries.csv : id<TAB>[v1,v2,...]   first nq query vectors
  truth.ivecs : ivecs format (copied from sift_groundtruth.ivecs), consumed by 07_report.py
  dim.txt     : vector dimension
"""
import shutil
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
    sift_dir = sys.argv[1]
    csv_dir = sys.argv[2]
    nq = int(sys.argv[3]) if len(sys.argv) > 3 else 100

    base = read_fvecs(f"{sift_dir}/sift/sift_base.fvecs")
    query = read_fvecs(f"{sift_dir}/sift/sift_query.fvecs")
    assert base.shape[1] == query.shape[1]

    fmt_rows(np.arange(1, len(base) + 1), base, f"{csv_dir}/base.csv")
    fmt_rows(np.arange(1, nq + 1), query[:nq], f"{csv_dir}/queries.csv")
    shutil.copyfile(f"{sift_dir}/sift/sift_groundtruth.ivecs", f"{csv_dir}/truth.ivecs")

    with open(f"{csv_dir}/dim.txt", "w") as f:
        f.write(str(base.shape[1]))

    print(f"dim={base.shape[1]} base={len(base)} queries={nq}", file=sys.stderr)


if __name__ == "__main__":
    main()
