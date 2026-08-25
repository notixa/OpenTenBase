#!/bin/bash
# L2 (SIFT1M) one-shot full run: setup -> download -> convert -> load -> index -> baseline.
#   ./run.sh              # full pipeline
#   NQUERIES=100 ./run.sh # use 100 queries
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

log "=== $SCENARIO scenario full run ==="
bash "$(dirname "${BASH_SOURCE[0]}")/setup.sh"
bash "$(dirname "${BASH_SOURCE[0]}")/download.sh"
python3 "$(dirname "${BASH_SOURCE[0]}")/convert.py" "$DATASET_SRC" "$CSV_DIR" "$NQUERIES"
source "$(dirname "${BASH_SOURCE[0]}")/pipeline.sh"
