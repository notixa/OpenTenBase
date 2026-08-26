#!/bin/bash
set -euo pipefail

export STAT_DURATION=5

for d in l2 ip cos; do
    echo "=================================="
    echo "Running baseline for $d..."
    echo "=================================="
    cd benchmark/$d
    ./run.sh > run.log 2>&1
    cd ../..

    echo "=================================="
    echo "Running optimized for $d..."
    echo "=================================="
    cd benchmark/$d
    ./run_opt.sh > run_opt.log 2>&1
    cd ../..
done
echo "ALL FINISHED"
