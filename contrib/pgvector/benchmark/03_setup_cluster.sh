#!/bin/bash
# Single-node PostgreSQL setup: initdb + start/stop/status.
#
# Usage:
#   ./03_setup_cluster.sh          # initdb (if needed) & start
#   ./03_setup_cluster.sh stop     # stop postgres
#   ./03_setup_cluster.sh status
#   FORCE=1 ./03_setup_cluster.sh  # wipe $PGDATA and re-init
set -euo pipefail
source "$(dirname "$0")/env.sh"

MODE="${1:-start}"

stop_all() {
    if [ -f "$PGDATA/postmaster.pid" ]; then
        log "stopping postgres"
        pg_ctl -D "$PGDATA" stop -m fast
    else
        log "not running"
    fi
}

case "$MODE" in
    stop)   stop_all; exit 0 ;;
    status) pg_ctl -D "$PGDATA" status 2>&1 || true; exit 0 ;;
    start)  ;;
    *)      die "usage: $0 [start|stop|status]" ;;
esac

# --- sanity ---
[ -x "$PG_BIN/postgres" ] || die "postgres not built at $OTB_BUILD — run ./configure + make first"
[ -x "$PG_BIN/initdb" ]   || die "initdb missing"
[ -f "$OTB_BUILD/share/postgresql/extension/vector.control" ] || die "pgvector not installed (make PG_CONFIG=$PG_BIN/pg_config install)"

if [ -f "$PGDATA/PG_VERSION" ]; then
    log "data dir already initialized; starting"
    pg_ctl -D "$PGDATA" -l "$BENCH_HOME/pg.log" start
else
    if [ -e "$PGDATA" ]; then
        if [ "${FORCE:-0}" = "1" ]; then
            log "FORCE=1: wiping $PGDATA"
            rm -rf "$PGDATA"
        else
            die "$PGDATA exists but is not a data dir (use FORCE=1)"
        fi
    fi
    log "initdb single-node PostgreSQL at $PGDATA (port $PGPORT)"
    initdb -D "$PGDATA" -U "$PGUSER" -E UTF8 --locale=C -A trust

    log "applying tuning..."
    cat >> "$PGDATA/postgresql.conf" <<EOF
shared_buffers = 2GB
maintenance_work_mem = 1GB
effective_cache_size = 6GB
max_wal_size = 4GB
checkpoint_timeout = 30min
synchronous_commit = off
EOF

    log "starting..."
    pg_ctl -D "$PGDATA" -l "$BENCH_HOME/pg.log" start
fi

sleep 1
log "ready. quick check:"
psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d postgres -c "SELECT version();"
