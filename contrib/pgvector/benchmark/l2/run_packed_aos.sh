#!/bin/bash
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

export IVFFLAT_TOP_K=$TOPK
export RESDIR="$BENCH_DIR/results_packed_aos"
mkdir -p "$RESDIR"

log "=== $SCENARIO scenario Packed AoS run (TOPK=$IVFFLAT_TOP_K) ==="
bash "$(dirname "${BASH_SOURCE[0]}")/setup.sh"
bash "$(dirname "${BASH_SOURCE[0]}")/download.sh"
[ -f "$CSV_DIR/base.csv" ] || python3 "$(dirname "${BASH_SOURCE[0]}")/convert.py" "$DATASET_SRC" "$CSV_DIR" "$NQUERIES"
source "$(dirname "${BASH_SOURCE[0]}")/pipeline.sh"
bash "$(dirname "${BASH_SOURCE[0]}")/stat_sweep.sh" "${STAT_DURATION:-5}"
