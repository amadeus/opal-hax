#!/bin/bash
# patch-app.sh <path/to/Opal.app> [--force]
# (usually called by make-patched-app.sh, which first makes a fresh copy: macOS App Management protection
#  blocks modifying an app bundle it already knows about, e.g. one that has been launched)
# Builds opalcore into Opal's camera-driver XPC service (OpalCameraDeviceService):
#   adds LC_LOAD_DYLIB @executable_path/../Frameworks/opalcore.dylib, copies the dylib in, and re-signs ONLY
#   that XPC service ad-hoc with its original entitlements. The main app and other services are untouched.
# Refuses to patch /Applications/Opal.app unless --force is given. Re-running is safe (idempotent).
set -e
APP="${1:?usage: patch-app.sh path/to/Opal.app [--force]}"
APP="$(cd "$APP" && pwd)"
HERE="$(cd "$(dirname "$0")" && pwd)"
case "$APP" in /Applications/*) [ "$2" = "--force" ] || { echo "refusing to patch $APP without --force"; exit 1; } ;; esac
XPC="$APP/Contents/XPCServices/OpalCameraDeviceService.xpc"
BIN="$XPC/Contents/MacOS/OpalCameraDeviceService"
[ -f "$BIN" ] || { echo "not an Opal.app: $BIN missing"; exit 1; }
[ -f "$HERE/opalcore.dylib" ] || "$HERE/build.sh"

ENT=$(mktemp -t opalcore-ent).plist
codesign -d --entitlements - --xml "$XPC" > "$ENT" 2>/dev/null || true
if [ ! -s "$ENT" ]; then echo "could not read entitlements (already ad-hoc without them?)"; cp "$HERE/devsvc-entitlements.plist" "$ENT"; fi
cp "$ENT" "$HERE/devsvc-entitlements.plist"   # keep a copy for re-patching

codesign --remove-signature "$BIN" 2>/dev/null || true
/usr/bin/python3 -I "$HERE/insert_dylib.py" "$BIN" "@executable_path/../Frameworks/opalcore.dylib"
mkdir -p "$XPC/Contents/Frameworks"
cp "$HERE/opalcore.dylib" "$XPC/Contents/Frameworks/opalcore.dylib"
codesign -f -s - "$XPC/Contents/Frameworks/opalcore.dylib"
codesign -f -s - --options runtime --entitlements "$ENT" "$XPC"
rm -f "$ENT"

echo "--- verify"
codesign -v --strict "$XPC" && echo "XPC service signature OK (ad-hoc)"
otool -arch arm64 -L "$BIN" | grep opalcore
codesign -d --entitlements - "$XPC" 2>/dev/null | grep -E "disable-library-validation|dyld" | head -2
echo "patched $APP"
