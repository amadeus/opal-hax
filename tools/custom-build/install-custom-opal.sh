#!/bin/bash
# install-custom-opal.sh — install build/Opal.app (custom, team UHD99KF9X7) in place of the original Opal.
# Run it yourself from Terminal (uses sudo). Everything it replaces is moved to rollback/<timestamp>/;
# undo with rollback-custom-opal.sh.
#
#  1. quit Opal; move /Applications/Opal.app and the original com.opalcamera.* launchd plists to rollback/
#  2. copy build/Opal.app to /Applications
#  3. install launchd configs for the renamed components:
#       /Library/LaunchDaemons/com.amadeus.oc.cameraExtensionShim.plist   (system-wide rendezvous for the camera extension)
#       /Library/LaunchAgents/com.amadeus.oc.OpalCamera.startOnUsbPlugged.agent.plist  (launch Opal on plug-in, PIDs f63b+f63d)
#  4. launch Opal: it asks to activate the new camera extension -> approve in
#     System Settings > General > Login Items & Extensions > Camera Extensions.
#     The old Opal extension is removed by macOS once its app is gone (may need a reboot).
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
APP="$ROOT/build/Opal.app"
TS=$(date +%Y%m%d-%H%M%S)
RB="$ROOT/rollback/$TS"
U=$(id -u)

[ -d "$APP" ] || { echo "no $APP — run build_custom_opal.py first"; exit 1; }
codesign --verify --deep --strict "$APP" || { echo "build is not validly signed"; exit 1; }
codesign -d --entitlements - "$APP/Contents/MacOS/Opal Camera Setup.app" 2>/dev/null | grep -q system-extension.install \
  || { echo "build has no system-extension entitlement (build with --profile build/Setup.provisionprofile)"; exit 1; }

echo "This replaces /Applications/Opal.app with the custom build. Originals go to $RB"
read -r -p "Continue? [y/N] " a; [ "$a" = y ] || exit 1
sudo -v
mkdir -p "$RB/LaunchDaemons" "$RB/LaunchAgents"

# 1. stop and set aside the original
osascript -e 'tell application "Opal" to quit' 2>/dev/null || true
launchctl bootout gui/$U/local.opal-launch 2>/dev/null || true
for f in /Library/LaunchAgents/com.opalcamera.*.plist; do
  [ -e "$f" ] || continue
  launchctl bootout gui/$U "$f" 2>/dev/null || true
  sudo mv "$f" "$RB/LaunchAgents/"
done
for f in /Library/LaunchDaemons/com.opalcamera.*.plist; do
  [ -e "$f" ] || continue
  sudo launchctl bootout system "$f" 2>/dev/null || true
  sudo mv "$f" "$RB/LaunchDaemons/"
done
sleep 2
[ -d /Applications/Opal.app ] && sudo mv /Applications/Opal.app "$RB/Opal.app"

# 2. install the custom app
sudo ditto "$APP" /Applications/Opal.app

# 3. launchd configs for the renamed components
SHIM=/Applications/Opal.app/Contents/Library/LaunchServices/com.amadeus.oc.cameraExtensionShim
TMP=$(mktemp -d)
cat > "$TMP/com.amadeus.oc.cameraExtensionShim.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>com.amadeus.oc.cameraExtensionShim</string>
  <key>Program</key><string>$SHIM</string>
  <key>ProgramArguments</key><array><string>$SHIM</string></array>
  <key>MachServices</key><dict><key>com.amadeus.oc.cameraExtensionShim</key><true/></dict>
</dict></plist>
PLIST
USBH=/Applications/Opal.app/Contents/Library/USBItems/OpalCameraStartOnUsbPlugged.app/Contents/MacOS/OpalCameraStartOnUsbPlugged
cat > "$TMP/com.amadeus.oc.OpalCamera.startOnUsbPlugged.agent.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>com.amadeus.oc.OpalCamera.startOnUsbPlugged.agent</string>
  <key>ProgramArguments</key><array><string>$USBH</string></array>
  <key>LaunchEvents</key><dict><key>com.apple.iokit.matching</key><dict>
    <key>com.apple.device-attach</key><dict>
      <key>IOProviderClass</key><string>IOUSBDevice</string><key>IOMatchLaunchStream</key><true/>
      <key>idVendor</key><integer>999</integer><key>idProduct</key><integer>63035</integer></dict>
    <key>com.apple.device-attach-f63d</key><dict>
      <key>IOProviderClass</key><string>IOUSBDevice</string><key>IOMatchLaunchStream</key><true/>
      <key>idVendor</key><integer>999</integer><key>idProduct</key><integer>63037</integer></dict>
  </dict></dict>
</dict></plist>
PLIST
sudo install -m 644 -o root -g wheel "$TMP/com.amadeus.oc.cameraExtensionShim.plist" /Library/LaunchDaemons/
sudo install -m 644 -o root -g wheel "$TMP/com.amadeus.oc.OpalCamera.startOnUsbPlugged.agent.plist" /Library/LaunchAgents/
rm -rf "$TMP"
sudo launchctl bootstrap system /Library/LaunchDaemons/com.amadeus.oc.cameraExtensionShim.plist
launchctl bootstrap gui/$U /Library/LaunchAgents/com.amadeus.oc.OpalCamera.startOnUsbPlugged.agent.plist

# 4. launch
open -a /Applications/Opal.app
echo
echo "Installed. If macOS asks, allow the 'Opal C1' camera extension in System Settings >"
echo "General > Login Items & Extensions > Camera Extensions. Originals saved in $RB"
echo "Check: systemextensionsctl list | grep -i opal ; tools/opal-watch/opal-watch-report 1"
