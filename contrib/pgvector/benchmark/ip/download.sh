#!/bin/bash
# Download the IP (inner product) dataset: lastfm-64-dot (ann-benchmarks HDF5).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

if [ -f "$DATASET_FILE" ]; then
    log "dataset already present: $DATASET_FILE"
    exit 0
fi

log "downloading $DATASET_URL ..."
if command -v wget >/dev/null; then
    wget -c "$DATASET_URL" -O "$DATASET_FILE"
else
    curl -fL "$DATASET_URL" -o "$DATASET_FILE"
fi
log "downloaded: $(stat -c%s "$DATASET_FILE") bytes"
