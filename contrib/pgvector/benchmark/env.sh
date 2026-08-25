#!/bin/bash
# Common environment for pgvector SIFT1M baseline benchmark (OpenTenBase, centralized mode)
# Sourced by 01/03/04/05/06 scripts. Override any variable before sourcing.

# --- Paths (derived: benchmark dir is <otb>/contrib/pgvector/benchmark) ---
BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OTB_ROOT="$(cd "$BENCH_DIR/../../.." && pwd)"
OTB_BUILD="${OTB_BUILD:-$OTB_ROOT/otb_build}"          # configure --prefix
PG_BIN="$OTB_BUILD/bin"
PG_LIB="$OTB_BUILD/lib"

# Runtime artifacts are kept OUTSIDE the git tree
BENCH_HOME="${BENCH_HOME:-$HOME/otb_bench}"
SIFT_DIR="$BENCH_HOME/sift1m"           # downloaded & extracted dataset
OTB_DATA="$BENCH_HOME/cluster"          # gtm/cn/dn data directories
mkdir -p "$BENCH_HOME" "$SIFT_DIR" "$OTB_DATA" "$BENCH_DIR/results"

# --- Cluster ports (OpenTenBase manual conventions) ---
GTM_PORT=50001
CN_PORT=55000
DN_PORT=56000
CN_POOLER_PORT=6611
DN_POOLER_PORT=6621
CN_FORWARD_PORT=6681
DN_FORWARD_PORT=6680

# --- Which node each phase connects to ---
# OpenTenBase DNs are READ-ONLY for direct app connections by design, so:
#   * LOADING must go through the CN (table create + COPY distribute to DNs)
#   * IVFFlat INDEX BUILD must run DIRECTLY on the DN: building via CN dispatch
#     breaks pgvector's block sampling (RelationGetNumberOfBlocks returns bad
#     value in the remote txn) -> "ivfflat index created with little data" ->
#     recall 0%. Direct DN build samples correctly.
#   * BENCHMARK QUERIES run directly on the DN (reads are allowed; the IVFFlat
#     scan + distance computation are the kernel code under optimization).
# PGPORT (used by 05/06) targets the DN; 04_load_data.sh overrides to the CN.
PGHOST="${PGHOST:-127.0.0.1}"
PGPORT="${PGPORT:-$DN_PORT}"
PGUSER="${PGUSER:-$(whoami)}"
PGDATABASE="${PGDATABASE:-vector_bench}"
export PGHOST PGPORT PGUSER PGDATABASE

# --- Benchmark parameters ---
LISTS="${LISTS:-1000}"                              # ivfflat lists (1M rows -> ~rows/1000)
TOPK="${TOPK:-10}"                                  # recall@10
NQUERIES="${NQUERIES:-100}"                         # sift_query.fvecs has 10000; we use first N
PROBES_SWEEP="${PROBES_SWEEP:-1 2 5 10 20 40 60 80 100 200 400 1000}"
PARALLEL_BUILD="${PARALLEL_BUILD:-0}"               # max_parallel_maintenance_workers

export PATH="$PG_BIN:$PATH"
export LD_LIBRARY_PATH="$PG_LIB:${LD_LIBRARY_PATH:-}"

log()  { echo "[bench] $*"; }
die()  { echo "[bench][FATAL] $*" >&2; exit 1; }
