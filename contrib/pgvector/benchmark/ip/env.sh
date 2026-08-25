#!/bin/bash
# IP (Last.fm inner product) scenario — full self-contained config (no parent deps).
# Paths derived from this file: <repo>/contrib/pgvector/benchmark/ip/env.sh

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
export SCENARIO="ip"
export TABLE="lastfm_base"
export OPCLASS="vector_ip_ops"
export OP="<#>"
export DATASET_URL="http://ann-benchmarks.com/lastfm-64-dot.hdf5"
export DATASET_FILE="$BENCH_HOME/ip/lastfm-64-dot.hdf5"
export CSV_DIR="$BENCH_HOME/ip/csv"
export DATASET_SRC="$DATASET_FILE"          # convert.py input (hdf5 file)
export RESDIR="$BENCH_DIR/results"
mkdir -p "$(dirname "$DATASET_FILE")" "$CSV_DIR" "$RESDIR"

export PATH="$PG_BIN:$PATH"
export LD_LIBRARY_PATH="$PG_LIB:${LD_LIBRARY_PATH:-}"

log() { echo "[$SCENARIO] $*"; }
die() { echo "[$SCENARIO][FATAL] $*" >&2; exit 1; }
