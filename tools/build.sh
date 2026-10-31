#!/bin/bash
# usage: ./build.sh <src.cpp> <out> [extra linker args]
# Builds against Opal.app's own libdepthai-core.dylib (arm64). Run ./fetch-deps.sh first.
set -e
cd "$(dirname "$0")"
V=$PWD/vendor
FW=/Users/amadeus/Developer/experiments/opal-camera/Opal.app/Contents/Frameworks
I="-I$V/gen -I$V/depthai-core/include -I$V/depthai-core/include/depthai -I$V/depthai-core/shared/depthai-shared/include \
   -I$V/depthai-core/shared/depthai-shared/3rdparty -I$V/depthai-core/shared/depthai-bootloader-shared/include \
   -I$V/json/single_include -I$V/xlink/include -I$V/libnop/include"
# XLINK_USE_MX_ID_NAME: Opal's build uses 48-byte device names (verified: sizeof(dai::Device)==0x2b8, DeviceInfo==0x3c)
clang++ -std=c++14 -DXLINK_USE_MX_ID_NAME -O1 -w -Wno-error=missing-template-arg-list-after-template-kw -arch arm64 $I \
  "$1" -o "$2" -L"$FW" -ldepthai-core -lusb-1.0.0 -Wl,-rpath,"$FW" "${@:3}"
