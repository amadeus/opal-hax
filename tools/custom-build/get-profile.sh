#!/bin/bash
# get-profile.sh — CHANGES YOUR APPLE DEVELOPER ACCOUNT: lets Xcode automatic signing register the App ID
# com.amadeus.oc.OpalExtensionInstaller (System Extension capability) for team UHD99KF9X7, register this Mac
# for development if needed, and create a macOS development provisioning profile. Copies it to
# build/Setup.provisionprofile for build_custom_opal.py --profile.
set -e
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer   # Xcode, without changing xcode-select
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/../.." && pwd)"
DD="$ROOT/build/profile-helper-dd"
xcodebuild -project "$HERE/profile-helper/ProfileHelper.xcodeproj" -target ProfileHelper -configuration Debug \
  -allowProvisioningUpdates -derivedDataPath "$DD" SYMROOT="$DD/sym" build | tail -5
APP=$(find "$DD" -name ProfileHelper.app -type d | head -1)
cp "$APP/Contents/embedded.provisionprofile" "$ROOT/build/Setup.provisionprofile"
security cms -D -i "$ROOT/build/Setup.provisionprofile" | plutil -p - | grep -E "Name|application-identifier|system-extension|ExpirationDate"
echo "profile ready: $ROOT/build/Setup.provisionprofile"
