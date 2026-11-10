#!/bin/bash
# Build opalspy.dylib (injected into / built into Opal's main app).
set -e
cd "$(dirname "$0")"
clang -arch arm64 -dynamiclib -fobjc-arc -framework AppKit -framework CoreMediaIO -framework IOKit -framework IOSurface -framework QuartzCore -L/usr/lib/swift -lswiftCore \
  -install_name @executable_path/../Frameworks/opalspy.dylib \
  -o opalspy.dylib opalspy.m debounce.m cmio_inuse.m hidepanels.m launchsetup.m statushud.m firmwareoffer.m extguide.m filterfps.m
codesign -s - -f opalspy.dylib
echo built opalspy.dylib
