#!/bin/bash
# Build opalspy.dylib (injected into / built into Opal's main app).
set -e
cd "$(dirname "$0")"
clang -arch arm64 -dynamiclib -fobjc-arc -framework AppKit -framework CoreMediaIO \
  -install_name @executable_path/../Frameworks/opalspy.dylib \
  -o opalspy.dylib opalspy.m debounce.m launchquiet.m cmio_inuse.m
codesign -s - -f opalspy.dylib
echo built opalspy.dylib
