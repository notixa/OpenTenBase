#!/bin/bash
# Single-node PostgreSQL initdb + start (idempotent). Usage: ./setup.sh [start|stop|status]
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
MODE="${1:-start}"

case "$MODE" in
    stop)   if [ -f "$PGDATA/postmaster.pid" ]; then log "stopping"; pg_ctl -D "$PGDATA" stop -m fast; else log "not running"; fi; exit 0 ;;
    status) pg_ctl -D "$PGDATA" status 2>&1 || true; exit 0 ;;
    start)  ;;
    *)      die "usage: $0 [start|stop|status]" ;;
esac

[ -x "$PG_BIN/postgres" ] || die "postgres not built at $OTB_BUILD"
[ -x "$PG_BIN/initdb" ]   || die "initdb missing"

if [ -f "$PGDATA/PG_VERSION" ]; then
    pg_ctl -D "$PGDATA" status >/dev/null 2>&1 || pg_ctl -D "$PGDATA" -l "$BENCH_HOME/pg.log" start
else
    if [ -e "$PGDATA" ]; then
        [ "${FORCE:-0}" = "1" ] && rm -rf "$PGDATA" || die "$PGDATA exists but is not a data dir (use FORCE=1)"
    fi
    log "initdb at $PGDATA"
    initdb -D "$PGDATA" -U "$PGUSER" -E UTF8 --locale=C -A trust
    cat >> "$PGDATA/postgresql.conf" <<EOF
shared_buffers = 2GB
maintenance_work_mem = 1GB
effective_cache_size = 6GB
max_wal_size = 4GB
checkpoint_timeout = 30min
synchronous_commit = off
EOF
    pg_ctl -D "$PGDATA" -l "$BENCH_HOME/pg.log" start
fi
sleep 1
pg_ctl -D "$PGDATA" status >/dev/null 2>&1 || die "postgres failed to start (see $BENCH_HOME/pg.log)"
log "postgres ready on $PGPORT"
