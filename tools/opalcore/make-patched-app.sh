#!/bin/bash
# make-patched-app.sh [source=Opal Backup.app] [dest=Opal Patched.app]
# Builds a fresh patched copy of Opal from a pristine source (never modifies the source).
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="${1:-$ROOT/Opal Backup.app}"
DST="${2:-$ROOT/Opal Patched.app}"
codesign -v --strict "$SRC/Contents/XPCServices/OpalCameraDeviceService.xpc" || { echo "source is not pristine: $SRC"; exit 1; }
if [ -e "$DST" ]; then
  rm -rf "$DST" 2>/dev/null || { DST="${DST%.app} $(date +%H%M%S).app"; echo "old copy is protected by macOS; using $DST"; }
fi
ditto --noextattr --noqtn "$SRC" "$DST"
"$(dirname "$0")/patch-app.sh" "$DST"
