#!/bin/bash
# Build opalcore.dylib (injected into OpalCameraDeviceService). Requires ../fetch-deps.sh to have run.
set -e
cd "$(dirname "$0")"
V=$PWD/../vendor
FW=/Users/amadeus/Developer/experiments/opal-camera/Opal.app/Contents/Frameworks
I="-I$V/gen -I$V/depthai-core/include -I$V/depthai-core/include/depthai -I$V/depthai-core/shared/depthai-shared/include \
   -I$V/depthai-core/shared/depthai-shared/3rdparty -I$V/depthai-core/shared/depthai-bootloader-shared/include \
   -I$V/json/single_include -I$V/xlink/include -I$V/libnop/include"
clang -arch arm64 -fobjc-arc -O1 -c cmio_inuse.m -o cmio_inuse.o
clang++ -arch arm64 -std=c++14 -fobjc-arc -DXLINK_USE_MX_ID_NAME -O1 -w \
  -Wno-error=missing-template-arg-list-after-template-kw $I -c opalcore.mm -o opalcore.o
clang++ -arch arm64 -dynamiclib -fobjc-arc opalcore.o cmio_inuse.o \
  -framework Foundation -framework CoreMediaIO -L"$FW" -ldepthai-core \
  -install_name @executable_path/../Frameworks/opalcore.dylib \
  -Wl,-rpath,@loader_path/../../../../Frameworks \
  -o opalcore.dylib
rm -f cmio_inuse.o opalcore.o
codesign -s - -f opalcore.dylib
echo built opalcore.dylib
