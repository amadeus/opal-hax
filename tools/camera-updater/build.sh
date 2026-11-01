#!/bin/bash
# Build "Opal Camera Updater.app" (camera firmware updater bundled in Opal.app/Contents/Helpers). It links Opal's
# own libdepthai-core/libusb from Opal.app/Contents/Frameworks at runtime. Requires ../fetch-deps.sh to have run.
set -e
cd "$(dirname "$0")"
V=$PWD/../vendor
FW=/Users/amadeus/Developer/experiments/opal-camera/Opal.app/Contents/Frameworks
I="-I$V/gen -I$V/depthai-core/include -I$V/depthai-core/include/depthai -I$V/depthai-core/shared/depthai-shared/include \
   -I$V/depthai-core/shared/depthai-shared/3rdparty -I$V/depthai-core/shared/depthai-bootloader-shared/include \
   -I$V/json/single_include -I$V/xlink/include -I$V/libnop/include"
APP="build/Opal Camera Updater.app"
rm -rf build; mkdir -p "$APP/Contents/MacOS"
cp Info.plist "$APP/Contents/Info.plist"
clang++ -arch arm64 -std=c++14 -fobjc-arc -DXLINK_USE_MX_ID_NAME -O1 -w \
  -Wno-error=missing-template-arg-list-after-template-kw $I updater.mm \
  -framework AppKit -L"$FW" -ldepthai-core -lusb-1.0.0 \
  -Wl,-rpath,@executable_path/../../../../Frameworks \
  -o "$APP/Contents/MacOS/Opal Camera Updater"
codesign -s - -f "$APP" >/dev/null
echo "built $APP"
