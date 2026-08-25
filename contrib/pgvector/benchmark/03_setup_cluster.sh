#!/bin/bash
# Deploy a single-machine (centralized) OpenTenBase instance: GTM + 1 CN + 1 DN.
#
# Usage:
#   ./03_setup_cluster.sh          # deploy & start (refuses to overwrite existing dirs)
#   ./03_setup_cluster.sh stop     # stop all nodes
#   ./03_setup_cluster.sh status
#   FORCE=1 ./03_setup_cluster.sh  # wipe $OTB_DATA and redeploy from scratch
set -euo pipefail
source "$(dirname "$0")/env.sh"

MODE="${1:-start}"

GTM_DATA="$OTB_DATA/gtm"
CN_DATA="$OTB_DATA/cn"
DN_DATA="$OTB_DATA/dn"

stop_all() {
    for spec in "coordinator $CN_DATA" "datanode $DN_DATA"; do
        set -- $spec
        [ -d "$2" ] && { log "stopping $1 ($2)"; pg_ctl -D "$2" -Z "$1" stop -m fast -t 20 2>/dev/null || true; }
    done
    [ -d "$GTM_DATA" ] && { log "stopping gtm"; gtm_ctl stop -Z gtm -D "$GTM_DATA" 2>/dev/null || true; }
    # fallback: kill stragglers referencing our data dirs (safe: patterns only match this benchmark)
    pkill -f "postgres .*$CN_DATA" 2>/dev/null || true
    pkill -f "postgres .*$DN_DATA" 2>/dev/null || true
    pkill -f "gtm -D $GTM_DATA" 2>/dev/null || true
    sleep 1
    log "all stopped"
}

status_all() {
    pg_ctl -D "$CN_DATA" -Z coordinator status 2>/dev/null || log "cn: not running"
    pg_ctl -D "$DN_DATA" -Z datanode status 2>/dev/null || log "dn: not running"
    gtm_ctl -Z gtm -D "$GTM_DATA" status 2>/dev/null || log "gtm: not running"
}

case "$MODE" in
    stop)   stop_all; exit 0 ;;
    status) status_all; exit 0 ;;
    start)  ;;
    *)      die "usage: $0 [start|stop|status]" ;;
esac

# --- sanity ---
[ -x "$PG_BIN/postgres" ]     || die "OpenTenBase not built at $OTB_BUILD — run 'make && make install' at $OTB_ROOT first (see README)"
[ -x "$PG_BIN/initgtm" ]      || die "initgtm missing"
[ -f "$OTB_BUILD/share/postgresql/extension/vector.control" ] || die "pgvector not installed — run 'make install' in $OTB_ROOT/contrib/pgvector (see README)"

if [ -d "$CN_DATA" ] || [ -d "$DN_DATA" ] || [ -d "$GTM_DATA" ]; then
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1: wiping $OTB_DATA"
        stop_all >/dev/null 2>&1 || true
        rm -rf "$GTM_DATA" "$CN_DATA" "$DN_DATA"
    else
        die "cluster dirs already exist under $OTB_DATA (use FORCE=1 ./03_setup_cluster.sh to redeploy)"
    fi
fi

# --- GTM ---
log "initializing GTM (port $GTM_PORT)"
initgtm -D "$GTM_DATA" -Z gtm
cat >> "$GTM_DATA/gtm.conf" <<EOF
port = $GTM_PORT
nodename = 'gtm'
listen_addresses = '127.0.0.1'
EOF
gtm_ctl start -Z gtm -D "$GTM_DATA"
sleep 1

common_conf() { # $1=port $2=pooler_port $3=forward_port
    # NOTE: this OpenTenBase version has NO gtm_host/gtm_port/remote_read_mode GUCs —
    # GTM location is registered in the pgxc_node catalog by initdb --master_gtm_* options.
    # forward_port (default 6669) is used by the forward manager on EVERY node → must differ per node.
    cat <<EOF
listen_addresses = '127.0.0.1'
port = $1
pooler_port = $2
forward_port = $3
max_connections = 200
shared_buffers = 2GB
maintenance_work_mem = 1GB
effective_cache_size = 6GB
max_wal_size = 4GB
checkpoint_timeout = 30min
synchronous_commit = off
EOF
}

# --- Datanode ---
log "initializing DN (port $DN_PORT)"
initdb -D "$DN_DATA" -U "$PGUSER" -E UTF8 --locale=C -A trust \
    --nodename=dn1 --nodetype=datanode \
    --master_gtm_nodename=gtm --master_gtm_ip=127.0.0.1 --master_gtm_port=$GTM_PORT
common_conf "$DN_PORT" "$DN_POOLER_PORT" "$DN_FORWARD_PORT" >> "$DN_DATA/postgresql.conf"
pg_ctl start -D "$DN_DATA" -Z datanode -l "$OTB_DATA/dn.log"
sleep 1

# --- Coordinator ---
log "initializing CN (port $CN_PORT)"
initdb -D "$CN_DATA" -U "$PGUSER" -E UTF8 --locale=C -A trust \
    --nodename=cn --nodetype=coordinator \
    --master_gtm_nodename=gtm --master_gtm_ip=127.0.0.1 --master_gtm_port=$GTM_PORT
common_conf "$CN_PORT" "$CN_POOLER_PORT" "$CN_FORWARD_PORT" >> "$CN_DATA/postgresql.conf"
pg_ctl start -D "$CN_DATA" -Z coordinator -l "$OTB_DATA/cn.log"
sleep 1

# --- cluster topology ---
# Bootstrap notes (verified against this OpenTenBase version):
#  * initdb --nodename=<n> auto-creates that node's SELF entry in pgxc_node (with a
#    default port), so use ALTER NODE to fix its address instead of CREATE NODE.
#  * CREATE NODE on the CN does NOT auto-propagate to the DN during bootstrap —
#    register the topology on BOTH nodes explicitly, then pgxc_pool_reload() on each
#    (this refreshes the shmem node table, otherwise CREATE DEFAULT NODE GROUP
#    crashes with "node oid ... could not get nodeid").
#  * The node group must be CREATE **DEFAULT** NODE GROUP, otherwise CREATE TABLE
#    ... DISTRIBUTE BY fails with "default group not defined".
#  * pgxc_node PORT = the node's MAIN port (pooler_port/forward_port are separate).

wait_ready() { # $1=port $2=label
    log "waiting for $2 (:${1}) ..."
    local i=0
    until psql -h 127.0.0.1 -p "$1" -U "$PGUSER" -d postgres -c "SELECT 1" >/dev/null 2>&1; do
        i=$((i+1)); [ $i -gt 30 ] && die "$2 not ready after 30s"
        sleep 1
    done
}

wait_ready "$CN_PORT" "CN"
wait_ready "$DN_PORT" "DN"

log "registering topology on CN"
# NOTE: ALTER NODE accepts FORWARD= (forward_port) even though older gram.y
# comments omit it. Without the forward port, DN->CN "forward" connections fail
# ("conn is not inited ... forward port 0") and distributed DML aborts.
psql -h 127.0.0.1 -p "$CN_PORT" -U "$PGUSER" -d postgres -v ON_ERROR_STOP=1 <<SQL
ALTER NODE cn WITH (HOST='127.0.0.1', PORT=$CN_PORT, FORWARD=$CN_FORWARD_PORT);
CREATE NODE dn1 WITH (TYPE='datanode', HOST='127.0.0.1', PORT=$DN_PORT, FORWARD=$DN_FORWARD_PORT, PRIMARY, PREFERRED);
SQL
psql -h 127.0.0.1 -p "$CN_PORT" -U "$PGUSER" -d postgres -c "SELECT pgxc_pool_reload();" >/dev/null

log "registering topology on DN"
# CRITICAL: the DN's SELF node (dn1) must be marked PRIMARY, otherwise the DN
# considers itself a standby and runs read-only for direct connections.
psql -h 127.0.0.1 -p "$DN_PORT" -U "$PGUSER" -d postgres -v ON_ERROR_STOP=1 <<SQL
ALTER NODE dn1 WITH (HOST='127.0.0.1', PORT=$DN_PORT, FORWARD=$DN_FORWARD_PORT, PRIMARY);
CREATE NODE cn WITH (TYPE='coordinator', HOST='127.0.0.1', PORT=$CN_PORT, FORWARD=$CN_FORWARD_PORT);
SQL
psql -h 127.0.0.1 -p "$DN_PORT" -U "$PGUSER" -d postgres -c "SELECT pgxc_pool_reload();" >/dev/null

log "creating default node group"
psql -h 127.0.0.1 -p "$CN_PORT" -U "$PGUSER" -d postgres -v ON_ERROR_STOP=1 \
    -c "CREATE DEFAULT NODE GROUP group1 WITH (dn1);"

log "cluster up: GTM=$GTM_PORT  CN=$CN_PORT  DN=$DN_PORT"
log "quick check:"
psql -h 127.0.0.1 -p "$CN_PORT" -U "$PGUSER" -d postgres -c "SELECT node_name, node_type, node_port FROM pgxc_node ORDER BY node_name;"
log "done. Benchmark target: CN at $CN_PORT (DNs are read-only for direct connections by design)"
