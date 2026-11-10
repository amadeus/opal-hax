#!/bin/bash
# build-windows-on-mac.sh - cross-compile Opal for Windows on this Mac (MinGW-w64) and package it:
#   On the PC: unzip, run InstallOpal.exe, click Install (the app installs itself as Opal.exe; no scripts, which
#   antivirus flags as droppers).
# Prereqs: brew install mingw-w64 ninja cmake; Opal assets in tools/vendor (extract-opal-fw.py, make-template.py,
# luts/bake-luts.sh); depthai source from build-depthai.sh (tools/vendor/depthai-core-opal, patched).
# The PC needs nothing but Windows 11: Opal.exe links the GCC runtime statically and only uses system DLLs.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
TOOLS="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$TOOLS/.." && pwd)"
V="$TOOLS/vendor"
TC="$HERE/cmake/mingw-w64-x86_64.cmake"
DAI="$V/depthai-build-mingw"
APP="$V/opal-win-mingw"
export CMAKE_POLICY_VERSION_MINIMUM=3.5
command -v x86_64-w64-mingw32-g++ >/dev/null || { echo "brew install mingw-w64"; exit 1; }
DEV=$(ls "$V"/opal-fw/depthai-device-fwp-*.tar.xz | head -1)
BL=$(ls "$V"/opal-fw/depthai-bootloader-fwp-*.tar.xz | head -1)
for p in "$DEV" "$BL" "$V/opal-template/full.json" "$V/opal-template/luts/index.json" "$V/depthai-core-opal"; do
  [ -e "$p" ] || { echo "missing $p (see tools/win/README.md)"; exit 1; }
done
grep -q "opal-win: restored" "$V/depthai-core-opal/src/pipeline/datatype/StreamMessageParser.cpp" \
  || { echo "depthai source not patched: run tools/win/build-depthai.sh once"; exit 1; }
DEV_HASH=$(basename "$DEV" | sed -E 's/.*fwp-([0-9a-f]{40}).*/\1/')
BL_HASH=$(basename "$BL" | sed -E 's/.*fwp-([0-9a-f]{40}).*/\1/')

echo "==> XLink with the composite-device fix (tools/win/xlink/patch-xlink.py)"
XL="$V/xlink-opal"
if [ ! -f "$XL/CMakeLists.txt" ]; then
  mkdir -p "$XL"
  curl -sfL https://github.com/luxonis/XLink/archive/b7d8e6009ceb5b2278212e3f2a57f5f76e8df5d9.tar.gz | tar xz -C "$XL" --strip-components 1
fi
python3 "$HERE/xlink/patch-xlink.py" "$XL"

echo "==> depthai-core for Windows (static, Opal firmware embedded)"
if [ ! -f "$DAI/install/lib/libdepthai-core.a" ] || [ "$XL/src/pc/Win/src/win_usb.c" -nt "$DAI/install/lib/libdepthai-core.a" ]; then
  mkdir -p "$DAI/resources"
  cp "$DEV" "$BL" "$DAI/resources/"
  git -C "$V/depthai-core-opal/shared/depthai-shared" rev-parse HEAD > "$DAI/resources/depthai-shared-commit-hash-$DEV_HASH.txt"
  git -C "$V/depthai-core-opal/shared/depthai-bootloader-shared" rev-parse HEAD > "$DAI/resources/depthai-bootloader-shared-commit-hash-$BL_HASH.txt"
  [ -f "$DAI/CMakeCache.txt" ] || cmake -S "$V/depthai-core-opal" -B "$DAI" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DDEPTHAI_OPENCV_SUPPORT=OFF -DDEPTHAI_ENABLE_BACKWARD=OFF \
    -DDEPTHAI_BUILD_EXAMPLES=OFF -DDEPTHAI_BUILD_TESTS=OFF -DDEPTHAI_CLANG_FORMAT=OFF -DCMAKE_INSTALL_PREFIX="$DAI/install" \
    -DDEPTHAI_XLINK_LOCAL="$XL"
  cmake --build "$DAI"
  cmake --install "$DAI"
fi

echo "==> Opal for Windows"
[ -f "$APP/CMakeCache.txt" ] || cmake -S "$HERE" -B "$APP" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release \
  -Ddepthai_DIR="$DAI/install/lib/cmake/depthai"
cmake --build "$APP"

echo "==> converting looks to the binary LUT format"
python3 "$HERE/luts/cube-to-lut.py" "$HERE/dist/resources/opal-template/luts"

echo "==> checking runtime dependencies"
BAD=$(for f in "$HERE/dist/Opal.exe" "$HERE/dist/OpalVirtualCamera.dll"; do
  x86_64-w64-mingw32-objdump -p "$f" | sed -n 's/.*DLL Name: //p'; done | sort -u |
  grep -viE '^(api-ms-win-.*|kernel32|user32|gdi32|shell32|advapi32|ole32|oleaut32|comdlg32|d2d1|d3d11|dxgi|d3dcompiler_47|dwmapi|dwrite|iphlpapi|mfplat|setupapi|winusb|ws2_32|windowscodecs|cfgmgr32)\.dll$' || true)
[ -z "$BAD" ] || { echo "non-system DLLs needed: $BAD"; exit 1; }

OUT="$ROOT/build/Opal-Windows-$(date +%Y%m%d-%H%M).zip"
mkdir -p "$ROOT/build"
rm -rf "$V/opal-win-pkg" && mkdir -p "$V/opal-win-pkg" && cp -R "$HERE/dist" "$V/opal-win-pkg/Opal for Windows"
# in the download the app is the installer (installer.cpp installs it as Opal.exe)
mv "$V/opal-win-pkg/Opal for Windows/Opal.exe" "$V/opal-win-pkg/Opal for Windows/InstallOpal.exe"
(cd "$V/opal-win-pkg" && zip -qr "$OUT" "Opal for Windows" -x '*/.DS_Store')
echo "wrote $OUT ($(du -h "$OUT" | cut -f1))"

# diagnostic: OpalWebcamTest.exe + the camera tuning blob (an Opal asset: local only, never in git)
T="$V/opal-win-pkg/OpalWebcamTest"
mkdir -p "$T"
cp "$APP/OpalWebcamTest.exe" "$T/"
cp "$V/opal-template/assets/8c723046f982093b473345ebf02d38a652995b78bd985a0cc224a6cd3a0d22e1.bin" "$T/Opal_v0.12.bin"
TOUT="$ROOT/build/OpalWebcamTest-$(date +%Y%m%d-%H%M).zip"
(cd "$V/opal-win-pkg" && zip -qr "$TOUT" OpalWebcamTest)
echo "wrote $TOUT"
