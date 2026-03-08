#!/bin/bash

image=$(sudo docker ps --filter ancestor=reffs-dev --format '{{.Names}}')

sudo docker exec "$image" sh -c '
pid=$(for p in /proc/[0-9]*; do
    cmdline=$(cat $p/cmdline 2>/dev/null | tr "\0" " ")
    echo "$cmdline" | grep -q reffsd && echo $(basename $p)
done | grep -v "^1$" | head -1)

echo "pid=$pid"

for tid in /proc/$pid/task/*/; do
  tid_num=$(basename $tid)
  wchan=$(cat $tid/wchan 2>/dev/null)
  comm=$(cat $tid/comm 2>/dev/null)
  echo "tid=$tid_num comm=$comm wchan=$wchan"
done

echo "---stacks---"

for tid in /proc/$pid/task/*/; do
  tid_num=$(basename $tid)
  echo "=== tid $tid_num ==="
  cat $tid/stack 2>/dev/null
done
'
