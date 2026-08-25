#!/bin/bash
# Build (or rebuild) the IVFFlat index DIRECTLY on the DN. Usage: ./05_build_index.sh [LISTS]
#
# MUST run on the DN directly: building via CN dispatch breaks pgvector's block
# sampling (RelationGetNumberOfBlocks misbehaves in the remote transaction) ->
# "ivfflat index created with little data" -> recall 0%. A direct DN build
# samples correctly. Requires allow_dml_on_datanode=on to permit the write.
# Keeps only one ivfflat index on sift_base at a time.
set -euo pipefail
source "$(dirname "$0")/env.sh"

LISTS="${1:-$LISTS}"
IDX="idx_sift_ivfflat"
PSQL_DN="psql -h $PGHOST -p $DN_PORT -U $PGUSER"

OLD=$($PSQL_DN -d "$PGDATABASE" -tA -c \
    "SELECT indexname FROM pg_indexes WHERE tablename='sift_base' AND indexdef ILIKE '%ivfflat%'" || true)
if [ -n "$OLD" ]; then
    log "dropping existing ivfflat index(es): $OLD"
    $PSQL_DN -d "$PGDATABASE" -c "SET allow_dml_on_datanode=on; DROP INDEX $OLD;"
fi

log "building ivfflat index directly on DN, lists=$LISTS (serial; a few minutes)..."
$PSQL_DN -d "$PGDATABASE" -v ON_ERROR_STOP=1 <<SQL
SET allow_dml_on_datanode = on;
SET maintenance_work_mem = '1GB';
\timing on
CREATE INDEX $IDX ON sift_base USING ivfflat (v vector_l2_ops) WITH (lists = $LISTS);
SQL

log "index size: $($PSQL_DN -d "$PGDATABASE" -tA -c "SELECT pg_size_pretty(pg_relation_size('$IDX'));")"
