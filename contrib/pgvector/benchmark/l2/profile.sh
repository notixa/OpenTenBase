#!/bin/bash
# Profile the pgvector IVFFlat query path with perf (scenario-parameterized).
#
# Modes:
#   record (default): perf record -> flame graph + top-symbols report
#   --stat           : perf stat for IPC / cache-miss / branch-miss counters
#
# Uses the scenario's TABLE/OP from env.sh (e.g. sift_base + <->, or lastfm_base + <#>).
# FlameGraph scripts live in ~/codes/opensource-project/FlameGraph.
#
# Usage:
#   ./profile.sh [PROBES] [DURATION_SECONDS]
#   ./profile.sh --stat [PROBES] [DURATION_SECONDS]
#   ./profile.sh 10 30            # record: probes=10, 30s
#   ./profile.sh --stat 10 20     # stat: probes=10, 20s
#
# Env overrides: PROBES DURATION QUERY_IDX FREQ REPEATS FGDIR OUTFILE STAT_EVENTS
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

MODE="record"
if [ "${1:-}" = "--stat" ]; then
    MODE="stat"
    shift
fi

PROBES="${1:-${PROBES:-10}}"
DURATION="${2:-${DURATION:-30}}"
QUERY_IDX="${QUERY_IDX:-1}"
FREQ="${FREQ:-999}"
REPEATS="${REPEATS:-10000}"
FGDIR="${FGDIR:-$HOME/codes/opensource-project/FlameGraph}"
STAT_EVENTS="${STAT_EVENTS:-cycles:u,instructions:u,cache-misses:u,cache-references:u,branch-misses:u,branches:u,L1-dcache-loads:u,L1-dcache-load-misses:u}"

QUERIES_CSV="$CSV_DIR/queries.csv"
[ -f "$QUERIES_CSV" ] || die "$QUERIES_CSV missing — run convert.py first"
command -v perf >/dev/null || die "perf not installed"
[ "$MODE" = "stat" ] || [ -x "$FGDIR/stackcollapse-perf.pl" ] || die "FlameGraph scripts not found under $FGDIR"

OUTDIR="$RESDIR/profile"
mkdir -p "$OUTDIR"
OUTFILE="${OUTFILE:-$OUTDIR/probes${PROBES}_${DURATION}s}"

# ---- 1. build the workload: pid + fixed query, then \watch to loop until killed ----
Q=$(sed -n "${QUERY_IDX}p" "$QUERIES_CSV" | cut -f2)
[ -n "$Q" ] || die "no query at index $QUERY_IDX"

log "building workload (mode=$MODE, table=$TABLE op=$OP, probes=$PROBES, query #$QUERY_IDX, \\watch loop)"
{
    echo "SELECT pg_backend_pid();"
    echo "SET enable_seqscan = off;"
    echo "SET ivfflat.probes = $PROBES;"; [ -n "${IVFFLAT_TOP_K:-}" ] && echo "SET ivfflat.top_k = $IVFFLAT_TOP_K;"
    echo "SELECT id FROM $TABLE ORDER BY v $OP '$Q' LIMIT $TOPK;"
    echo '\watch 0.001'
} > "$OUTDIR/workload.sql"

# ---- 2. launch workload in background, capture backend pid ----
log "launching workload on port $PGPORT..."
psql -h "$PGHOST" -p "$PGPORT" -U "$PGUSER" -d "$PGDATABASE" -A -t \
    -f "$OUTDIR/workload.sql" > "$OUTDIR/workload.out" 2>"$OUTDIR/workload.err" &
WPID=$!

PID=""
for i in $(seq 1 50); do
    PID=$(head -n1 "$OUTDIR/workload.out" 2>/dev/null | tr -d '[:space:]')
    case "$PID" in ''|0|*[!0-9]*) PID="";; esac
    [ -n "$PID" ] && break
    sleep 0.2
done
[ -n "$PID" ] || { kill "$WPID" 2>/dev/null || true; die "could not capture backend pid (see $OUTDIR/workload.err)"; }
log "backend pid = $PID"

# ---- 3. perf ----
if [ "$MODE" = "stat" ]; then
    log "perf stat for ${DURATION}s..."
    perf stat -p "$PID" -e "$STAT_EVENTS" -- sleep "$DURATION" 2>&1 \
        | tee "$OUTFILE.stat.txt"

    # Hybrid CPU splits counters into cpu_core/ and cpu_atom/ groups — aggregate
    # them and derive the ratios perf can't auto-compute across two PMUs.
    echo ""
    echo "=== aggregate ratios (cpu_core + cpu_atom) ==="
    awk '
        function num(s) { gsub(/,/, "", s); return s + 0 }
        /cycles\/u/                 { cyc += num($1) }
        /instructions\/u/           { ins += num($1) }
        /cache-misses\/u/           { cm  += num($1) }
        /cache-references\/u/       { cr  += num($1) }
        /branch-misses\/u/          { bm  += num($1) }
        /branches\/u/               { br  += num($1) }
        /L1-dcache-load-misses\/u/  { l1m += num($1) }
        /L1-dcache-loads\/u/        { l1l += num($1) }
        END {
            if (cyc > 0) printf "  IPC (insn/cycle)    = %.3f\n", ins / cyc
            if (cr  > 0) printf "  LLC cache miss rate = %.2f%%\n", 100 * cm / cr
            if (br  > 0) printf "  branch miss rate    = %.2f%%\n", 100 * bm / br
            if (l1l > 0) printf "  L1 dcache miss rate = %.2f%%\n", 100 * l1m / l1l
        }
    ' "$OUTFILE.stat.txt"
else
    log "perf record for ${DURATION}s (freq $FREQ Hz)..."
    perf record -F "$FREQ" -g --call-graph dwarf -p "$PID" \
        -o "$OUTFILE.data" -- sleep "$DURATION"
fi

# ---- 4. stop workload ----
kill "$WPID" 2>/dev/null || true
wait "$WPID" 2>/dev/null || true

# ---- 5. post-process (record mode only) ----
if [ "$MODE" != "stat" ]; then
    log "rendering flame graph..."
    perf script -i "$OUTFILE.data" > "$OUTFILE.perf"
    "$FGDIR/stackcollapse-perf.pl" "$OUTFILE.perf" > "$OUTFILE.folded"
    "$FGDIR/flamegraph.pl" --title "pgvector ivfflat $SCENARIO probes=$PROBES (${DURATION}s)" \
        "$OUTFILE.folded" > "$OUTFILE.svg"

    log "generating symbol report..."
    perf report -i "$OUTFILE.data" --stdio --sort=symbol --percent-limit=0.5 \
        > "$OUTFILE.report.txt"

    log "done. artifacts:"
    log "  flame graph : $OUTFILE.svg"
    log "  symbol top  : $OUTFILE.report.txt"
    log "  raw perf    : $OUTFILE.data"

    echo ""
    echo "=== top hot symbols ==="
    awk '/^#/{next} /^$/{next} {print}' "$OUTFILE.report.txt" | head -25 || true
else
    log "done. stat output: $OUTFILE.stat.txt"
fi
