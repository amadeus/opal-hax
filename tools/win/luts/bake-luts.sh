#!/bin/bash
# Bake Opal's 13 looks into .cube LUTs: tools/vendor/opal-template/luts/{<name>.cube,index.json}
# usage: tools/win/luts/bake-luts.sh [Opal.app] [lut size, default 64]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
app="${1:-$repo/Opal Backup.app}"
size="${2:-64}"
out="$repo/tools/vendor/opal-template/luts"
bin="$(mktemp -d)/bake-luts"
mkdir -p "$out"
clang -O2 -fobjc-arc -Wall -Wno-unused-function -framework Foundation -framework Metal -framework CoreImage \
  -framework CoreGraphics "$here/bake-luts.m" -o "$bin"
"$bin" --app "$app" --out "$out" --size "$size"
