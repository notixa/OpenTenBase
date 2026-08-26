#!/bin/bash
# Self-contained pipeline: load -> build index -> run sweep -> report -> plot.
# Sourced by run.sh AFTER env.sh. Uses env vars: TABLE OPCLASS OP CSV_DIR RESDIR.
set -euo pipefail

PSQL="psql -h $PGHOST -p $PGPORT -U $PGUSER"

# ---------- 1. load ----------
[ -f "$CSV_DIR/dim.txt" ] || die "$CSV_DIR/dim.txt missing — run convert.py first"
DIM=$(cat "$CSV_DIR/dim.txt")
[ -f "$CSV_DIR/base.csv" ] || die "$CSV_DIR/base.csv missing"

$PSQL -d postgres -tc "SELECT 1 FROM pg_database WHERE datname='$PGDATABASE'" | grep -q 1 || \
    createdb -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" "$PGDATABASE"

$PSQL -d "$PGDATABASE" -v ON_ERROR_STOP=1 <<SQL
CREATE EXTENSION IF NOT EXISTS vector;
DROP TABLE IF EXISTS $TABLE;
CREATE TABLE $TABLE (id int PRIMARY KEY, v vector($DIM));
SQL

log "loading into $TABLE ..."
time $PSQL -d "$PGDATABASE" -v ON_ERROR_STOP=1 -c \
    "\copy $TABLE FROM '$CSV_DIR/base.csv' WITH (FORMAT text, DELIMITER E'\t')"
$PSQL -d "$PGDATABASE" -c "ANALYZE $TABLE;"
log "rows: $($PSQL -d "$PGDATABASE" -tA -c "SELECT count(*) FROM $TABLE;")"

# ---------- 2. build index ----------
IDX="idx_${TABLE}"
OLD=$($PSQL -d "$PGDATABASE" -tA -c \
    "SELECT indexname FROM pg_indexes WHERE tablename='$TABLE' AND indexdef ILIKE '%ivfflat%'" || true)
[ -n "$OLD" ] && { log "dropping $OLD"; $PSQL -d "$PGDATABASE" -c "DROP INDEX $OLD;"; }

log "building ivfflat index ($OPCLASS), lists=$LISTS ..."
$PSQL -d "$PGDATABASE" -v ON_ERROR_STOP=1 <<SQL
SET maintenance_work_mem = '1GB';
\timing on
CREATE INDEX $IDX ON $TABLE USING ivfflat (v $OPCLASS) WITH (lists = $LISTS);
SQL

# ---------- 3. run sweep ----------
head -n "$NQUERIES" "$CSV_DIR/queries.csv" | awk -F'\t' -v k="$TOPK" -v op="$OP" \
    '{printf "SELECT id FROM '"$TABLE"' ORDER BY v %s \047%s\047 LIMIT %d;\n", op, $2, k}' \
    > "$RESDIR/gen.queries.sql"

run_sql() { # $1=sqlfile $2=outfile
    $PSQL -d "$PGDATABASE" -A -t -v ON_ERROR_STOP=1 -f "$1" > /dev/null 2>&1 || true   # warmup
    $PSQL -d "$PGDATABASE" -A -t -v ON_ERROR_STOP=1 -f "$1" > "$2"
}

log "running exact seq-scan reference ..."
{
    echo '\timing on'
    echo 'SET enable_indexscan = off;'
    echo 'SET enable_bitmapscan = off;'
    cat "$RESDIR/gen.queries.sql"
} > "$RESDIR/gen.seq.sql"
run_sql "$RESDIR/gen.seq.sql" "$RESDIR/raw_seq.txt"

for P in $PROBES_SWEEP; do
    [ "$P" -gt "$LISTS" ] && { log "skip probes=$P > lists=$LISTS"; continue; }
    log "running ivfflat probes=$P ..."
    {
        echo '\timing on'
        echo 'SET enable_seqscan = off;'
        echo "SET ivfflat.probes = $P;"; [ -n "${IVFFLAT_TOP_K:-}" ] && echo "SET ivfflat.top_k = $IVFFLAT_TOP_K;"
        cat "$RESDIR/gen.queries.sql"
    } > "$RESDIR/gen.ivf.sql"
    run_sql "$RESDIR/gen.ivf.sql" "$RESDIR/raw_l${LISTS}_p${P}.txt"
done

# ---------- 4. report + plot ----------
log "generating report..."
"$BENCH_DIR/report.py" \
    --truth "$CSV_DIR/truth.ivecs" \
    --results-dir "$RESDIR" --lists "$LISTS" --nq "$NQUERIES" --topk "$TOPK"

python3 "$BENCH_DIR/plot.py" "$RESDIR/summary.csv" --out "$RESDIR/baseline_curve.png"
log "done. results in $RESDIR/summary.csv"
