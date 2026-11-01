#!/bin/bash
# update-custom-opal.sh — replace the installed custom Opal (/Applications/Opal.app) with a fresh build/Opal.app.
# For updates AFTER install-custom-opal.sh has been run once. Leaves launchd configs and rollback/ alone.
# Run it yourself from Terminal (uses sudo).
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
APP="$ROOT/build/Opal.app"
[ -d "$APP" ] || { echo "no $APP — run build_custom_opal.py first"; exit 1; }
codesign --verify --deep --strict "$APP" || { echo "build is not validly signed"; exit 1; }
codesign -d --entitlements - "$APP/Contents/MacOS/Opal Camera Setup.app" 2>/dev/null | grep -q system-extension.install \
  || { echo "build has no system-extension entitlement (build with --profile build/Setup.provisionprofile)"; exit 1; }
[ "$(defaults read /Applications/Opal.app/Contents/Info.plist CFBundleIdentifier 2>/dev/null)" = com.amadeus.oc.OpalCamera ] \
  || { echo "/Applications/Opal.app is not the custom build — use install-custom-opal.sh"; exit 1; }

echo "This replaces /Applications/Opal.app with $APP (Opal will quit and relaunch)."
read -r -p "Continue? [y/N] " a; [ "$a" = y ] || exit 1
sudo -v
osascript -e 'tell application "Opal" to quit' 2>/dev/null || true
for i in $(seq 20); do pgrep -qx Opal || break; sleep 0.5; done
sudo rm -rf /Applications/Opal.app
sudo ditto "$APP" /Applications/Opal.app
open -a /Applications/Opal.app
echo "Updated. Check: tail ~/Library/Logs/opal-watch/opalspy.log ~/Library/Logs/opal-watch/camera.log"
