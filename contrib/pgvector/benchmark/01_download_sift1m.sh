#!/bin/bash
# Download SIFT1M (1M x 128-d SIFT vectors + 10k queries + ground truth)
# Source: ANN search test corpus (texmex / IRISA), used by ann-benchmarks.
set -euo pipefail
source "$(dirname "$0")/env.sh"

TARBALL="$SIFT_DIR/sift.tar.gz"

cd "$SIFT_DIR"
if [ -f sift/sift_base.fvecs ] && [ -f sift/sift_query.fvecs ] && [ -f sift/sift_groundtruth.ivecs ]; then
    log "SIFT1M already present in $SIFT_DIR/sift"
    exit 0
fi

URLS=(
    "ftp://ftp.irisa.fr/local/texmex/corpus/sift.tar.gz"
    "http://corpus-texmex.irisa.fr/sift.tar.gz"
)

ok=0
for url in "${URLS[@]}"; do
    log "trying $url"
    if command -v wget >/dev/null; then
        wget -c --timeout=60 --tries=2 "$url" -O "$TARBALL" && ok=1 && break
    else
        curl -fL --connect-timeout 60 "$url" -o "$TARBALL" && ok=1 && break
    fi
done
[ "$ok" = "1" ] || die "download failed; open http://corpus-texmex.irisa.fr/ in a browser and download sift.tar.gz into $SIFT_DIR manually"

log "extracting..."
tar xzf "$TARBALL" -C "$SIFT_DIR"

# Verify expected contents (sizes in bytes)
check() { [ -f "$1" ] || die "missing $1"; }
check "$SIFT_DIR/sift/sift_base.fvecs"
check "$SIFT_DIR/sift/sift_query.fvecs"
check "$SIFT_DIR/sift/sift_groundtruth.ivecs"
# per-vector bytes = 4 (dim int32) + 128*4 (floats) = 516
# base 1,000,000 * 516 = 516000000 ; query 10,000 * 516 = 5160000 ; truth 10,000 * (4+100*4) = 4040000
act1=$(stat -c%s "$SIFT_DIR/sift/sift_base.fvecs")
act2=$(stat -c%s "$SIFT_DIR/sift/sift_query.fvecs")
act3=$(stat -c%s "$SIFT_DIR/sift/sift_groundtruth.ivecs")
[ "$act1" = "516000000" ] || die "sift_base.fvecs size $act1 != 516000000"
[ "$act2" = "5160000" ]  || die "sift_query.fvecs size $act2 != 5160000"
[ "$act3" = "4040000" ]  || die "sift_groundtruth.ivecs size $act3 != 4040000"
log "SIFT1M OK: base=1,000,000 x 128, query=10,000, truth=10,000 x top-100"
