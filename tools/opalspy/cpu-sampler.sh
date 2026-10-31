#!/bin/bash
# cpu-sampler.sh [seconds=1200] — samples CPU% / RSS of Opal processes every 5 s into ~/Library/Logs/opal-watch/cpu.log
END=$(( $(date +%s) + ${1:-1200} ))
OUT="$HOME/Library/Logs/opal-watch/cpu.log"
while [ $(date +%s) -lt $END ]; do
  line=$(ps -axo %cpu=,rss=,comm= | awk '/Opal.app\/Contents\/MacOS\/Opal$|OpalCameraDeviceService$|OpalCameraVideoService$|opalCameraExtension$|cameraExtensionShim$/ {n=$3; sub(/.*\//,"",n); printf "%s=%.1f%%/%dMB ", n, $1, $2/1024}')
  echo "$(date '+%Y-%m-%d %H:%M:%S') $line" >> "$OUT"
  sleep 5
done
