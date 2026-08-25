#!/bin/bash
# Create DB + extension + table, load SIFT1M base vectors via \copy, ANALYZE.
# Loads through the CN (DNs are read-only for direct writes; COPY distributes to DNs).
set -euo pipefail
source "$(dirname "$0")/env.sh"

CSV_BASE="$SIFT_DIR/csv/base.csv"
[ -f "$CSV_BASE" ] || die "$CSV_BASE missing — run 01_download_sift1m.sh and 02_convert_sift1m.py first"

CN="psql -h $PGHOST -p $CN_PORT -U $PGUSER"
log "loading via CN ($CN_PORT); table uses DISTRIBUTE BY REPLICATION"

# create database if missing (via CN)
$CN -d postgres -tc "SELECT 1 FROM pg_database WHERE datname='$PGDATABASE'" | grep -q 1 || \
    createdb -h "$PGHOST" -p "$CN_PORT" -U "$PGUSER" "$PGDATABASE"
log "database ready: $PGDATABASE"

$CN -d "$PGDATABASE" -v ON_ERROR_STOP=1 <<SQL
CREATE EXTENSION IF NOT EXISTS vector;
DROP TABLE IF EXISTS sift_base;
CREATE TABLE sift_base (id int PRIMARY KEY, v vector(128)) DISTRIBUTE BY REPLICATION;
SQL

log "loading 1,000,000 vectors (this takes a few minutes)..."
time $CN -d "$PGDATABASE" -v ON_ERROR_STOP=1 -c \
    "\copy sift_base FROM '$CSV_BASE' WITH (FORMAT text, DELIMITER E'\t')"

log "ANALYZE..."
$CN -d "$PGDATABASE" -c "ANALYZE sift_base;"
log "rows: $($CN -d "$PGDATABASE" -tA -c 'SELECT count(*) FROM sift_base;')"
log "load done"
