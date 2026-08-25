#!/bin/bash
# Run the baseline sweep: exact seq-scan reference + ivfflat probes sweep.
# Latency measured client-side by psql \timing over $NQUERIES queries (single session),
# each query: SELECT id FROM sift_base ORDER BY v <-> '<q>' LIMIT $TOPK;
# A warmup pass (identical SQL) is run first and discarded.
set -euo pipefail
source "$(dirname "$0")/env.sh"

LISTS="${1:-$LISTS}"
QUERIES_CSV="$SIFT_DIR/csv/queries.csv"
[ -f "$QUERIES_CSV" ] || die "$QUERIES_CSV missing — run 01+02 first"
head -n "$NQUERIES" "$QUERIES_CSV" | awk -F'\t' -v k="$TOPK" \
    '{printf "SELECT id FROM sift_base ORDER BY v <-> \047%s\047 LIMIT %d;\n", $2, k}' \
    > "$BENCH_DIR/results/gen.queries.sql"

GEN="$BENCH_DIR/results"
run_sql() { # $1=sqlfile $2=outfile
    psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -A -t -v ON_ERROR_STOP=1 \
        -f "$1" > /dev/null 2>&1 || true   # warmup, ignore
    psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -A -t -v ON_ERROR_STOP=1 \
        -f "$1" > "$2"
}

# ---------- exact reference: force seq scan (recall = 1.0 by definition) ----------
log "running exact seq-scan reference ($NQUERIES queries, slowest pass)..."
{
    echo '\timing on'
    echo 'SET enable_indexscan = off;'
    echo 'SET enable_bitmapscan = off;'
    cat "$GEN/gen.queries.sql"
} > "$GEN/gen.seq.sql"
run_sql "$GEN/gen.seq.sql" "$GEN/raw_seq.txt"

# ---------- ivfflat probes sweep ----------
for P in $PROBES_SWEEP; do
    if [ "$P" -gt "$LISTS" ]; then
        log "skip probes=$P > lists=$LISTS"
        continue
    fi
    log "running ivfflat probes=$P ..."
    {
        echo '\timing on'
        echo 'SET enable_seqscan = off;'
        echo "SET ivfflat.probes = $P;"
        cat "$GEN/gen.queries.sql"
    } > "$GEN/gen.ivf.sql"
    run_sql "$GEN/gen.ivf.sql" "$GEN/raw_l${LISTS}_p${P}.txt"
done

log "all runs done — generating report"
"$(dirname "$0")/07_report.py" \
    --truth "$SIFT_DIR/sift/sift_groundtruth.ivecs" \
    --results-dir "$GEN" --lists "$LISTS" --nq "$NQUERIES" --topk "$TOPK"
