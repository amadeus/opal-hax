#!/bin/bash
# Install (or remove) local.opal-launch: starts Opal with the opalspy telemetry recorder at login and
# whenever the Opal C1 attaches (both USB PIDs f63b and f63d). Disables Opal's own USB-plug agent
# (per-user override, no sudo) so it can't start a recorder-less Opal first.
#   ./install.sh            install
#   ./install.sh uninstall  remove and re-enable Opal's original agent
set -e
U=$(id -u)
HERE="$(cd "$(dirname "$0")" && pwd)"
ORIG=com.opalcamera.OpalCamera.startOnUsbPlugged.agent
if [ "$1" = "uninstall" ]; then
  launchctl bootout gui/$U/local.opal-launch 2>/dev/null || true
  rm -f ~/Library/LaunchAgents/local.opal-launch.plist
  launchctl enable gui/$U/$ORIG
  launchctl bootstrap gui/$U /Library/LaunchAgents/$ORIG.plist 2>/dev/null || true
  echo "removed local.opal-launch; Opal's original USB agent re-enabled"
  exit 0
fi
mkdir -p ~/Library/Logs/opal-watch
launchctl bootout gui/$U/$ORIG 2>/dev/null || true
launchctl disable gui/$U/$ORIG
cp "$HERE/local.opal-launch.plist" ~/Library/LaunchAgents/local.opal-launch.plist
launchctl bootout gui/$U/local.opal-launch 2>/dev/null || true
launchctl bootstrap gui/$U ~/Library/LaunchAgents/local.opal-launch.plist
echo "installed local.opal-launch (Opal's original agent disabled)."
echo "Restart Opal with the recorder now: osascript -e 'tell application \"Opal\" to quit'  (it will be relaunched)"
