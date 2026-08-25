#!/bin/bash
# Build (or rebuild) the IVFFlat index. Usage: ./05_build_index.sh [LISTS]
# Keeps only one ivfflat index on sift_base at a time.
set -euo pipefail
source "$(dirname "$0")/env.sh"

LISTS="${1:-$LISTS}"
IDX="idx_sift_ivfflat"
PSQL="psql -h $PGHOST -p $PGPORT -U $PGUSER"

OLD=$($PSQL -d "$PGDATABASE" -tA -c \
    "SELECT indexname FROM pg_indexes WHERE tablename='sift_base' AND indexdef ILIKE '%ivfflat%'" || true)
if [ -n "$OLD" ]; then
    log "dropping existing ivfflat index(es): $OLD"
    $PSQL -d "$PGDATABASE" -c "DROP INDEX $OLD;"
fi

log "building ivfflat index, lists=$LISTS (a few minutes)..."
$PSQL -d "$PGDATABASE" -v ON_ERROR_STOP=1 <<SQL
SET maintenance_work_mem = '1GB';
\timing on
CREATE INDEX $IDX ON sift_base USING ivfflat (v vector_l2_ops) WITH (lists = $LISTS);
SQL

log "index size: $($PSQL -d "$PGDATABASE" -tA -c "SELECT pg_size_pretty(pg_relation_size('$IDX'));")"
