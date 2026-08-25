#!/bin/bash
# L2 (SIFT1M) scenario — full self-contained config (no parent deps).
# Paths derived from this file: <repo>/contrib/pgvector/benchmark/l2/env.sh

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OTB_ROOT="$(cd "$BENCH_DIR/../../../.." && pwd)"
OTB_BUILD="${OTB_BUILD:-$OTB_ROOT/otb19_build}"
PG_BIN="$OTB_BUILD/bin"
PG_LIB="$OTB_BUILD/lib"

BENCH_HOME="${BENCH_HOME:-$HOME/otb_bench}"
PGDATA="$BENCH_HOME/pgdata"

PGHOST="${PGHOST:-127.0.0.1}"
PGPORT="${PGPORT:-5432}"
PGUSER="${PGUSER:-$(whoami)}"
PGDATABASE="${PGDATABASE:-vector_bench}"
export PGHOST PGPORT PGUSER PGDATABASE

# benchmark params (override on the command line, e.g. LISTS=100 ./run.sh)
LISTS="${LISTS:-1000}"
TOPK="${TOPK:-10}"
NQUERIES="${NQUERIES:-100}"
PROBES_SWEEP="${PROBES_SWEEP:-1 2 5 10 20 40 60 80 100 200 400 1000}"

# scenario-specific
export SCENARIO="l2"
export TABLE="sift_base"
export OPCLASS="vector_l2_ops"
export OP="<->"
export SIFT_DIR="$BENCH_HOME/l2"            # dataset dir (contains sift/*.fvecs)
export CSV_DIR="$SIFT_DIR/csv"              # converted csv + truth + dim
export DATASET_SRC="$SIFT_DIR"              # convert.py input
export RESDIR="$BENCH_DIR/results"
mkdir -p "$SIFT_DIR" "$CSV_DIR" "$RESDIR"

export PATH="$PG_BIN:$PATH"
export LD_LIBRARY_PATH="$PG_LIB:${LD_LIBRARY_PATH:-}"

log() { echo "[$SCENARIO] $*"; }
die() { echo "[$SCENARIO][FATAL] $*" >&2; exit 1; }
