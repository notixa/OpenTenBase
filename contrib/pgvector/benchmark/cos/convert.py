#!/usr/bin/env python3
"""Convert ann-benchmarks HDF5 (train/test/neighbors) to pgvector CSV + ivecs truth.

Usage: convert.py <hdf5_file> <csv_dir> [nq]

Outputs (in csv_dir):
  base.csv    : id<TAB>[v1,v2,...]   id = 1..N (neighbor idx i maps to table id i+1)
  queries.csv : id<TAB>[v1,v2,...]   first nq query vectors
  truth.ivecs : ivecs format (per row: [count][0-based neighbor indices]), consumed by 07_report.py
  dim.txt     : vector dimension

Requires: h5py, numpy.
"""
import sys

import h5py
import numpy as np


def fmt_rows(ids, vecs, out):
    with open(out, "w") as f:
        for i, v in zip(ids, vecs):
            s = ",".join(f"{x:.9g}" for x in v)
            f.write(f"{i}\t[{s}]\n")


def main():
    src = sys.argv[1]
    out = sys.argv[2]
    nq = int(sys.argv[3]) if len(sys.argv) > 3 else 100

    f = h5py.File(src, "r")
    train = np.asarray(f["train"], dtype=np.float32)
    test = np.asarray(f["test"], dtype=np.float32)
    neighbors = np.asarray(f["neighbors"], dtype=np.int32)
    f.close()

    dim = int(train.shape[1])
    fmt_rows(np.arange(1, len(train) + 1), train, f"{out}/base.csv")
    fmt_rows(np.arange(1, nq + 1), test[:nq], f"{out}/queries.csv")

    with open(f"{out}/truth.ivecs", "wb") as fo:
        for row in neighbors[:nq]:
            np.array([len(row)], dtype=np.int32).tofile(fo)
            row.astype(np.int32).tofile(fo)

    with open(f"{out}/dim.txt", "w") as fo:
        fo.write(str(dim))

    print(f"dim={dim} base={len(train)} queries={nq}", file=sys.stderr)


if __name__ == "__main__":
    main()
