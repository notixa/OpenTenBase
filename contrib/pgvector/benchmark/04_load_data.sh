#!/bin/bash
# Create DB + extension + table, load SIFT1M base vectors via \copy, ANALYZE.
# Single-node PostgreSQL: plain local table (no distribution clause).
set -euo pipefail
source "$(dirname "$0")/env.sh"

CSV_BASE="$SIFT_DIR/csv/base.csv"
[ -f "$CSV_BASE" ] || die "$CSV_BASE missing — run 01_download_sift1m.sh and 02_convert_sift1m.py first"

PSQL="psql -h $PGHOST -p $PGPORT -U $PGUSER"
log "loading into single-node PostgreSQL ($PGPORT)"

# create database if missing
$PSQL -d postgres -tc "SELECT 1 FROM pg_database WHERE datname='$PGDATABASE'" | grep -q 1 || \
    createdb -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" "$PGDATABASE"
log "database ready: $PGDATABASE"

$PSQL -d "$PGDATABASE" -v ON_ERROR_STOP=1 <<SQL
CREATE EXTENSION IF NOT EXISTS vector;
DROP TABLE IF EXISTS sift_base;
CREATE TABLE sift_base (id int PRIMARY KEY, v vector(128));
SQL

log "loading 1,000,000 vectors..."
time $PSQL -d "$PGDATABASE" -v ON_ERROR_STOP=1 -c \
    "\copy sift_base FROM '$CSV_BASE' WITH (FORMAT text, DELIMITER E'\t')"

log "ANALYZE..."
$PSQL -d "$PGDATABASE" -c "ANALYZE sift_base;"
log "rows: $($PSQL -d "$PGDATABASE" -tA -c 'SELECT count(*) FROM sift_base;')"
log "load done"
