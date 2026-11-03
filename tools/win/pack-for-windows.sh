#!/bin/bash
# pack-for-windows.sh - on the Mac: make build/opal-windows-src.zip with everything the PC build needs:
#   tools/win (sources + scripts) and Opal's camera assets (firmware, captured pipeline + NN blobs, look LUTs).
# The assets are extracted from Opal.app, so they travel in this zip, not through git.
# Prereqs (once): extract-opal-fw.py, make-template.py <pipecap session>, luts/bake-luts.sh.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
V="$ROOT/tools/vendor"
for p in "$V/opal-fw" "$V/opal-template/full.json" "$V/opal-template/luts/index.json"; do
  [ -e "$p" ] || { echo "missing $p (see tools/win/README.md)"; exit 1; }
done
OUT="$ROOT/build/opal-windows-src.zip"
mkdir -p "$ROOT/build"
rm -f "$OUT"
cd "$ROOT"
zip -qr "$OUT" tools/win tools/vendor/opal-fw tools/vendor/opal-template \
  -x 'tools/win/dist/*' -x '*/.DS_Store' -x 'tools/win/smoke/*'
echo "wrote $OUT ($(du -h "$OUT" | cut -f1))"
echo "On the PC: unzip it anywhere (e.g. C:\\opal), then in PowerShell:"
echo "  powershell -ExecutionPolicy Bypass -File C:\\opal\\tools\\win\\build-win.ps1"
