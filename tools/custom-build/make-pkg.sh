#!/bin/bash
# make-pkg.sh — package the notarized build/Opal.app as a signed, notarized installer: build/Opal-<version>.pkg
# Double-click it on any Mac: removes the original Opal (app to Trash, its camera extension, launchd items, legacy
# DAL plugin, receipt), installs the custom app + its launchd items, activates/updates the camera extension
# (first install: allow it once in System Settings), keeps that Mac's own Opal settings (else defaults), starts Opal.
# Log on the target Mac: /var/log/opal-custom-install.log
#
# Prereqs: build_custom_opal.py --developer-id --profile build/Setup-DeveloperID.provisionprofile, notarize.sh,
# a "Developer ID Installer" certificate, notarytool keychain profile "opal-notary".
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/tools/custom-build"
APP="$ROOT/build/Opal.app"
INSTALLER_ID="Developer ID Installer: Amadeus Demarzi (UHD99KF9X7)"
xcrun stapler validate "$APP" >/dev/null || { echo "build/Opal.app is not notarized (run notarize.sh)"; exit 1; }

VER="$(defaults read "$APP/Contents/Info.plist" CFBundleShortVersionString)-custom.$(date +%Y%m%d%H%M)"
WORK="$ROOT/build/pkgwork"
rm -rf "$WORK"; mkdir -p "$WORK/root/Applications" "$WORK/scripts"
ditto "$APP" "$WORK/root/Applications/Opal.app"
cp "$HERE/pkg/preinstall" "$HERE/pkg/postinstall" "$HERE/pkg/export-oc-settings.js" "$WORK/scripts/"
chmod 755 "$WORK/scripts/preinstall" "$WORK/scripts/postinstall"

# always install to /Applications (never "relocate" onto another copy with the same bundle id, e.g. build/)
pkgbuild --analyze --root "$WORK/root" "$WORK/component.plist" >/dev/null
plutil -replace 0.BundleIsRelocatable -bool NO "$WORK/component.plist"
pkgbuild --root "$WORK/root" --component-plist "$WORK/component.plist" --scripts "$WORK/scripts" \
  --identifier com.amadeus.oc.OpalCamera.pkg --version "$VER" --install-location / "$WORK/component.pkg"
PKG="$ROOT/build/Opal-$VER.pkg"
productbuild --package "$WORK/component.pkg" --sign "$INSTALLER_ID" "$PKG"
xcrun notarytool submit "$PKG" --keychain-profile opal-notary --wait
xcrun stapler staple "$PKG"
spctl -a -vv -t install "$PKG"
rm -rf "$WORK"
echo "package: $PKG"
