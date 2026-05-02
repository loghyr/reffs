#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
DUR=30
cd ~/reffs
echo "N,variant,total_files,errors,duration_s,aggregate_MBps"
for variant in plain rs; do
  for N in 1 2 4 8; do
    out=$(sudo docker compose -f deploy/benchmark/docker-compose.yml \
        --profile run run --rm \
        --entrypoint /bin/bash \
        -v /tmp/exp04_workload.sh:/tmp/wl.sh:ro,z \
        bench /tmp/wl.sh "$N" "$variant" "$DUR" 2>&1)
    total=$(echo "$out" | awk '/^WORKER/ {gsub("files=","",$3); sum+=$3} END {print sum+0}')
    errs=$(echo "$out" | awk '/^WORKER/ {gsub("errors=","",$4); sum+=$4} END {print sum+0}')
    aggr=$(awk -v t=$total -v d=$DUR 'BEGIN {printf "%.2f", t*4/d}')
    echo "$N,$variant,$total,$errs,$DUR,$aggr"
  done
done
