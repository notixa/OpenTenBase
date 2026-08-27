#!/bin/bash
set -euo pipefail

export PATH="/home/notixa/codes/opensource-project/OpenTenBase/otb19_build/bin:$PATH"
export STAT_DURATION=5
BENCH_HOME="${BENCH_HOME:-$HOME/otb_bench}"
PGDATA="$BENCH_HOME/pgdata"

echo "Restarting PostgreSQL..."
pg_ctl -D "$PGDATA" restart

for d in l2 ip cos; do
    echo "========================================================="
    echo "Running AoSoA Benchmark for $d..."
    echo "========================================================="
    cd "benchmark/$d"
    ./run_aosoa.sh
    cd ../..
done

echo "ALL AOSOA BENCHMARKS COMPLETED!"
