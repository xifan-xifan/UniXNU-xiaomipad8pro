#!/bin/sh
# Boot-window probe: one record every 15 s for 20 min, light enough not to be the thing it measures
# (a 5 s loop with top -l 1 measured 12 s per record and pushed the boot out by ~150 s; top -l 1 also reports 0.0 %CPU).
out=/var/tmp/piano-boot-probe.txt
: > "$out"
i=0
while [ $i -lt 80 ]; do
  e=$(( $(date +%s) - 1795585949 ))
  {
    echo "### elapsed=${e}s load=$(sysctl -n vm.loadavg)"
    echo "cpu: $(top -l 2 | grep 'CPU usage' | tail -1)"
    echo "top: $(top -l 2 -n 4 -o cpu -stats pid,command,cpu | tail -4 | tr '\n' '|')"
  } >> "$out" 2>&1
  sleep 15
  i=$((i + 1))
done
echo "### probe done at ${e}s" >> "$out"