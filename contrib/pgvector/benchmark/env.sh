#!/bin/bash
# Common environment for pgvector SIFT1M baseline benchmark (single-node PostgreSQL 19)
# Sourced by 01/02/03/04/05/06 scripts. Override any variable before sourcing.

# --- Paths (derived: benchmark dir is <repo>/contrib/pgvector/benchmark) ---
BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OTB_ROOT="$(cd "$BENCH_DIR/../../.." && pwd)"
OTB_BUILD="${OTB_BUILD:-$OTB_ROOT/otb19_build}"          # configure --prefix
PG_BIN="$OTB_BUILD/bin"
PG_LIB="$OTB_BUILD/lib"

# Runtime artifacts are kept OUTSIDE the git tree
BENCH_HOME="${BENCH_HOME:-$HOME/otb_bench}"
SIFT_DIR="$BENCH_HOME/sift1m"           # downloaded & extracted dataset (reused)
PGDATA="$BENCH_HOME/pgdata"             # single-node data directory
mkdir -p "$BENCH_HOME" "$SIFT_DIR" "$BENCH_DIR/results"

# --- Connection (single-node PostgreSQL) ---
PGHOST="${PGHOST:-127.0.0.1}"
PGPORT="${PGPORT:-5432}"
PGUSER="${PGUSER:-$(whoami)}"
PGDATABASE="${PGDATABASE:-vector_bench}"
export PGHOST PGPORT PGUSER PGDATABASE

# --- Benchmark parameters ---
LISTS="${LISTS:-1000}"                              # ivfflat lists (1M rows -> ~rows/1000)
TOPK="${TOPK:-10}"                                  # recall@10
NQUERIES="${NQUERIES:-100}"                         # sift_query.fvecs has 10000; we use first N
PROBES_SWEEP="${PROBES_SWEEP:-1 2 5 10 20 40 60 80 100 200 400 1000}"

export PATH="$PG_BIN:$PATH"
export LD_LIBRARY_PATH="$PG_LIB:${LD_LIBRARY_PATH:-}"

log()  { echo "[bench] $*"; }
die()  { echo "[bench][FATAL] $*" >&2; exit 1; }
