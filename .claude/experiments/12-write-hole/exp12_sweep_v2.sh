#!/bin/bash
set -eu
cd ~/reffs
echo "kill_ms,outcome,read_ok,diff_X"
for kms in 50 150 300 450 600 800; do
  out=$(sudo docker compose -f deploy/benchmark/docker-compose.yml \
      --profile run run --rm --entrypoint /bin/bash \
      -v /tmp/exp12_workload_v2.sh:/tmp/wh.sh:ro,z \
      bench /tmp/wh.sh "$kms" 2>&1)
  line=$(echo "$out" | grep "^RESULT_V2" | tail -1)
  outcome=$(echo "$line" | sed -n 's/.*outcome=\([^ ]*\).*/\1/p')
  ok=$(echo "$line" | sed -n 's/.*read_ok=\([0-9]*\).*/\1/p')
  dx=$(echo "$outcome" | sed -n 's/.*diff_X=\([0-9]*\).*/\1/p')
  short=$(echo "$outcome" | sed 's/(.*//')
  echo "$kms,$short,$ok,${dx:-}"
done
