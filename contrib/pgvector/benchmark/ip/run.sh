#!/bin/bash
# IP (Last.fm inner product) one-shot full run:
#   setup -> download -> convert -> load -> index -> baseline -> perf stat sweep.
#   ./run.sh              # full pipeline
#   NQUERIES=100 ./run.sh # use 100 queries
#   STAT_DURATION=20 ./run.sh   # perf stat sweep duration (default 10s)
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

log "=== $SCENARIO scenario full run ==="
bash "$(dirname "${BASH_SOURCE[0]}")/setup.sh"
bash "$(dirname "${BASH_SOURCE[0]}")/download.sh"
python3 "$(dirname "${BASH_SOURCE[0]}")/convert.py" "$DATASET_SRC" "$CSV_DIR" "$NQUERIES"
source "$(dirname "${BASH_SOURCE[0]}")/pipeline.sh"
bash "$(dirname "${BASH_SOURCE[0]}")/stat_sweep.sh" "${STAT_DURATION:-30}"
