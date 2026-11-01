#!/bin/bash
# notarize.sh — send build/Opal.app (built with --developer-id) to Apple's notary service, staple the ticket,
# and check Gatekeeper accepts it. Needs keychain credentials saved once with:
#   xcrun notarytool store-credentials opal-notary --apple-id <Apple ID email> --team-id UHD99KF9X7
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
APP="$ROOT/build/Opal.app"
ZIP="$ROOT/build/Opal-notarize.zip"
codesign -dv --verbose=2 "$APP" 2>&1 | grep -q "Authority=Developer ID Application" \
  || { echo "build/Opal.app is not Developer ID signed (build with --developer-id)"; exit 1; }
rm -f "$ZIP"
ditto -c -k --keepParent "$APP" "$ZIP"
xcrun notarytool submit "$ZIP" --keychain-profile opal-notary --wait
rm -f "$ZIP"
xcrun stapler staple "$APP"
spctl -a -vv -t exec "$APP"
