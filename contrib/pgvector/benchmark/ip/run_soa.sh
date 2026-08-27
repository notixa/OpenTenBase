#!/bin/bash
# Optimized Top-K run with SoA Layout
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

export IVFFLAT_TOP_K=$TOPK
export RESDIR="$BENCH_DIR/results_soa"
mkdir -p "$RESDIR"

log "=== $SCENARIO scenario SoA run (TOPK=$IVFFLAT_TOP_K) ==="
bash "$(dirname "${BASH_SOURCE[0]}")/setup.sh"
bash "$(dirname "${BASH_SOURCE[0]}")/download.sh"
[ -f "$CSV_DIR/base.csv" ] || python3 "$(dirname "${BASH_SOURCE[0]}")/convert.py" "$DATASET_SRC" "$CSV_DIR" "$NQUERIES"
source "$(dirname "${BASH_SOURCE[0]}")/pipeline.sh"
bash "$(dirname "${BASH_SOURCE[0]}")/stat_sweep.sh" "${STAT_DURATION:-5}"
