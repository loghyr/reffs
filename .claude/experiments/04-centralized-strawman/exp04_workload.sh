#!/bin/bash
# SPDX-FileCopyrightText: 2026 Tom Haynes <loghyr@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
# Robust exp04 workload: workers tolerate individual ec_demo failures.
# Args: <N> <variant: plain|rs> <duration_sec>
N=$1
VARIANT=$2
DUR=$3
EC_DEMO=/shared/build/tools/ec_demo
MDS=reffs-mds
SIZE=$((4 * 1024 * 1024))

dd if=/dev/urandom of=/tmp/in bs=$SIZE count=1 2>/dev/null

worker() {
  local id=$1
  local count=0
  local errs=0
  local end=$(( $(date +%s) + DUR ))
  while [ $(date +%s) -lt $end ]; do
    if [ "$VARIANT" = "plain" ]; then
      "$EC_DEMO" put --mds "$MDS" --file "exp04_${VARIANT}_${id}_${count}" \
        --input /tmp/in 2>>/tmp/wl-$id.err
    else
      "$EC_DEMO" write --mds "$MDS" --file "exp04_${VARIANT}_${id}_${count}" \
        --input /tmp/in --k 4 --m 2 --codec rs --layout v1 \
        2>>/tmp/wl-$id.err
    fi
    if [ $? -eq 0 ]; then count=$((count+1)); else errs=$((errs+1)); fi
  done
  echo "WORKER $id files=$count errors=$errs"
}

rm -f /tmp/wl-*.err
for i in $(seq 1 $N); do
  worker $i &
done
wait
echo "DONE N=$N variant=$VARIANT dur=${DUR}s"
echo "--- stderr summary ---"
cat /tmp/wl-*.err 2>/dev/null | sort | uniq -c | sort -rn | head -5
