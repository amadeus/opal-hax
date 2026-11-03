#!/bin/bash
# Run Opal's own Metal shaders (from Opal.app) next to CPU transcriptions of our specs; see README.md.
# usage: tools/win/verify/shader-check.sh [Opal.app]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
bin="$(mktemp -d)/shader-check"
clang -O2 -fobjc-arc -Wall -Wno-unused-function -framework Foundation -framework Metal "$here/shader-check.m" -o "$bin"
"$bin" "${1:-$repo/Opal Backup.app}" | grep -v -E '^ |^\}'
