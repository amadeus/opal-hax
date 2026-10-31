#!/bin/bash
# rollback-custom-opal.sh [rollback/<timestamp>] — undo install-custom-opal.sh: remove the custom build and its
# launchd configs, restore the original Opal.app and com.opalcamera.* plists (latest rollback dir by default).
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
RB="${1:-$(ls -d "$ROOT"/rollback/* 2>/dev/null | sort | tail -1)}"
[ -d "$RB" ] || { echo "no rollback dir found"; exit 1; }
U=$(id -u)
echo "Restoring the original Opal from $RB"
read -r -p "Continue? [y/N] " a; [ "$a" = y ] || exit 1
sudo -v
osascript -e 'tell application "Opal" to quit' 2>/dev/null || true
launchctl bootout gui/$U /Library/LaunchAgents/com.amadeus.oc.OpalCamera.startOnUsbPlugged.agent.plist 2>/dev/null || true
sudo launchctl bootout system /Library/LaunchDaemons/com.amadeus.oc.cameraExtensionShim.plist 2>/dev/null || true
sudo rm -f /Library/LaunchAgents/com.amadeus.oc.*.plist /Library/LaunchDaemons/com.amadeus.oc.*.plist
sleep 2
sudo rm -rf /Applications/Opal.app
if [ -d "$RB/Opal.app" ]; then sudo mv "$RB/Opal.app" /Applications/Opal.app
else sudo ditto "$ROOT/Opal Backup.app" /Applications/Opal.app; fi
for f in "$RB"/LaunchDaemons/*.plist; do [ -e "$f" ] && sudo mv "$f" /Library/LaunchDaemons/ && sudo launchctl bootstrap system "/Library/LaunchDaemons/$(basename "$f")" || true; done
for f in "$RB"/LaunchAgents/*.plist; do [ -e "$f" ] && sudo mv "$f" /Library/LaunchAgents/ && launchctl bootstrap gui/$U "/Library/LaunchAgents/$(basename "$f")" || true; done
open -a /Applications/Opal.app
echo "Original Opal restored. The custom 'Opal C1' extension is removed by macOS once its app is gone (may need a reboot)."
